#include "launcher/competitive_service.h"

#include "platform/http_client.h"
#include "platform/session_log.h"

#include <algorithm>

namespace mscharged::launcher
{
using platform::ProfileError;
using platform::ProfileService;
using platform::SignInResult;
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

namespace
{
// How often the launcher asks for news: the current set (a challenge, the
// other player's answer and readiness, the result; as often as the server
// says, this by default), which Discord friends play Recharged, the player
// (friend codes the game proved), and the server while it does not answer.
// Every call is also the player's heartbeat: a set lasts while both have
// Recharged open, and only a player with Recharged open can be challenged.
constexpr auto kMatchPoll = 4s;
constexpr auto kDiscordPoll = 30s;
constexpr auto kPlayerPoll = 60s;
constexpr auto kRetry = 15s;

// A Mii sign-in's failure, as the page tells it.
std::string MiiSignInError(const ProfileError& error)
{
    switch (error.Status())
    {
    case 0: return "The server doesn't answer. Check your internet connection, then try again.";
    case 404: return "This server can't sign in players with a Mii yet.";
    default:
        // The server's own words (a name it refuses, how long to wait), else
        // what its status says.
        if (std::string(error.what()).rfind("The server answered", 0) != 0) return error.what();
        if (error.Status() == 429) return "Too many new players from your network just now. Try again later.";
        return error.what();
    }
}
} // namespace

CompetitiveService::CompetitiveService(std::filesystem::path session_file, std::filesystem::path discord_link_file)
    : session_file_(std::move(session_file)), discord_link_file_(std::move(discord_link_file))
{
    worker_ = std::thread([this] { Run(); });
}

CompetitiveService::~CompetitiveService()
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stop_ = true;
        if (cancel_) cancel_->Cancel();
    }
    wake_.notify_all();
    worker_.join();
}

void CompetitiveService::SetServer(const std::string& server)
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (server == status_.server && (server.empty() || status_.state != State::NoServer)) return;
        ++generation_;
        tasks_.clear();
        if (cancel_) cancel_->Cancel();
        const auto serial = status_.serial;
        status_ = Status{};
        status_.serial = serial + 1;
        status_.server = server;
        status_.state = server.empty() ? State::NoServer : State::Connecting;
        token_.clear();
        sign_in_id_.clear();
        known_asking_.clear();
        if (!server.empty())
            tasks_.push_back({generation_, [this](ProfileService& api) { Connect(api); }, false});
    }
    wake_.notify_all();
}

void CompetitiveService::Post(Task task, bool action)
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (status_.state == State::NoServer) return;
        tasks_.push_back({generation_, std::move(task), action});
        if (action)
        {
            status_.busy = true;
            ++status_.serial;
        }
    }
    wake_.notify_all();
}

void CompetitiveService::Change(const std::function<void(Status&)>& change)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (running_ != generation_) return; // for a server no longer in use
    change(status_);
    ++status_.serial;
}

CompetitiveService::Status CompetitiveService::Snapshot() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return status_;
}

void CompetitiveService::Run()
{
    std::unique_lock<std::mutex> lock(mutex_);
    while (!stop_)
    {
        const State state = status_.state;
        const bool polling = state == State::SignedIn || state == State::SigningIn || state == State::Unavailable;
        if (tasks_.empty() && !(polling && Clock::now() >= next_poll_))
        {
            if (polling) wake_.wait_until(lock, next_poll_);
            else wake_.wait(lock);
            continue;
        }
        Task task;
        bool action = false;
        if (!tasks_.empty())
        {
            auto queued = std::move(tasks_.front());
            tasks_.pop_front();
            if (queued.generation != generation_) continue;
            task = std::move(queued.task);
            action = queued.action;
        }
        else
            task = [this](ProfileService& api) { Poll(api); };
        const std::uint64_t generation = generation_;
        running_ = generation;
        auto cancel = std::make_shared<platform::HttpCancel>();
        cancel_ = cancel;
        const std::string server = status_.server, token = token_;
        lock.unlock();
        // A call answered after SetServer chose another server is dropped:
        // the generation tells Change and the session file apart.
        try
        {
            ProfileService api(server, token);
            api.SetCancel(cancel);
            task(api);
        }
        catch (const ProfileError& e)
        {
            if (!e.Cancelled()) Fail(e);
        }
        catch (const std::exception& e)
        {
            Fail(e);
        }
        lock.lock();
        if (cancel_ == cancel) cancel_.reset();
        if (action && generation == generation_)
        {
            bool more = false;
            for (const auto& queued : tasks_) more = more || (queued.action && queued.generation == generation_);
            status_.busy = more;
            ++status_.serial;
        }
    }
}

