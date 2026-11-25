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

std::optional<std::size_t> OwnTournament(const std::vector<ProfileTournament>& tournaments, std::int64_t pinned)
{
    for (std::size_t at = 0; at < tournaments.size(); ++at)
        if (pinned && tournaments[at].id == pinned && tournaments[at].joined && tournaments[at].state == "running")
            return at;
    for (std::size_t at = 0; at < tournaments.size(); ++at)
        if (tournaments[at].joined && tournaments[at].state == "running") return at;
    return std::nullopt;
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
    std::optional<TournamentPage> shown, own;
    std::int64_t pinned = 0;
    {
        std::lock_guard lock(mutex_);
        shown = snapshot_.page;
        own = snapshot_.own;
        pinned = ownId_;
    }
    // The action first: joining, leaving and READY on the tournament shown,
    // a READY taken back on the player's own (where it was pressed, whatever
    // the pages show now).
    const auto& target = action == Action::Unready && own ? own : shown;
    if (action != Action::None && !signed_in) action_error = "Sign in to play tournaments";
    else if (action != Action::None && !target) action_error = "No tournament to play";
    else if (action != Action::None)
    {
        const TournamentPage& on = *target;
        const std::int64_t id = on.tournament.id;
        std::int64_t readied = 0;
        try
        {
            switch (action)
            {
            case Action::Join: api.JoinTournament(id); break;
            case Action::Leave: api.LeaveTournament(id); break;
            case Action::Ready:
            case Action::Unready:
                if (!on.me || !on.me->set) throw ProfileError("You have no set to play", 0);
                if (const auto made = api.TournamentReady(id, on.me->set, action == Action::Ready))
                {
                    readied = made->id;
                    // At once, not with the page asked below: the
                    // opponent's READY may make the set go meanwhile.
                    std::lock_guard lock(mutex_);
                    readyMatch_ = readied;
                    if (action == Action::Ready) pinned = ownId_ = id;
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
                     + on.tournament.name + (readied ? " (match " + std::to_string(readied) + ")" : ""));
        }
        catch (const ProfileError& e)
        {
            action_error = e.what();
            if (log_) log_(std::string("Tournament: the action failed: ") + e.what());
        }
    }
    // Then the list, the page shown and the player's own, and the match of
    // their set: each kept as it was when its own ask fails.
    std::vector<ProfileTournament> tournaments;
    std::optional<TournamentPage> page, own_page;
    std::optional<ProfileMatch> match;
    bool listed = false, page_known = false, own_known = false, match_known = false;
    const auto ask = [&](auto&& asking) {
        try
        {
            asking();
            return true;
        }
        catch (const ProfileError& e)
        {
            error = e.what();
            return false;
        }
    };
    listed = ask([&] { tournaments = api.Tournaments(); });
    if (listed)
    {
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
        const auto own_at = OwnTournament(tournaments, pinned);
        page_known = ask([&] {
            if (!at) return;
            page = api.Tournament(tournaments[*at].id);
            pollSeconds_ = page->poll_interval;
        });
        own_known = ask([&] {
            if (!own_at) return;
            if (page && page->tournament.id == tournaments[*own_at].id) own_page = page;
            else own_page = api.Tournament(tournaments[*own_at].id);
        });
        // The player's set plays: their match says who is ready.
        match_known = own_known && ask([&] {
            if (!signed_in || !own_page || !own_page->me || !own_page->me->set) return;
            if (const auto* set = own_page->Set(own_page->me->set); set && set->state == "playing" && set->match)
                if (auto current = api.CurrentMatch(); current && current->id == set->match) match = std::move(current);
        });
    }
    std::lock_guard lock(mutex_);
    const bool was_failing = !snapshot_.error.empty();
    if (!error.empty() && !was_failing && log_) log_("Tournament: " + error);
    else if (error.empty() && was_failing && log_) log_("Tournament: the server answers again");
    snapshot_.loaded = true;
    ++snapshot_.version;
    snapshot_.signed_in = signed_in;
    if (listed) snapshot_.tournaments = std::move(tournaments);
    if (page_known) snapshot_.page = std::move(page);
    if (own_known) snapshot_.own = std::move(own_page);
    if (match_known) snapshot_.match = std::move(match);
    snapshot_.error = std::move(error);
    if (action != Action::None)
    {
        snapshot_.action_error = std::move(action_error);
        snapshot_.busy = pending_ != Action::None;
    }
    snapshot_.fetched = Clock::now();
}
} // namespace mscharged::platform
