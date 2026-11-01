#include "platform/online_panel_feed.h"

#include "platform/http_client.h"

#include <chrono>

namespace mscharged::platform
{
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

namespace
{
// How often the open panel asks again, and after a failed answer.
constexpr auto kEvery = 3s;
constexpr auto kRetry = 10s;
} // namespace

std::unique_ptr<OnlinePanelFeed> OnlinePanelFeed::Start(const std::string& server_setting,
                                                        const std::filesystem::path& session_file,
                                                        std::string challenge_rules,
                                                        std::vector<std::string> discord_friends, Log log,
                                                        std::string* off)
{
    auto fail = [off](const std::string& reason) -> std::unique_ptr<OnlinePanelFeed> {
        if (off) *off = reason;
        return nullptr;
    };
    if (server_setting.empty()) return fail("no profile server (online.profile_server)");
    std::string error;
    const auto server = ProfileServerAddress(server_setting, &error);
    if (!server) return fail("online.profile_server: " + error);
    const auto session = LoadProfileSession(session_file);
    if (!session || session->server != *server) return fail("not signed in (the launcher's Online pages)");
    return std::unique_ptr<OnlinePanelFeed>(new OnlinePanelFeed(*server, session->token, std::move(challenge_rules),
                                                                std::move(discord_friends), std::move(log)));
}

OnlinePanelFeed::OnlinePanelFeed(std::string server, std::string token, std::string challenge_rules,
                                 std::vector<std::string> discord_friends, Log log)
    : server_(std::move(server)), token_(std::move(token)), discord_ids_(std::move(discord_friends)),
      log_(std::move(log)), challenge_rules_(std::move(challenge_rules))
{
    worker_ = std::thread([this] { Run(); });
}

OnlinePanelFeed::~OnlinePanelFeed()
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stop_ = true;
        if (cancel_) cancel_->Cancel();
    }
    wake_.notify_all();
    worker_.join();
}

void OnlinePanelFeed::SetOpen(bool open)
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (open == open_) return;
        open_ = open;
        due_ = open; // ask at once when it opens
    }
    wake_.notify_all();
}

void OnlinePanelFeed::SetActivity(std::string activity)
{
    std::lock_guard<std::mutex> lock(mutex_);
    activity_ = std::move(activity);
}

OnlinePanelFeed::Snapshot OnlinePanelFeed::Latest()
{
    std::lock_guard<std::mutex> lock(mutex_);
    return snapshot_;
}

std::optional<std::string> OnlinePanelFeed::TakeNotice()
{
    std::lock_guard<std::mutex> lock(mutex_);
    auto notice = std::move(notice_);
    notice_.reset();
    return notice;
}

void OnlinePanelFeed::Send(std::function<void(ProfileService&)> action, std::string what)
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        actions_.push_back({std::move(action), std::move(what)});
    }
    wake_.notify_all();
}

void OnlinePanelFeed::SetChallengeRules(std::string challenge_rules)
{
    std::lock_guard<std::mutex> lock(mutex_);
    challenge_rules_ = std::move(challenge_rules);
}

std::string OnlinePanelFeed::ChallengeRules()
{
    std::lock_guard<std::mutex> lock(mutex_);
    return challenge_rules_;
}

void OnlinePanelFeed::Challenge(std::int64_t player, std::int64_t rule_set, MatchRules rules, std::string name)
{
    Send(
        [player, rule_set, rules = std::move(rules), name = std::move(name)](ProfileService& api) {
            if (rule_set > 0) api.ChallengePlayer(player, rule_set);
            else api.ChallengePlayer(player, rules, name);
        },
        "The challenge");
}

void OnlinePanelFeed::AddFriend(std::int64_t player)
{
    Send([player](ProfileService& api) { api.AddFriend(player); }, "Adding the friend");
}

void OnlinePanelFeed::RemoveFriend(std::int64_t player)
{
    Send([player](ProfileService& api) { api.RemoveFriend(player); }, "Taking off the friend");
}

void OnlinePanelFeed::AddMiiFriend(std::int32_t profile)
{
    Send([profile](ProfileService& api) { api.AddMiiFriend(profile); }, "Adding the Mii friend");
}

