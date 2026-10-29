#include "launcher/competitive_service.h"

#include "platform/http_client.h"

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
// How often the page asks for news: the current set (a challenge, the other
// player's answer and readiness, the result; as often as the server says,
// this by default), the lobby's lists, the player (friend codes the game
// proved), and the server while it does not answer. Every call is also the
// player's heartbeat: a set lasts while both have Recharged open.
constexpr auto kMatchPoll = 4s;
constexpr auto kListsPoll = 5s;
constexpr auto kPlayerPoll = 60s;
constexpr auto kRetry = 15s;
} // namespace

CompetitiveService::CompetitiveService(std::filesystem::path session_file) : session_file_(std::move(session_file))
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
    platform::PlayerLists lists;
    try
    {
        lists = api.Players();
    }
    catch (const ProfileError& e)
    {
        // A server before sets has no lists.
        if (e.Status() != 404) throw;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (running_ != generation_) return;
    platform::SaveProfileSession(session_file_, {api.Server(), token, player.name});
    token_ = token;
    sign_in_id_.clear();
    status_.state = State::SignedIn;
    status_.player = player;
    status_.rule_sets = sets;
    status_.match = match;
    status_.lists = lists;
    status_.code.clear();
    status_.url.clear();
    status_.error.clear();
    ++status_.serial;
    next_me_ = Clock::now() + kPlayerPoll;
    next_lists_ = Clock::now() + kListsPoll;
    next_poll_ = Clock::now() + (interval ? std::chrono::seconds(interval) : kMatchPoll);
}

void CompetitiveService::Poll(ProfileService& api)
{
    State state;
    std::string id;
    bool me, lists;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        state = status_.state;
        id = sign_in_id_;
        me = Clock::now() >= next_me_;
        lists = Clock::now() >= next_lists_;
        next_poll_ = Clock::now() + (state == State::SigningIn ? std::chrono::seconds(poll_interval_) : kMatchPoll);
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
        case SignInResult::Status::SignedIn: Signed(api, result.token, result.player); return;
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
    int interval = 0;
    const auto match = api.CurrentMatch(&interval);
    std::optional<platform::ProfilePlayer> player;
    if (me) player = api.Me();
    std::optional<platform::PlayerLists> found;
    if (lists)
    {
        try
        {
            found = api.Players();
        }
        catch (const ProfileError& e)
        {
            if (e.Status() != 404) throw;
            found = platform::PlayerLists{};
        }
    }
    Change([&](Status& status) {
        status.match = match;
        if (player) status.player = *player;
        if (found) status.lists = *found;
        status.error.clear();
    });
    std::lock_guard<std::mutex> lock(mutex_);
    if (me) next_me_ = Clock::now() + kPlayerPoll;
    if (lists) next_lists_ = Clock::now() + kListsPoll;
    if (interval) next_poll_ = Clock::now() + std::chrono::seconds(interval);
}

void CompetitiveService::SignIn()
{
    Post([this](ProfileService& api) {
        const auto start = api.StartSignIn();
        std::lock_guard<std::mutex> lock(mutex_);
        if (running_ != generation_) return;
        sign_in_id_ = start.id;
        poll_interval_ = start.poll_interval;
        next_poll_ = Clock::now() + std::chrono::seconds(start.poll_interval);
        status_.state = State::SigningIn;
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

void CompetitiveService::StopSignIn()
{
    std::lock_guard<std::mutex> lock(mutex_);
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

void CompetitiveService::Challenge(const std::string& opponent, std::int64_t rule_set)
{
    Post([this, opponent, rule_set](ProfileService& api) {
        const auto match = api.Challenge(opponent, rule_set);
        Change([&](Status& status) { status.match = match; });
    }, true);
}

void CompetitiveService::ChallengePlayer(std::int64_t opponent_id, std::int64_t rule_set)
{
    Post([this, opponent_id, rule_set](ProfileService& api) {
        const auto match = api.ChallengePlayer(opponent_id, rule_set);
        Change([&](Status& status) { status.match = match; });
    }, true);
}

namespace
{
// The friends the server answered, and every list's friend marks after them.
void SetFriends(CompetitiveService::Status& status, std::vector<platform::ProfilePlayer> friends)
{
    const auto marked = [&](std::vector<platform::ProfilePlayer>& players) {
        for (auto& player : players)
            player.is_friend = std::any_of(friends.begin(), friends.end(),
                                           [&](const platform::ProfilePlayer& friend_) { return friend_.id == player.id; });
    };
    marked(status.lists.online);
    marked(status.lists.recent);
    status.lists.friends = std::move(friends);
}
} // namespace

void CompetitiveService::AddFriend(std::int64_t player_id)
{
    Post([this, player_id](ProfileService& api) {
        auto friends = api.AddFriend(player_id);
        Change([&](Status& status) { SetFriends(status, std::move(friends)); });
    }, true);
}

void CompetitiveService::AddFriend(const std::string& name)
{
    Post([this, name](ProfileService& api) {
        auto friends = api.AddFriend(name);
        Change([&](Status& status) { SetFriends(status, std::move(friends)); });
    }, true);
}

void CompetitiveService::RemoveFriend(std::int64_t player_id)
{
    Post([this, player_id](ProfileService& api) {
        auto friends = api.RemoveFriend(player_id);
        Change([&](Status& status) { SetFriends(status, std::move(friends)); });
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

void CompetitiveService::Rematch()
{
    Post([this](ProfileService& api) {
        const auto current = Snapshot().match;
        if (!current) return;
        const auto match = api.Rematch(current->id);
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
