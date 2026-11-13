#include "launcher/discord_friends.h"

#include "platform/path.h"
#include "platform/profile_service.h"
#include "platform/session_log.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <deque>
#include <optional>
#include <system_error>
#include <unordered_set>

#if MSCHARGED_HAS_DISCORD_SOCIAL_SDK
#include <SDL3/SDL_loadso.h>
#include <SDL3/SDL_error.h>
#include <cdiscord.h>
#endif

namespace mscharged::launcher
{
using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;

const char* DiscordFriends::LibraryName()
{
#if defined(_WIN32)
    return "discord_partner_sdk.dll";
#elif defined(__APPLE__)
    return "libdiscord_partner_sdk.dylib";
#else
    return "libdiscord_partner_sdk.so";
#endif
}

// What the launcher's thread and the SDK's thread share.
struct DiscordFriends::Inbox
{
    enum class Command
    {
        Connect,
        Cancel,
        Disconnect,
    };

    std::mutex mutex;
    std::condition_variable wake;
    bool stop = false;
    Status status;
    Account account; // the one SetAccount wants
    std::deque<Command> commands;
    // Trade results, with the trade they answer (Worker::trade_).
    std::deque<std::pair<std::uint64_t, TradeResult>> results;

    void Change(const std::function<void(Status&)>& change)
    {
        std::lock_guard<std::mutex> lock(mutex);
        change(status);
        ++status.serial;
    }
};

#if MSCHARGED_HAS_DISCORD_SOCIAL_SDK
namespace
{
// How often the thread lets the SDK run its callbacks while it is in use.
constexpr auto kTick = 50ms;
// The friends are read again at most this often after a change.
constexpr auto kFriendsEvery = 500ms;
// A refresh that failed (not refused) is tried again after this.
constexpr auto kRetry = 60s;

std::string Lower(std::string text)
{
    for (auto& c : text) c = char(std::tolower(static_cast<unsigned char>(c)));
    return text;
}
} // namespace

// The C functions of the SDK this uses, found in the library at run time.
#define MSCHARGED_DISCORD_FUNCTIONS(X)                                                                         \
    X(Discord_RunCallbacks)                                                                                    \
    X(Discord_Free)                                                                                            \
    X(Discord_Client_Init)                                                                                     \
    X(Discord_Client_Drop)                                                                                     \
    X(Discord_Client_SetApplicationId)                                                                         \
    X(Discord_Client_AddLogCallback)                                                                           \
    X(Discord_Client_SetStatusChangedCallback)                                                                 \
    X(Discord_Client_Connect)                                                                                  \
    X(Discord_Client_Disconnect)                                                                               \
    X(Discord_Client_UpdateToken)                                                                              \
    X(Discord_Client_Authorize)                                                                                \
    X(Discord_Client_AbortAuthorize)                                                                           \
    X(Discord_Client_CreateAuthorizationCodeVerifier)                                                          \
    X(Discord_Client_GetDefaultPresenceScopes)                                                                 \
    X(Discord_Client_GetRelationships)                                                                         \
    X(Discord_Client_GetRelationshipsByGroup)                                                                  \
    X(Discord_Client_SetRelationshipCreatedCallback)                                                           \
    X(Discord_Client_SetRelationshipDeletedCallback)                                                           \
    X(Discord_Client_SetRelationshipGroupsUpdatedCallback)                                                     \
    X(Discord_Client_SetUserUpdatedCallback)                                                                   \
    X(Discord_Client_ErrorToString)                                                                            \
    X(Discord_Client_GetVersionMajor)                                                                          \
    X(Discord_Client_GetVersionMinor)                                                                          \
    X(Discord_Client_GetVersionPatch)                                                                          \
    X(Discord_AuthorizationArgs_Init)                                                                          \
    X(Discord_AuthorizationArgs_Drop)                                                                          \
    X(Discord_AuthorizationArgs_SetClientId)                                                                   \
    X(Discord_AuthorizationArgs_SetScopes)                                                                     \
    X(Discord_AuthorizationArgs_SetCodeChallenge)                                                              \
    X(Discord_AuthorizationCodeVerifier_Drop)                                                                  \
    X(Discord_AuthorizationCodeVerifier_Challenge)                                                             \
    X(Discord_AuthorizationCodeVerifier_Verifier)                                                              \
    X(Discord_AuthorizationCodeChallenge_Drop)                                                                 \
    X(Discord_ClientResult_Drop)                                                                               \
    X(Discord_ClientResult_Successful)                                                                         \
    X(Discord_ClientResult_Error)                                                                              \
    X(Discord_RelationshipHandle_Drop)                                                                         \
    X(Discord_RelationshipHandle_DiscordRelationshipType)                                                      \
    X(Discord_RelationshipHandle_Id)                                                                           \
    X(Discord_RelationshipHandle_User)                                                                         \
    X(Discord_UserHandle_Drop)                                                                                 \
    X(Discord_UserHandle_DisplayName)                                                                          \
    X(Discord_UserHandle_Status)

struct DiscordFriends::Sdk
{
#define MSCHARGED_DISCORD_POINTER(name) decltype(&::name) name = nullptr;
    MSCHARGED_DISCORD_FUNCTIONS(MSCHARGED_DISCORD_POINTER)
#undef MSCHARGED_DISCORD_POINTER

