#include "platform/tournament_feed.h"

#include <algorithm>
#include <functional>
#include <initializer_list>
#include <utility>

namespace mscharged::platform
{
std::optional<std::size_t> ChooseTournament(const std::vector<ProfileTournament>& tournaments)
{
    const auto first = [&](auto&& wanted) -> std::optional<std::size_t> {
        for (std::size_t at = 0; at < tournaments.size(); ++at)
            if (wanted(tournaments[at])) return at;
        return std::nullopt;
    };
    for (const auto& wanted : std::initializer_list<std::function<bool(const ProfileTournament&)>>{
             [](const ProfileTournament& t) { return t.joined && t.state == "running"; },
             [](const ProfileTournament& t) { return t.joined && t.state == "registration"; },
             [](const ProfileTournament& t) { return t.state == "registration"; },
             [](const ProfileTournament& t) { return t.state == "running"; },
         })
        if (const auto at = first(wanted)) return at;
    // Else the last that ended, with its standings; one called off says nothing.
    return first([](const ProfileTournament& t) { return t.state == "finished"; });
}

std::unique_ptr<TournamentFeed> TournamentFeed::Start(const std::string& server_setting,
                                                      std::filesystem::path session_file, Log log, std::string* off)
{
    if (server_setting.empty())
    {
        if (off) *off = "no profile server (online.profile_server)";
        return nullptr;
    }
    std::string error;
    const auto server = ProfileServerAddress(server_setting, &error);
    if (!server)
    {
        if (off) *off = "online.profile_server: " + error;
        return nullptr;
    }
    return std::unique_ptr<TournamentFeed>(new TournamentFeed(*server, std::move(session_file), std::move(log)));
}

TournamentFeed::TournamentFeed(std::string server, std::filesystem::path session_file, Log log)
    : server_(std::move(server)), sessionFile_(std::move(session_file)), log_(std::move(log))
{
    thread_ = std::thread([this] { Run(); });
}

TournamentFeed::~TournamentFeed()
{
    {
        std::lock_guard lock(mutex_);
        stop_ = true;
    }
    wake_.notify_all();
    thread_.join();
}

void TournamentFeed::SetOpen(bool open)
{
    {
        std::lock_guard lock(mutex_);
        if (open_ == open) return;
        open_ = open;
        if (!open) shown_ = 0;
    }
    wake_.notify_all();
}

void TournamentFeed::Show(std::int64_t id)
{
    {
        std::lock_guard lock(mutex_);
        if (shown_ == id) return;
        shown_ = id;
        shownChanged_ = true;
    }
    wake_.notify_all();
}

void TournamentFeed::Ask(Action action)
{
    {
        std::lock_guard lock(mutex_);
        pending_ = action;
        snapshot_.busy = true;
        snapshot_.action_error.clear();
        ++snapshot_.version;
    }
    wake_.notify_all();
}

void TournamentFeed::Join() { Ask(Action::Join); }
void TournamentFeed::Leave() { Ask(Action::Leave); }
void TournamentFeed::Ready(bool ready) { Ask(ready ? Action::Ready : Action::Unready); }

TournamentFeed::Snapshot TournamentFeed::Latest() const
{
    std::lock_guard lock(mutex_);
    return snapshot_;
}

std::uint64_t TournamentFeed::Version() const
{
    std::lock_guard lock(mutex_);
    return snapshot_.version;
}

std::int64_t TournamentFeed::ReadyMatch() const
{
    std::lock_guard lock(mutex_);
    return readyMatch_;
}

void TournamentFeed::Run()
{
    // The next round while the pages are open, and while they are closed
    // (the first a moment after the game starts).
    Clock::time_point next{}, watch = Clock::now() + std::chrono::seconds(5);
    std::unique_lock lock(mutex_);
    while (!stop_)
    {
        if (pending_ == Action::None && !shownChanged_)
        {
            const bool open = open_;
            const auto due = open ? next : watch;
            if (Clock::now() < due)
            {
                wake_.wait_until(lock, due, [this, open] {
                    return stop_ || open_ != open || shownChanged_ || pending_ != Action::None;
                });
                continue;
            }
        }
        const Action action = std::exchange(pending_, Action::None);
        shownChanged_ = false;
        lock.unlock();
        Round(action);
        lock.lock();
        const auto now = Clock::now();
        next = now + std::chrono::seconds(pollSeconds_);
        // The player in a tournament open for entries or running: their set
        // may come any time.
        const bool watching = std::any_of(snapshot_.tournaments.begin(), snapshot_.tournaments.end(),
                                          [](const ProfileTournament& t) {
                                              return t.joined && (t.state == "running" || t.state == "registration");
                                          });
        watch = now + std::chrono::seconds(watching ? kWatchSeconds : kIdleSeconds);
    }
}

void TournamentFeed::Round(Action action)
{
    const auto session = LoadProfileSession(sessionFile_);
    const bool signed_in = session && session->server == server_;
    ProfileService api(server_, signed_in ? session->token : std::string());
    api.SetActivity("game");
    std::string error, action_error;
    std::vector<ProfileTournament> tournaments;
    std::optional<TournamentPage> page;
    std::optional<ProfileMatch> match;
    std::int64_t readied = 0;
    bool listed = false;
    try
    {
        // The action first, on the tournament shown.
        std::optional<TournamentPage> shown;
        {
            std::lock_guard lock(mutex_);
            shown = snapshot_.page;
        }
        if (action != Action::None && !signed_in) action_error = "Sign in to play tournaments";
        else if (action != Action::None && !shown) action_error = "No tournament to play";
        if (action != Action::None && signed_in && shown)
        {
            const std::int64_t id = shown->tournament.id;
            try
            {
                switch (action)
                {
                case Action::Join: shown = api.JoinTournament(id); break;
                case Action::Leave: shown = api.LeaveTournament(id); break;
                case Action::Ready:
                case Action::Unready:
                    if (!shown->me || !shown->me->set) throw ProfileError("You have no set to play", 0);
                    if (const auto made = api.TournamentReady(id, shown->me->set, action == Action::Ready))
                    {
                        readied = made->id;
                        // At once, not with the page asked below: the
                        // opponent's READY may make the set go meanwhile.
                        std::lock_guard lock(mutex_);
                        readyMatch_ = readied;
                    }
                    break;
                case Action::None: break;
                }
                if (log_)
                    log_(std::string("Tournament: ")
                         + (action == Action::Join    ? "joined "
                            : action == Action::Leave ? "left "
                            : action == Action::Ready ? "READY in "
                                                      : "READY taken back in ")
                         + shown->tournament.name + (readied ? " (match " + std::to_string(readied) + ")" : ""));
            }
            catch (const ProfileError& e)
            {
                action_error = e.what();
                if (log_) log_(std::string("Tournament: the action failed: ") + e.what());
            }
        }
        tournaments = api.Tournaments();
        listed = true;
        // The one the pages show, while they are open and it is listed; else
        // the feed's choice.
        std::optional<std::size_t> at;
        {
            std::lock_guard lock(mutex_);
            if (open_ && shown_)
                for (std::size_t n = 0; n < tournaments.size(); ++n)
                    if (tournaments[n].id == shown_) at = n;
        }
        if (!at) at = ChooseTournament(tournaments);
        if (at)
        {
            page = api.Tournament(tournaments[*at].id);
            pollSeconds_ = page->poll_interval;
            // The player's set plays: their match says who is ready.
            if (signed_in && page->me && page->me->set)
                if (const auto* set = page->Set(page->me->set); set && set->state == "playing" && set->match)
                    if (auto current = api.CurrentMatch(); current && current->id == set->match)
                        match = std::move(current);
        }
    }
    catch (const ProfileError& e)
    {
        error = e.what();
    }
    std::lock_guard lock(mutex_);
    const bool was_failing = !snapshot_.error.empty();
    if (!error.empty() && !was_failing && log_) log_("Tournament: " + error);
    else if (error.empty() && was_failing && log_) log_("Tournament: the server answers again");
    snapshot_.loaded = true;
    ++snapshot_.version;
    snapshot_.signed_in = signed_in;
    if (listed)
    {
        snapshot_.tournaments = std::move(tournaments);
        snapshot_.page = std::move(page);
        snapshot_.match = std::move(match);
    }
    snapshot_.error = std::move(error);
    if (action != Action::None)
    {
        snapshot_.action_error = std::move(action_error);
        snapshot_.busy = pending_ != Action::None;
    }
    snapshot_.fetched = Clock::now();
}
} // namespace mscharged::platform
