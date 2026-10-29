#include "platform/competitive_host.h"

#include "platform/http_client.h"

#include <cstring>

namespace mscharged::platform
{
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

namespace
{
// The server keeps a ready game for 30 seconds; the game says it every 10.
constexpr auto kReadyEvery = 10s;
constexpr auto kMatchEvery = 10s;
constexpr auto kRetry = 15s;
// cGame's eGameState.
constexpr int kGamePreGame = 0, kGameKickoff = 1, kGameEnd = 3, kGamePlay = 5;
// A set's game waits this long for its last report before it closes anyway.
constexpr auto kCloseWait = 20s;

bool SameRules(const std::optional<ChargedCompetitiveRules>& a, const std::optional<ChargedCompetitiveRules>& b)
{
    if (a.has_value() != b.has_value()) return false;
    return !a || std::memcmp(&*a, &*b, sizeof *a) == 0;
}
} // namespace

std::unique_ptr<CompetitiveHost> CompetitiveHost::Start(const std::string& server_setting,
                                                        const std::filesystem::path& session_file, Log log,
                                                        std::string* off)
{
    auto fail = [off](const std::string& reason) -> std::unique_ptr<CompetitiveHost> {
        if (off) *off = reason;
        return nullptr;
    };
    if (server_setting.empty()) return fail("no profile server (online.profile_server)");
    std::string error;
    const auto server = ProfileServerAddress(server_setting, &error);
    if (!server) return fail("online.profile_server: " + error);
    const auto session = LoadProfileSession(session_file);
    if (!session || session->server != *server) return fail("not signed in (the launcher's Competitive page)");
    return std::unique_ptr<CompetitiveHost>(
        new CompetitiveHost(*server, session->token, session->player_name, std::move(log)));
}

CompetitiveHost::CompetitiveHost(std::string server, std::string token, std::string player, Log log)
    : server_(std::move(server)), token_(std::move(token)), player_(std::move(player)), log_(std::move(log))
{
    worker_ = std::thread([this] { Run(); });
}

CompetitiveHost::~CompetitiveHost()
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stop_ = true;
        if (cancel_) cancel_->Cancel();
    }
    wake_.notify_all();
    worker_.join();
}

void CompetitiveHost::Said(const std::string& line)
{
    // Each different line once in a row: a server that stays away is not a flood.
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (line == last_said_) return;
        last_said_ = line;
    }
    if (log_) log_("Competitive: " + line);
}

bool CompetitiveHost::Observe(const ChargedWfcSnapshot& wfc, const ChargedPresenceSnapshot* presence,
                              std::optional<ChargedCompetitiveRules>& rules)
{
    std::unique_lock<std::mutex> lock(mutex_);
    // The game's login: a new one (another ticket) is proven again.
    const std::string ticket = wfc.logged_in ? std::string(wfc.login_ticket) : std::string();
    const unsigned profile = wfc.logged_in ? wfc.profile_id : 0;
    bool wake = false;
    if (profile != login_profile_ || ticket != login_ticket_)
    {
        login_profile_ = profile;
        login_ticket_ = ticket;
        wake = true;
    }

    // The games of a series played by competitive rules, each counted when it
    // ends; the series is reported once one side has won it, or when the
    // match ends before (a player left).
    const bool in_series = presence && wfc.competitive && presence->in_match && presence->ranked;
    if (in_series && wfc.competitive_setups != series_setup_)
    {
        series_setup_ = wfc.competitive_setups;
        series_.clear();
        series_match_ = match_ ? match_->id : 0;
        series_games_ = wfc.competitive_games;
        game_recorded_ = false;
        series_done_ = series_match_ == 0 || series_games_ < 1;
    }
    if (in_series && !series_done_)
    {
        const int state = presence->game_state;
        if (state == kGameEnd && !game_recorded_ && (wfc.local_side == 0 || wfc.local_side == 1))
        {
            const int mine = wfc.local_side, theirs = 1 - mine;
            MatchGame game;
            game.my_score = presence->scores[mine];
            game.opponent_score = presence->scores[theirs];
            game.my_captain = CaptainKey(presence->teams[mine]);
            game.opponent_captain = CaptainKey(presence->teams[theirs]);
            game.stadium = StadiumKey(presence->stadium);
            game_recorded_ = true;
            if (!game.my_captain.empty() && !game.opponent_captain.empty() && !game.stadium.empty())
            {
                series_.push_back(game);
                int won = 0, lost = 0;
                for (const auto& played : series_)
                {
                    won += played.my_score > played.opponent_score;
                    lost += played.my_score < played.opponent_score;
                }
                const int needed = series_games_ / 2 + 1;
                if (won >= needed || lost >= needed || int(series_.size()) >= series_games_)
                {
                    reports_.push_back({series_match_, series_});
                    ++unsent_;
                    series_done_ = true;
                    wake = true;
                }
            }
        }
        else if (state == kGamePreGame || state == kGameKickoff || state == kGamePlay)
            game_recorded_ = false; // the series' next game
    }
    else if (!series_done_ && !(presence && presence->in_match))
    {
        // Left before the series ended: what was played, if anything.
        if (!series_.empty())
        {
            reports_.push_back({series_match_, series_});
            ++unsent_;
        }
        series_done_ = true;
        wake = true;
    }

    const bool changed = rules_version_ != rules_seen_;
    if (changed)
    {
        rules_seen_ = rules_version_;
        rules = rules_;
    }

    // A set's game: done once over, failed or called off.
    if (set_)
    {
        if (wfc.set_stage != set_stage_)
        {
            set_stage_ = wfc.set_stage;
            set_popup_ = wfc.set_popup;
            wake = true;
        }
        const bool done = set_stage_ == CHARGED_SET_OVER || set_stage_ == CHARGED_SET_FAILED || set_called_off_;
        if (done && set_done_ == Clock::time_point{})
        {
            set_done_ = Clock::now();
            wake = true;
        }
    }
    lock.unlock();
    if (wake) wake_.notify_all();
    return changed;
}