    // Loads the library and finds every function; the library stays loaded
    // until the program ends, as threads it started may outlive its client.
    bool Load(const std::filesystem::path& file, std::string& error)
    {
        SDL_SharedObject* library = SDL_LoadObject(PathUtf8(file).c_str());
        if (!library)
        {
            error = SDL_GetError();
            return false;
        }
#define MSCHARGED_DISCORD_FIND(name)                                                                           \
    name = reinterpret_cast<decltype(name)>(SDL_LoadFunction(library, #name));                                 \
    if (!name)                                                                                                 \
    {                                                                                                          \
        error = "it has no " #name;                                                                            \
        return false;                                                                                          \
    }
        MSCHARGED_DISCORD_FUNCTIONS(MSCHARGED_DISCORD_FIND)
#undef MSCHARGED_DISCORD_FIND
        return true;
    }

    // Text the SDK returns, which it leaves to the caller to free.
    std::string Take(Discord_String text) const
    {
        std::string result(reinterpret_cast<const char*>(text.ptr), text.size);
        Discord_Free(text.ptr);
        return result;
    }

    static Discord_String View(const std::string& text)
    {
        return {reinterpret_cast<std::uint8_t*>(const_cast<char*>(text.data())), text.size()};
    }
};

// The SDK's side, on the thread only: the client, the link's progress and
// the friends.
struct DiscordFriends::Worker
{
    Worker(DiscordFriends& owner, Inbox& inbox) : owner(owner), inbox(inbox) {}

    DiscordFriends& owner;
    Inbox& inbox;
    Sdk sdk;
    bool loaded = false;
    std::optional<Discord_Client> client;
    Account account;
    std::string verifier;          // the running authorization's
    std::uint64_t trade = 0;       // the trade whose result is awaited (0: none)
    bool friends_dirty = false;
    Clock::time_point friends_at{};
    std::optional<Clock::time_point> retry_at;

    static void NoFree(void*) {}

    static std::string ResultError(const Sdk& sdk, Discord_ClientResult* result)
    {
        Discord_String text{};
        sdk.Discord_ClientResult_Error(result, &text);
        return sdk.Take(text);
    }

    void SetState(State state, std::string error = {})
    {
        inbox.Change([&](Status& status) {
            status.state = state;
            if (!error.empty() && status.error != error) ++status.error_serial;
            status.error = std::move(error);
            if (state != State::Ready) status.friends.clear();
        });
    }

    void Fail(const std::string& error)
    {
        platform::SessionLogLine("Discord friends: " + error);
        SetState(State::Failed, error);
    }

    // The library and a client, once; false (Missing) when it does not load.
    bool EnsureClient()
    {
        if (!loaded)
        {
            std::string error;
            const auto file = owner.library_;
            if (!sdk.Load(file, error))
            {
                platform::SessionLogLine("Discord friends: off (" + PathUtf8(file.filename()) + ": " + error
                                         + ")");
                SetState(State::Missing, "The Discord library does not load: " + error);
                return false;
            }
            loaded = true;
            platform::SessionLogLine("Discord friends: Discord Social SDK "
                                     + std::to_string(sdk.Discord_Client_GetVersionMajor()) + "."
                                     + std::to_string(sdk.Discord_Client_GetVersionMinor()) + "."
                                     + std::to_string(sdk.Discord_Client_GetVersionPatch()));
        }
        if (client) return true;
        client.emplace();
        sdk.Discord_Client_Init(&*client);
        sdk.Discord_Client_AddLogCallback(
            &*client,
            [](Discord_String message, Discord_LoggingSeverity, void* data) {
                auto* self = static_cast<Worker*>(data);
                platform::SessionLogLine("Discord SDK: " + self->sdk.Take(message));
            },
            NoFree, this, Discord_LoggingSeverity_Warning);
        sdk.Discord_Client_SetStatusChangedCallback(
            &*client,
            [](Discord_Client_Status status, Discord_Client_Error error, int32_t detail, void* data) {
                static_cast<Worker*>(data)->StatusChanged(status, error, detail);
            },
            NoFree, this);
        const auto dirty = [](std::uint64_t, bool, void* data) { static_cast<Worker*>(data)->friends_dirty = true; };
        const auto dirty_user = [](std::uint64_t, void* data) { static_cast<Worker*>(data)->friends_dirty = true; };
        sdk.Discord_Client_SetRelationshipCreatedCallback(&*client, dirty, NoFree, this);
        sdk.Discord_Client_SetRelationshipDeletedCallback(&*client, dirty, NoFree, this);
        sdk.Discord_Client_SetRelationshipGroupsUpdatedCallback(&*client, dirty_user, NoFree, this);
        sdk.Discord_Client_SetUserUpdatedCallback(&*client, dirty_user, NoFree, this);
        return true;
    }

    void DropClient()
    {
        if (!client) return;
        sdk.Discord_Client_Disconnect(&*client);
        sdk.Discord_Client_Drop(&*client);
        client.reset();
        verifier.clear();
    }

    void StatusChanged(Discord_Client_Status status, Discord_Client_Error error, int32_t detail)
    {
        if (status == Discord_Client_Status_Ready)
        {
            platform::SessionLogLine("Discord friends: connected");
            SetState(State::Ready);
            friends_dirty = true;
        }
        else if (status == Discord_Client_Status_Disconnected && error != Discord_Client_Error_None)
        {
            Discord_String text{};
            sdk.Discord_Client_ErrorToString(error, &text);
            Fail("Discord's connection failed (" + sdk.Take(text) + ", " + std::to_string(detail) + ")");
            retry_at = Clock::now() + kRetry;
        }
    }

    void Trade(TradeRequest request)
    {
        const std::uint64_t id = ++trade;
        std::weak_ptr<Inbox> weak = owner.inbox_;
        owner.trade_(std::move(request), [weak, id](TradeResult result) {
            if (const auto inbox = weak.lock())
            {
                {
                    std::lock_guard<std::mutex> lock(inbox->mutex);
                    inbox->results.emplace_back(id, std::move(result));
                }
                inbox->wake.notify_all();
            }
        });
    }

    // A new account (or none): the link of this server, if one is kept.
    void Switch(const Account& wanted)
    {
        DropClient();
        trade = 0;
        retry_at.reset();
        account = wanted;
        if (account.server.empty())
        {
            SetState(State::Off);
            return;
        }
        Refresh();
    }

    void Refresh()
    {
        retry_at.reset();
        const auto link = platform::LoadDiscordLink(owner.link_file_);
        if (!link || link->server != account.server)
        {
            SetState(State::NotLinked);
            return;
        }
        SetState(State::Connecting);
        Trade({{}, {}, {}, link->refresh_token});
    }

    // Discord's authorization in the Discord app or the browser, which comes
    // back to the SDK's local web server with a code.
    void Authorize()
    {
        if (account.server.empty() || !EnsureClient()) return;
        sdk.Discord_Client_SetApplicationId(&*client, account.application);
        Discord_AuthorizationCodeVerifier code_verifier{};
        sdk.Discord_Client_CreateAuthorizationCodeVerifier(&*client, &code_verifier);
        Discord_String text{};
        sdk.Discord_AuthorizationCodeVerifier_Verifier(&code_verifier, &text);
        verifier = sdk.Take(text);
        Discord_AuthorizationCodeChallenge challenge{};
        sdk.Discord_AuthorizationCodeVerifier_Challenge(&code_verifier, &challenge);
        sdk.Discord_Client_GetDefaultPresenceScopes(&text);
        const std::string scopes = sdk.Take(text);
        Discord_AuthorizationArgs args{};
        sdk.Discord_AuthorizationArgs_Init(&args);
        sdk.Discord_AuthorizationArgs_SetClientId(&args, account.application);
        sdk.Discord_AuthorizationArgs_SetScopes(&args, Sdk::View(scopes));
        sdk.Discord_AuthorizationArgs_SetCodeChallenge(&args, &challenge);
        SetState(State::Linking);
        sdk.Discord_Client_Authorize(
            &*client, &args,
            [](Discord_ClientResult* result, Discord_String code, Discord_String redirect, void* data) {
                auto* self = static_cast<Worker*>(data);
                const auto& sdk = self->sdk;
                const bool ok = sdk.Discord_ClientResult_Successful(result);
                const std::string error = ok ? std::string() : ResultError(sdk, result);
                sdk.Discord_ClientResult_Drop(result);
                std::string code_text = sdk.Take(code), redirect_text = sdk.Take(redirect);
                self->Authorized(ok, error, std::move(code_text), std::move(redirect_text));
            },
            NoFree, this);
        sdk.Discord_AuthorizationArgs_Drop(&args);
        sdk.Discord_AuthorizationCodeChallenge_Drop(&challenge);
        sdk.Discord_AuthorizationCodeVerifier_Drop(&code_verifier);
    }

    void Authorized(bool ok, const std::string& error, std::string code, std::string redirect)
    {
        if (StateNow() != State::Linking) return; // cancelled meanwhile
        if (!ok)
        {
            Fail(error.empty() ? "Discord did not link." : "Discord did not link: " + error);
            return;
        }
        SetState(State::Connecting);
        Trade({std::move(code), verifier, std::move(redirect), {}});
    }

    State StateNow()
    {
        std::lock_guard<std::mutex> lock(inbox.mutex);
        return inbox.status.state;
    }

    // The server's answer: the tokens go to the SDK, which then connects.
    void Traded(TradeResult result)
    {
        trade = 0;
        if (!result.error.empty())
        {
            if (result.refused)
            {
                // Discord ended the link (or the code was used): made anew.
                platform::ForgetProfileSession(owner.link_file_);
                DropClient();
                SetState(State::NotLinked, "Discord ended the link. Connect Discord again.");
            }
            else
            {
                Fail(result.error);
                retry_at = Clock::now() + kRetry;
            }
            return;
        }
        try
        {
            platform::SaveDiscordLink(owner.link_file_, {account.server, result.refresh_token});
        }
        catch (const std::exception& e)
        {
            platform::SessionLogLine(std::string("Discord friends: the link is not kept: ") + e.what());
        }
        if (!EnsureClient()) return;
        sdk.Discord_Client_SetApplicationId(&*client, account.application);
        sdk.Discord_Client_UpdateToken(
            &*client, Discord_AuthorizationTokenType_Bearer, Sdk::View(result.access_token),
            [](Discord_ClientResult* outcome, void* data) {
                auto* self = static_cast<Worker*>(data);
                const auto& sdk = self->sdk;
                const bool ok = sdk.Discord_ClientResult_Successful(outcome);
                const std::string error = ok ? std::string() : ResultError(sdk, outcome);
                sdk.Discord_ClientResult_Drop(outcome);
                if (!self->client) return;
                if (ok) sdk.Discord_Client_Connect(&*self->client);
                else self->Fail("Discord did not take the sign-in: " + error);
            },
            NoFree, this);
    }

    // The friends list, as the SDK has it now.
    void ReadFriends()
    {
        friends_dirty = false;
        friends_at = Clock::now();
        std::unordered_set<std::uint64_t> playing;
        Discord_RelationshipHandleSpan span{};
        sdk.Discord_Client_GetRelationshipsByGroup(&*client, Discord_RelationshipGroupType_OnlinePlayingGame, &span);
        for (std::size_t i = 0; i < span.size; ++i)
        {
            playing.insert(sdk.Discord_RelationshipHandle_Id(&span.ptr[i]));
            sdk.Discord_RelationshipHandle_Drop(&span.ptr[i]);
        }
        sdk.Discord_Free(span.ptr);

        std::vector<Friend> friends;
        span = {};
        sdk.Discord_Client_GetRelationships(&*client, &span);
        for (std::size_t i = 0; i < span.size; ++i)
        {
            auto* relationship = &span.ptr[i];
            Discord_UserHandle user{};
            if (sdk.Discord_RelationshipHandle_DiscordRelationshipType(relationship) == Discord_RelationshipType_Friend
                && sdk.Discord_RelationshipHandle_User(relationship, &user))
            {
                Friend entry;
                const auto id = sdk.Discord_RelationshipHandle_Id(relationship);
                entry.id = std::to_string(id);
                Discord_String name{};
                sdk.Discord_UserHandle_DisplayName(&user, &name);
                entry.name = sdk.Take(name);
                switch (sdk.Discord_UserHandle_Status(&user))
                {
                case Discord_StatusType_Online:
                case Discord_StatusType_Streaming: entry.presence = Presence::Online; break;
                case Discord_StatusType_Idle: entry.presence = Presence::Idle; break;
                case Discord_StatusType_Dnd: entry.presence = Presence::DoNotDisturb; break;
                default: entry.presence = Presence::Offline; break;
                }
                entry.in_recharged = playing.count(id) != 0;
                friends.push_back(std::move(entry));
                sdk.Discord_UserHandle_Drop(&user);
            }
            sdk.Discord_RelationshipHandle_Drop(relationship);
        }
        sdk.Discord_Free(span.ptr);
        std::sort(friends.begin(), friends.end(), [](const Friend& a, const Friend& b) {
            if (a.in_recharged != b.in_recharged) return a.in_recharged;
            const bool a_on = a.presence != Presence::Offline, b_on = b.presence != Presence::Offline;
            if (a_on != b_on) return a_on;
            return Lower(a.name) < Lower(b.name);
        });
        inbox.Change([&](Status& status) {
            if (status.state == State::Ready) status.friends = std::move(friends);
        });
    }

    void Command(Inbox::Command command)
    {
        switch (command)
        {
        case Inbox::Command::Connect:
        {
            // Linked already (a connection that failed): the link is used
            // again; otherwise Discord's authorization.
            const State state = StateNow();
            if (state != State::NotLinked && state != State::Failed && state != State::Missing) break;
            DropClient();
            trade = 0;
            const auto link = platform::LoadDiscordLink(owner.link_file_);
            if (link && link->server == account.server) Refresh();
            else Authorize();
            break;
        }
        case Inbox::Command::Cancel:
            if (StateNow() == State::Linking && client)
            {
                sdk.Discord_Client_AbortAuthorize(&*client);
                DropClient();
                SetState(State::NotLinked);
            }
            break;
        case Inbox::Command::Disconnect:
            platform::ForgetProfileSession(owner.link_file_);
            DropClient();
            trade = 0;
            platform::SessionLogLine("Discord friends: disconnected");
            SetState(account.server.empty() ? State::Off : State::NotLinked);
            break;
        }
    }

    void Run()
    {
        std::unique_lock<std::mutex> lock(inbox.mutex);
        while (!inbox.stop)
        {
            if (inbox.account != account)
            {
                const Account wanted = inbox.account;
                lock.unlock();
                Switch(wanted);
                lock.lock();
                continue;
            }
            if (!inbox.commands.empty())
            {
                const auto command = inbox.commands.front();
                inbox.commands.pop_front();
                lock.unlock();
                Command(command);
                lock.lock();
                continue;
            }
            if (!inbox.results.empty())
            {
                auto [id, result] = std::move(inbox.results.front());
                inbox.results.pop_front();
                if (id != trade) continue; // answers a trade given up
                lock.unlock();
                Traded(std::move(result));
                lock.lock();
                continue;
            }
            if (retry_at && Clock::now() >= *retry_at && inbox.status.state == State::Failed)
            {
                lock.unlock();
                Refresh();
                lock.lock();
                continue;
            }
            if (client)
            {
                const bool ready = inbox.status.state == State::Ready;
                lock.unlock();
                sdk.Discord_RunCallbacks();
                if (ready && friends_dirty && Clock::now() - friends_at >= kFriendsEvery) ReadFriends();
                lock.lock();
                inbox.wake.wait_for(lock, kTick);
            }
            else if (retry_at)
                inbox.wake.wait_until(lock, *retry_at);
            else
                inbox.wake.wait(lock);
        }
        lock.unlock();
        DropClient();
    }
};
#else
struct DiscordFriends::Sdk
{
};
struct DiscordFriends::Worker
{
    Worker(DiscordFriends&, Inbox&) {}
    void Run() {}
};
#endif

DiscordFriends::DiscordFriends(std::filesystem::path library_folder, std::filesystem::path link_file, Trade trade)
    : library_(std::move(library_folder) / LibraryName()), link_file_(std::move(link_file)), trade_(std::move(trade)),
      inbox_(std::make_shared<Inbox>())
{
#if MSCHARGED_HAS_DISCORD_SOCIAL_SDK
    std::error_code error;
    if (!std::filesystem::is_regular_file(library_, error)) return; // Missing
    inbox_->status.state = State::Off;
    thread_ = std::thread([this] { Run(); });
#endif
}

DiscordFriends::~DiscordFriends()
{
    {
        std::lock_guard<std::mutex> lock(inbox_->mutex);
        inbox_->stop = true;
    }
    inbox_->wake.notify_all();
    if (thread_.joinable()) thread_.join();
}

void DiscordFriends::Run()
{
    Worker worker(*this, *inbox_);
    worker.Run();
}

void DiscordFriends::SetAccount(const Account& account)
{
    {
        std::lock_guard<std::mutex> lock(inbox_->mutex);
        if (inbox_->account == account) return;
        inbox_->account = account;
    }
    inbox_->wake.notify_all();
}

void DiscordFriends::Connect()
{
    {
        std::lock_guard<std::mutex> lock(inbox_->mutex);
        inbox_->commands.push_back(Inbox::Command::Connect);
    }
    inbox_->wake.notify_all();
}

void DiscordFriends::Cancel()
{
    {
        std::lock_guard<std::mutex> lock(inbox_->mutex);
        inbox_->commands.push_back(Inbox::Command::Cancel);
    }
    inbox_->wake.notify_all();
}

void DiscordFriends::Disconnect()
{
    {
        std::lock_guard<std::mutex> lock(inbox_->mutex);
        inbox_->commands.push_back(Inbox::Command::Disconnect);
    }
    inbox_->wake.notify_all();
}

DiscordFriends::Status DiscordFriends::Snapshot() const
{
    std::lock_guard<std::mutex> lock(inbox_->mutex);
    return inbox_->status;
}
} // namespace mscharged::launcher