void CompetitiveService::Fail(const std::exception& error)
{
    const auto* profile = dynamic_cast<const ProfileError*>(&error);
    if (profile && profile->SignedOut())
    {
        SignedOutHere("You were signed out: the session ended. Sign in again.");
        return;
    }
    Change([&](Status& status) {
        // The same failure again (a server that stays away) is told once.
        if (status.error != error.what()) ++status.error_serial;
        status.error = error.what();
        // Connecting failed: tried again later.
        if (status.state == State::Connecting) status.state = State::Unavailable;
    });
    std::lock_guard<std::mutex> lock(mutex_);
    if (running_ == generation_ && status_.state == State::Unavailable) next_poll_ = Clock::now() + kRetry;
}

void CompetitiveService::SignedOutHere(const char* reason)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (running_ != generation_) return;
    platform::ForgetProfileSession(session_file_);
    token_.clear();
    sign_in_id_.clear();
    status_.state = State::SignedOut;
    status_.player = {};
    status_.match.reset();
    status_.connecting_discord = false;
    status_.connect_error.clear();
    status_.code.clear();
    status_.url.clear();
    // The game save's Miis may play for this player now: asked again.
    status_.known_miis.clear();
    status_.known_asked.clear();
    known_asking_.clear();
    if (reason)
    {
        status_.error = reason;
        ++status_.error_serial;
    }
    ++status_.serial;
}

void CompetitiveService::Connect(ProfileService& api)
{
    const auto info = api.Info();
    Change([&](Status& status) {
        status.info = info;
        status.error.clear();
    });
    const auto saved = platform::LoadProfileSession(session_file_);
    if (!saved || saved->server != api.Server())
    {
        Change([](Status& status) { status.state = State::SignedOut; });
        return;
    }
    api.SetToken(saved->token);
    try
    {
        Signed(api, saved->token, api.Me());
    }
    catch (const ProfileError& e)
    {
        if (!e.SignedOut()) throw;
        SignedOutHere(nullptr);
    }
}

void CompetitiveService::Signed(ProfileService& api, const std::string& token, const platform::ProfilePlayer& player)
{
    // Everything the page shows signed in, before it shows it.
    api.SetToken(token);
    const auto sets = api.RuleSets();
    int interval = 0;
    const auto match = api.CurrentMatch(&interval);
    std::lock_guard<std::mutex> lock(mutex_);
    if (running_ != generation_) return;
    platform::SaveProfileSession(session_file_, {api.Server(), token, player.name});
    token_ = token;
    sign_in_id_.clear();
    status_.state = State::SignedIn;
    status_.player = player;
    status_.rule_sets = sets;
    status_.match = match;
    status_.code.clear();
    status_.url.clear();
    status_.error.clear();
    status_.connecting_discord = false;
    status_.connect_error.clear();
    status_.mii_error.clear();
    ++status_.serial;
    next_me_ = Clock::now() + kPlayerPoll;
    next_discord_ = Clock::now();
    next_poll_ = Clock::now() + (interval ? std::chrono::seconds(interval) : kMatchPoll);
}