void CompetitiveHost::PlaySet(std::int64_t set)
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        set_ = set;
        next_match_ = {};
    }
    wake_.notify_all();
    Said("set " + std::to_string(set) + ": the game goes straight to RANKED against the set's opponent");
}

bool CompetitiveHost::CloseNow()
{
    std::string why;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!set_ || set_closed_ || set_done_ == Clock::time_point{}) return false;
        // The last report out, and a set left without one waiting again.
        const bool reported = unsent_ == 0 && reports_.empty();
        const bool settled = set_stage_ == CHARGED_SET_OVER ? reported : set_unready_ || set_stage_ == CHARGED_SET_OVER;
        if (!settled && Clock::now() - set_done_ < kCloseWait) return false;
        set_closed_ = true;
        why = set_stage_ == CHARGED_SET_OVER   ? "the series is over"
            : set_stage_ == CHARGED_SET_FAILED ? "it stopped at popup " + std::to_string(set_popup_)
                                               : "the set was called off";
    }
    Said("set " + std::to_string(set_) + ": the game closes, " + why);
    return true;
}

void CompetitiveHost::Run()
{
    std::unique_lock<std::mutex> lock(mutex_);
    while (!stop_)
    {
        auto cancel = std::make_shared<HttpCancel>();
        cancel_ = cancel;
        lock.unlock();
        auto next = Clock::now() + kRetry;
        try
        {
            ProfileService api(server_, token_);
            api.SetCancel(cancel);
            Work(api);
            // The next match check, or the next ready call while one can be
            // made; a new login or a series to report wakes the thread sooner.
            std::lock_guard<std::mutex> relock(mutex_);
            const bool ready = match_ && rules_ && proven_profile_ && proven_profile_ == login_profile_;
            next = ready ? std::min(next_match_, next_ready_) : next_match_;
        }
        catch (const ProfileError& e)
        {
            if (e.Cancelled()) return;
            if (e.SignedOut())
            {
                Said("signed out (the session ended); sign in again on the launcher's Competitive page");
                std::lock_guard<std::mutex> relock(mutex_);
                if (rules_)
                {
                    rules_.reset();
                    ++rules_version_;
                }
                return;
            }
            Said(e.what());
        }
        catch (const std::exception& e)
        {
            Said(e.what());
        }
        lock.lock();
        cancel_.reset();
        if (stop_) break;
        // Woken early by a new login or a series to report.
        wake_.wait_until(lock, next);
    }
}