void OnlinePanelFeed::RemoveMiiFriend(std::int32_t profile)
{
    Send([profile](ProfileService& api) { api.RemoveMiiFriend(profile); }, "Taking off the Mii friend");
}

void OnlinePanelFeed::SetAvailable(bool available)
{
    Send([available](ProfileService& api) { api.SetAvailable(available); }, "Available for random games");
}

void OnlinePanelFeed::ReadyToPlay(std::int64_t match, bool ready)
{
    Send([match, ready](ProfileService& api) { api.ReadyToPlay(match, ready); }, "Ready to play!");
}

void OnlinePanelFeed::Cancel(std::int64_t match)
{
    Send([match](ProfileService& api) { api.Cancel(match); }, "Cancelling the set");
}

void OnlinePanelFeed::Run()
{
    std::unique_lock<std::mutex> lock(mutex_);
    auto next = Clock::now();
    while (!stop_)
    {
        // Wait for an action, the panel to open, or the next ask while open.
        const auto ready = [this, next] {
            return stop_ || !actions_.empty() || due_ || (open_ && Clock::now() >= next);
        };
        if (open_) wake_.wait_until(lock, next, ready);
        else wake_.wait(lock, ready);
        if (stop_) break;
        auto actions = std::move(actions_);
        actions_.clear();
        const bool fetch = open_ || due_ || !actions.empty();
        due_ = false;
        if (!fetch) continue; // closed while it waited
        auto cancel = std::make_shared<HttpCancel>();
        cancel_ = cancel;
        const std::string activity = activity_;
        lock.unlock();

        ProfileService api(server_, token_);
        api.SetCancel(cancel);
        api.SetActivity(activity);
        std::optional<std::string> notice;
        for (auto& pending : actions)
        {
            try
            {
                pending.action(api);
            }
            catch (const ProfileError& e)
            {
                if (e.Cancelled()) break;
                notice = e.Status() == 0 ? pending.what + " didn't reach the server." : pending.what + ": " + e.what();
            }
            catch (const std::exception& e)
            {
                notice = pending.what + ": " + e.what();
            }
        }
        bool failed = false;
        if (fetch)
        {
            try
            {
                Fetch(api);
            }
            catch (const std::exception& e)
            {
                failed = true;
                std::lock_guard<std::mutex> relock(mutex_);
                snapshot_.error = e.what();
            }
        }

        lock.lock();
        cancel_.reset();
        if (notice) notice_ = notice;
        next = Clock::now() + (failed ? kRetry : kEvery);
    }
}

void OnlinePanelFeed::Fetch(ProfileService& api)
{
    Snapshot fresh;
    fresh.me = api.Me();
    {
        // The rule sets and the server's version change rarely: once. The
        // Discord friends stay as they were if the server can't say now.
        std::lock_guard<std::mutex> lock(mutex_);
        fresh.rule_sets = snapshot_.rule_sets;
        fresh.server_version = snapshot_.server_version;
        fresh.discord_friends = snapshot_.discord_friends;
    }
    fresh.lists = api.Players();
    FetchDiscordFriends(api, fresh.discord_friends);
    fresh.match = api.CurrentMatch();
    if (fresh.rule_sets.empty()) fresh.rule_sets = api.RuleSets();
    if (fresh.server_version.empty()) fresh.server_version = api.Info().version;
    fresh.loaded = true;
    std::lock_guard<std::mutex> lock(mutex_);
    fresh.version = snapshot_.version + 1;
    snapshot_ = std::move(fresh);
}

// Which Discord friends play Recharged, with the lists: their online status
// changes as the friends' does. A failure keeps the last list and doesn't
// fail the panel's lists.
void OnlinePanelFeed::FetchDiscordFriends(ProfileService& api, std::vector<ProfilePlayer>& players)
{
    if (discord_ids_.empty()) return;
    std::string failure;
    try
    {
        players = FindDiscordPlayers(api, discord_ids_);
        discord_failed_ = false;
        return;
    }
    catch (const ProfileError& e)
    {
        if (e.Cancelled()) throw;
        failure = e.what();
    }
    catch (const std::exception& e)
    {
        failure = e.what();
    }
    if (discord_failed_) return;
    discord_failed_ = true;
    if (log_) log_("Overlay: which Discord friends play is not known (" + failure + ")");
}
} // namespace mscharged::platform