void CompetitiveService::Poll(ProfileService& api)
{
    State state;
    std::string id;
    bool me, discord_due, connecting;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        state = status_.state;
        id = sign_in_id_;
        connecting = status_.connecting_discord && !id.empty();
        me = Clock::now() >= next_me_;
        discord_due = Clock::now() >= next_discord_;
        next_poll_ = Clock::now()
            + (state == State::SigningIn || connecting ? std::chrono::seconds(poll_interval_) : kMatchPoll);
    }
    if (state == State::Unavailable)
    {
        Change([](Status& status) { status.state = State::Connecting; });
        Connect(api);
        return;
    }
    if (state == State::SigningIn)
    {
        const auto result = api.PollSignIn(id);
        switch (result.status)
        {
        case SignInResult::Status::Waiting: return;
        case SignInResult::Status::SignedIn:
            // The Discord friends link with the sign-in's tokens: kept before
            // it counts as signed in, so they connect from it at once.
            if (result.discord)
            {
                try
                {
                    platform::SaveDiscordLink(discord_link_file_, {api.Server(), result.discord->refresh_token});
                }
                catch (const std::exception& e)
                {
                    platform::SessionLogLine(std::string("Discord friends: the link is not kept: ") + e.what());
                }
            }
            Signed(api, result.token, result.player);
            return;
        case SignInResult::Status::Refused:
        case SignInResult::Status::Expired:
            Change([&](Status& status) {
                status.state = State::SignedOut;
                status.code.clear();
                status.url.clear();
                status.error = result.status == SignInResult::Status::Expired
                    ? "The sign-in took too long. Try again."
                    : (result.message.empty() ? "The sign-in was refused." : result.message);
                ++status.error_serial;
            });
            std::lock_guard<std::mutex> lock(mutex_);
            if (running_ == generation_) sign_in_id_.clear();
            return;
        }
        return;
    }
    if (state != State::SignedIn) return;
    // Discord being connected to a Mii player's profile: its sign-in, then
    // the usual asks while it waits.
    if (connecting)
    {
        const auto result = api.PollSignIn(id);
        if (result.status == SignInResult::Status::SignedIn)
        {
            if (result.discord)
            {
                try
                {
                    platform::SaveDiscordLink(discord_link_file_, {api.Server(), result.discord->refresh_token});
                }
                catch (const std::exception& e)
                {
                    platform::SessionLogLine(std::string("Discord friends: the link is not kept: ") + e.what());
                }
            }
            Signed(api, result.token, result.player);
            return;
        }
        if (result.status != SignInResult::Status::Waiting)
        {
            Change([&](Status& status) {
                status.connecting_discord = false;
                status.code.clear();
                status.url.clear();
                status.connect_error = result.status == SignInResult::Status::Expired
                    ? "Connecting Discord took too long. Try again."
                    : (result.message.empty() ? "Discord wasn't connected." : result.message);
            });
            std::lock_guard<std::mutex> lock(mutex_);
            if (running_ == generation_) sign_in_id_.clear();
            connecting = false;
        }
    }
    int interval = 0;
    const auto match = api.CurrentMatch(&interval);
    std::optional<platform::ProfilePlayer> player;
    if (me) player = api.Me();
    // Which of the Discord friends play Recharged.
    std::optional<std::vector<platform::ProfilePlayer>> discord;
    if (discord_due)
    {
        std::vector<std::string> ids;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            ids = discord_ids_;
        }
        discord = platform::FindDiscordPlayers(api, ids);
    }
    Change([&](Status& status) {
        status.match = match;
        if (player) status.player = *player;
        if (discord) status.discord_players = std::move(*discord);
        status.error.clear();
    });
    std::lock_guard<std::mutex> lock(mutex_);
    if (me) next_me_ = Clock::now() + kPlayerPoll;
    if (discord_due) next_discord_ = Clock::now() + kDiscordPoll;
    if (interval && !connecting) next_poll_ = Clock::now() + std::chrono::seconds(interval);
}

void CompetitiveService::SetDiscordFriends(std::vector<std::string> discord_ids)
{
    if (discord_ids.size() > platform::kDiscordFriendsLookedUp) discord_ids.resize(platform::kDiscordFriendsLookedUp);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (discord_ids == discord_ids_) return;
        // A first list, or another one: looked up at once.
        discord_ids_ = std::move(discord_ids);
        if (discord_ids_.empty()) status_.discord_players.clear();
        else next_poll_ = next_discord_ = Clock::now();
        ++status_.serial;
    }
    wake_.notify_all();
}

void CompetitiveService::LookUpMiis(std::vector<std::uint32_t> profile_ids)
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (profile_ids == known_asking_) return;
        known_asking_ = profile_ids;
    }
    Post(
        [this, profile_ids](ProfileService& api) {
            std::vector<std::pair<std::uint32_t, std::string>> known;
            try
            {
                if (!profile_ids.empty()) known = api.KnownWfc(profile_ids);
            }
            catch (const ProfileError& e)
            {
                // A server before 1.2.4 can't tell, nor one out of reach:
                // no answer, and the launcher goes by what it remembers.
                if (e.Cancelled()) throw;
                return;
            }
            Change([&](Status& status) {
                status.known_miis = std::move(known);
                status.known_asked = profile_ids;
            });
        },
        false);
}