void CompetitiveHost::Work(ProfileService& api)
{
    const auto now = Clock::now();
    bool fetch, report;
    unsigned profile;
    std::string ticket;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        fetch = now >= next_match_;
        report = !reports_.empty();
        profile = login_profile_;
        ticket = login_ticket_;
    }
    // The match the player accepted: its rules for the module.
    if (fetch || report)
    {
        auto current = api.CurrentMatch();
        if (current && current->state != "accepted") current.reset();
        std::optional<ChargedCompetitiveRules> rules;
        if (current)
        {
            std::string error;
            rules = ModuleRules(current->rules, current->opponent, &error);
            if (!rules) Said(error);
        }
        std::unique_lock<std::mutex> lock(mutex_);
        const bool news = (current ? current->id : 0) != (match_ ? match_->id : 0);
        match_ = current;
        if (!SameRules(rules, rules_))
        {
            rules_ = rules;
            ++rules_version_;
        }
        next_match_ = Clock::now() + kMatchEvery;
        if (news)
        {
            next_ready_ = {};
            paired_ = false;
        }
        lock.unlock();
        if (news && current)
            Said("match " + std::to_string(current->id) + " against " + current->opponent.name + " ("
                 + current->rule_set + "): " + DescribeRules(current->rules));
        else if (news)
            Said("no accepted match: RANKED stays retail");
        // A set's game whose set no longer waits for it (the opponent took
        // Ready to play! back, the games never met, the set ended) before
        // the games met.
        std::lock_guard<std::mutex> relock(mutex_);
        if (set_ && set_stage_ < CHARGED_SET_PLAYING
            && (!current || current->id != set_ || current->stage == "open" || current->stage == "invited"))
            set_called_off_ = true;
    }

    // A set's game that stopped or was called off before a result: the
    // player's Ready to play! is taken back, so the set waits for both again.
    std::int64_t unready = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (set_ && !set_unready_ && (set_stage_ == CHARGED_SET_FAILED || set_called_off_) && unsent_ == 0)
            unready = set_;
    }
    if (unready)
    {
        try
        {
            api.ReadyToPlay(unready, false);
        }
        catch (const ProfileError& e)
        {
            // A set that ended or is playing: nothing to take back.
            if (e.Status() == 0) throw;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        set_unready_ = true;
    }

    // The game's WFC login: the proof makes its friend code the player's.
    bool prove;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        prove = profile && !ticket.empty() && !(profile == proven_profile_ && ticket == proven_ticket_)
            && ticket != refused_ticket_;
    }
    if (prove)
    {
        try
        {
            const auto proven = api.ProveWfcLogin(profile, ticket);
            Said("WFC login proven: friend code " + proven.friend_code + " (" + proven.game + ")");
            std::lock_guard<std::mutex> lock(mutex_);
            proven_profile_ = profile;
            proven_ticket_ = ticket;
        }
        catch (const ProfileError& e)
        {
            if (e.Status() != 403) throw;
            // Not this login's ticket (a login meanwhile): wait for the next.
            Said(std::string("the server did not take the WFC login: ") + e.what());
            std::lock_guard<std::mutex> lock(mutex_);
            refused_ticket_ = ticket;
        }
    }

    // Ready for the accepted match, as long as the game is online as a proven profile.
    std::optional<ProfileMatch> match;
    bool ready;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        match = match_;
        ready = match && rules_ && proven_profile_ && proven_profile_ == login_profile_ && Clock::now() >= next_ready_;
        profile = proven_profile_;
    }
    if (ready)
    {
        const auto answer = api.Ready(match->id, profile, match->rules_hash);
        bool paired_now;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            paired_now = answer.paired && !paired_;
            paired_ = answer.paired;
            next_ready_ = Clock::now() + kReadyEvery;
            // Paired: the opponent's friend codes now include its game's.
            if (paired_now) next_match_ = {};
        }
        if (answer.other_release) Said("the opponent plays another release of the game: no match possible");
        else if (paired_now) Said("both games are ready: choose RANKED");
    }

    // The series played.
    std::vector<Report> reports;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        reports.swap(reports_);
    }
    for (std::size_t n = 0; n < reports.size(); ++n)
    {
        try
        {
            const auto settled = api.Report(reports[n].match, reports[n].games);
            std::string result = settled.state;
            if (settled.state == "finished")
                result = settled.winner == "you" ? "you won" : settled.winner == "opponent" ? "you lost" : "a draw";
            else if (settled.state == "accepted")
                result = "waiting for the opponent's report";
            Said("series of match " + std::to_string(reports[n].match) + " reported: " + result);
        }
        catch (const ProfileError& e)
        {
            // Kept for the next try unless the server refused it for good.
            if (e.Status() == 0)
            {
                std::lock_guard<std::mutex> lock(mutex_);
                reports_.insert(reports_.begin(), reports.begin() + std::ptrdiff_t(n), reports.end());
                throw;
            }
            Said(std::string("the result was not taken: ") + e.what());
        }
        std::lock_guard<std::mutex> lock(mutex_);
        if (unsent_ > 0) --unsent_;
        next_match_ = {};
    }
}
} // namespace mscharged::platform