void CompetitiveService::TradeDiscordToken(std::string code, std::string verifier, std::string redirect_uri,
                                           std::string refresh_token, DiscordTraded done)
{
    Post(
        [=](ProfileService& api) {
            try
            {
                done(refresh_token.empty() ? api.DiscordToken(code, verifier, redirect_uri)
                                           : api.RefreshDiscordToken(refresh_token),
                     std::string(), false);
            }
            catch (const ProfileError& e)
            {
                done({}, e.what(), e.Status() == 400);
                if (e.SignedOut()) throw;
            }
            catch (const std::exception& e)
            {
                done({}, e.what(), false);
            }
        },
        false);
}

void CompetitiveService::SignIn()
{
    Post([this](ProfileService& api) {
        // Signed in (with a Mii), the session goes with it: Discord joins
        // that profile, which stays signed in meanwhile.
        Change([](Status& status) { status.connect_error.clear(); });
        const bool linking = Snapshot().state == State::SignedIn;
        const auto start = api.StartSignIn();
        // A server that would not join Discord to this profile would sign
        // in another one instead.
        if (linking && !start.link)
        {
            Change([](Status& status) {
                status.connect_error = "This server can't connect Discord to a Mii profile yet.";
            });
            return;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        if (running_ != generation_) return;
        sign_in_id_ = start.id;
        poll_interval_ = start.poll_interval;
        next_poll_ = Clock::now() + std::chrono::seconds(start.poll_interval);
        if (status_.state == State::SignedIn) status_.connecting_discord = true;
        else status_.state = State::SigningIn;
        status_.code = start.code;
        status_.url = start.url;
        ++status_.sign_in;
        ++status_.serial;
    }, true);
}

void CompetitiveService::TestSignIn(const std::string& name)
{
    Post([this, name](ProfileService& api) {
        const auto result = api.TestSignIn(name);
        if (result.status == SignInResult::Status::SignedIn) Signed(api, result.token, result.player);
        else throw ProfileError(result.message.empty() ? "The sign-in was refused." : result.message, 200);
    }, true);
}

void CompetitiveService::MiiSignIn(const std::string& name)
{
    Post([this, name](ProfileService& api) {
        Change([](Status& status) { status.mii_error.clear(); });
        SignInResult result;
        try
        {
            result = api.MiiSignIn(name);
        }
        catch (const ProfileError& e)
        {
            if (e.Cancelled()) throw;
            Change([&](Status& status) { status.mii_error = MiiSignInError(e); });
            return;
        }
        if (result.status == SignInResult::Status::SignedIn) Signed(api, result.token, result.player);
        else
            Change([&](Status& status) {
                status.mii_error = result.message.empty() ? "The sign-in was refused." : result.message;
            });
    }, true);
}

void CompetitiveService::StopSignIn()
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (status_.state == State::SignedIn && status_.connecting_discord)
    {
        sign_in_id_.clear();
        status_.connecting_discord = false;
        status_.code.clear();
        status_.url.clear();
        ++status_.serial;
        return;
    }
    if (status_.state != State::SigningIn) return;
    sign_in_id_.clear();
    status_.state = State::SignedOut;
    status_.code.clear();
    status_.url.clear();
    ++status_.serial;
}

void CompetitiveService::SignOut()
{
    Post([this](ProfileService& api) {
        try
        {
            api.SignOut();
        }
        catch (const ProfileError& e)
        {
            // Signed out here all the same; the server ends the session in 90 days.
            if (e.Cancelled()) throw;
        }
        SignedOutHere(nullptr);
    }, true);
}

void CompetitiveService::ReadyToPlay(bool ready)
{
    Post([this, ready](ProfileService& api) {
        const auto current = Snapshot().match;
        if (!current) return;
        const auto match = api.ReadyToPlay(current->id, ready);
        Change([&](Status& status) { status.match = match; });
    }, true);
}

void CompetitiveService::Accept()
{
    Post([this](ProfileService& api) {
        const auto current = Snapshot().match;
        if (!current) return;
        const auto match = api.Accept(current->id);
        Change([&](Status& status) { status.match = match; });
    }, true);
}

void CompetitiveService::Decline()
{
    Post([this](ProfileService& api) {
        const auto current = Snapshot().match;
        if (!current) return;
        const auto match = api.Decline(current->id);
        Change([&](Status& status) { status.match = match; });
    }, true);
}

void CompetitiveService::CancelMatch()
{
    Post([this](ProfileService& api) {
        const auto current = Snapshot().match;
        if (!current) return;
        const auto match = api.Cancel(current->id);
        Change([&](Status& status) { status.match = match; });
    }, true);
}
} // namespace mscharged::launcher
