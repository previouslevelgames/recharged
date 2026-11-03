#include "platform/competitive_host.h"

#include "platform/http_client.h"
#include "platform/match_kind.h"

#include <algorithm>
#include <cstring>
#include <iterator>

namespace mscharged::platform
{
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

namespace
{
// The server keeps a ready game for 30 seconds; the game says it every 10.
constexpr auto kReadyEvery = 10s;
constexpr auto kMatchEvery = 10s;
// In the game's menus a challenge reaches the card sooner.
constexpr auto kMenuMatchEvery = 4s;
// A set's game on its way to the opponent's checks the set often: when the
// opponent leaves it, this game leaves it within seconds.
constexpr auto kSetWaitEvery = 2s;
// A closing game takes this long at most to take the player's Ready to play!
// back.
constexpr auto kWithdrawWait = 2s;
constexpr auto kRetry = 15s;
// cGame's eGameState.
constexpr int kGamePreGame = 0, kGameKickoff = 1, kGameEnd = 3, kGameUnloading = 4, kGamePlay = 5;
// GameInfoManager::eGameMode: the title screen's demo match.
constexpr int kModeDemo = 2;
// A set's game waits this long for its last report before it ends the set
// anyway.
constexpr auto kEndWait = 20s;

// The launcher's GoKey: each go of a set (it can go again after a Ready to
// play! taken back) has its own.
std::int64_t GoKey(const ProfileMatch& match) { return match.go_at ? match.go_at : match.id; }

// The log's words for a set's stage (ChargedSetStage).
std::string StageText(int stage, int popup)
{
    switch (stage)
    {
    case CHARGED_SET_BOOT: return "on its way (title, main menu or online menu)";
    case CHARGED_SET_MII: return "the main menu went online";
    case CHARGED_SET_OVERLAY: return "the Mii is chosen";
    case CHARGED_SET_LOGIN: return "logging in to WFC";
    case CHARGED_SET_SEARCH: return "searching RANKED for the opponent";
    case CHARGED_SET_PLAYING: return "the match is set up";
    case CHARGED_SET_OVER: return "back from the match";
    case CHARGED_SET_FAILED: return "stopped at popup " + std::to_string(popup);
    case CHARGED_SET_LEFT: return "the player left the search (Back)";
    default: return "stage " + std::to_string(stage);
    }
}

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
    if (!session || session->server != *server) return fail("not signed in (the launcher's Online pages)");
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
        std::unique_lock<std::mutex> lock(mutex_);
        stop_ = true;
        // A set still waiting for this game: the opponent's game hears at once
        // that this one is gone, not only once the server gives up on it.
        withdraw_on_stop_ = set_ && !set_unready_ && set_stage_ != CHARGED_SET_PLAYING && set_stage_ != CHARGED_SET_OVER;
        // During its match: the series with the game in play broken off, so
        // the server settles the set now and both players are free.
        if (!series_done_ && series_match_ != 0 && playing_)
        {
            auto games = series_;
            games.push_back(*playing_);
            reports_.push_back({series_match_, games});
            series_done_ = true;
        }
        if (withdraw_on_stop_ || !reports_.empty())
        {
            wake_.notify_all();
            wake_.wait_for(lock, kWithdrawWait, [this] { return withdrawn_; });
        }
        if (cancel_) cancel_->Cancel();
    }
    wake_.notify_all();
    worker_.join();
}

void CompetitiveHost::Kick()
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        kick_ = true;
    }
    wake_.notify_all();
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
        series_time_ = rules_ && rules_->limit == 0 ? rules_->time : 0;
        game_recorded_ = false;
        series_done_ = series_match_ == 0 || series_games_ < 1;
        playing_.reset();
        series_online_before_ = online_known_ ? online_.series : 0;
    }
    if (in_series && !series_done_)
    {
        const int state = presence->game_state;
        if (state != kGameEnd && state >= 0 && state != kGameUnloading && (wfc.local_side == 0 || wfc.local_side == 1))
        {
            const int mine = wfc.local_side, theirs = 1 - mine;
            MatchGame game;
            game.my_score = presence->scores[mine];
            game.opponent_score = presence->scores[theirs];
            game.my_captain = CaptainKey(presence->teams[mine]);
            game.opponent_captain = CaptainKey(presence->teams[theirs]);
            game.stadium = StadiumKey(presence->stadium);
            game.interrupted = true;
            game.played = series_time_ > 0 ? std::clamp(presence->game_time / float(series_time_), 0.0f, 1.0f) : 0.0f;
            if (!game.my_captain.empty() && !game.opponent_captain.empty() && !game.stadium.empty()) playing_ = game;
        }
        if (state == kGameEnd && !game_recorded_ && (wfc.local_side == 0 || wfc.local_side == 1))
        {
            playing_.reset();
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
                    TakeOnlineGames();
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
        // Left before the series ended (or its last end screen went by
        // unseen): what was played, if anything, and a game that broke off.
        TakeOnlineGames();
        if (!series_.empty())
        {
            reports_.push_back({series_match_, series_});
            ++unsent_;
        }
        series_done_ = true;
        wake = true;
    }

    // A match (not the title screen's demo) keeps challenges away: the
    // server hears of it at once.
    if (presence)
    {
        const char* activity = presence->in_match && presence->mode != kModeDemo ? "match" : "game";
        const char* kind = MatchKindOf(*presence);
        if (activity_ != activity)
        {
            activity_ = activity;
            next_match_ = {};
            wake = true;
        }
        // A new kind says so with the next heartbeat.
        match_kind_ = kind ? kind : "";
    }

    const bool changed = rules_version_ != rules_seen_;
    if (changed)
    {
        rules_seen_ = rules_version_;
        rules = rules_;
    }

    // A set's game: done once over, failed or called off.
    std::string stage_line;
    if (set_)
    {
        if (wfc.set_stage != set_stage_)
        {
            set_stage_ = wfc.set_stage;
            set_popup_ = wfc.set_popup;
            wake = true;
            stage_line = "set " + std::to_string(set_) + ": " + StageText(set_stage_, set_popup_);
        }
        const bool done = set_stage_ == CHARGED_SET_OVER || set_stage_ == CHARGED_SET_FAILED
                       || set_stage_ == CHARGED_SET_LEFT || set_called_off_;
        if (done && set_done_ == Clock::time_point{})
        {
            set_done_ = Clock::now();
            wake = true;
        }
    }
    lock.unlock();
    if (wake) Kick();
    if (!stage_line.empty()) Said(stage_line);
    return changed;
}

void CompetitiveHost::PlaySet(std::int64_t set)
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        set_ = set;
        next_match_ = {};
    }
    Kick();
    Said("set " + std::to_string(set) + ": the game goes straight to RANKED against the set's opponent");
}

std::optional<CompetitiveHost::SetStatus> CompetitiveHost::CurrentSet()
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (!set_) return std::nullopt;
    if (match_ && match_->id == set_)
    {
        set_opponent_ = match_->opponent.name;
        set_opponent_avatar_ = match_->opponent.avatar_url;
        set_games_ = match_->rules.games;
    }
    if (set_opponent_.empty()) return std::nullopt;
    SetStatus status;
    status.opponent = set_opponent_;
    status.games = set_games_;
    // The games this game counted (the game's thread keeps them).
    if (series_match_ == set_)
        for (const auto& game : series_)
        {
            status.won += game.my_score > game.opponent_score;
            status.lost += game.my_score < game.opponent_score;
        }
    return status;
}

std::optional<CompetitiveHost::SetEnd> CompetitiveHost::TakeSetEnd()
{
    SetEnd end;
    std::string why;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!set_ || set_done_ == Clock::time_point{}) return std::nullopt;
        // The last report out, and a set left without one waiting again.
        const bool reported = unsent_ == 0 && reports_.empty();
        // A played series: also the server's result (both reports), so a
        // rematch can be asked at once.
        const bool final = !match_ || match_->id != set_ || match_->state != "accepted";
        const bool settled = set_stage_ == CHARGED_SET_OVER ? reported && final
                           : set_unready_ || (set_called_off_ && called_off_how_ == SetEnd::How::Replaced);
        if (!settled && Clock::now() - set_done_ < kEndWait) return std::nullopt;
        if (match_ && match_->id == set_)
        {
            set_opponent_ = match_->opponent.name;
            set_opponent_avatar_ = match_->opponent.avatar_url;
            set_games_ = match_->rules.games;
        }
        end.match = set_;
        end.opponent = set_opponent_;
        end.opponent_avatar = set_opponent_avatar_;
        end.games = set_games_;
        if (series_match_ == set_)
            for (const auto& game : series_)
            {
                end.won += game.my_score > game.opponent_score;
                end.lost += game.my_score < game.opponent_score;
            }
        // Back from the match with no game played: the server dissolves it.
        const bool played = series_match_ == set_ && !series_.empty();
        end.how = set_stage_ == CHARGED_SET_OVER   ? (played ? SetEnd::How::Over : SetEnd::How::NoResult)
                : set_stage_ == CHARGED_SET_FAILED ? SetEnd::How::Failed
                : set_stage_ == CHARGED_SET_LEFT   ? SetEnd::How::Left
                                                   : called_off_how_;
        end.popup = set_popup_;
        // Ready to play! again only for a set the server keeps open.
        end.can_play_again = match_ && match_->id == set_ && match_->stage == "open";
        why = end.how == SetEnd::How::Over         ? "the series is over"
            : end.how == SetEnd::How::NoResult     ? "the match ended without a game played"
            : end.how == SetEnd::How::Failed       ? "it stopped at popup " + std::to_string(set_popup_)
            : end.how == SetEnd::How::Left         ? "the player left its search"
            : end.how == SetEnd::How::OpponentLeft ? "the opponent left it"
            : end.how == SetEnd::How::OpponentGone ? "the opponent closed Recharged"
            : end.how == SetEnd::How::Replaced     ? "another game of this player took it"
                                                   : "the set was called off";
        // No set's game any more: challenges reach the card again.
        set_ = 0;
        set_stage_ = CHARGED_SET_NONE;
        set_popup_ = -1;
        set_called_off_ = set_unready_ = false;
        called_off_how_ = SetEnd::How::CalledOff;
        set_done_ = {};
        paired_ = false;
        next_match_ = {};
    }
    Kick();
    Said("set " + std::to_string(end.match) + " is done, " + why + "; the game stays open");
    return end;
}

std::optional<CompetitiveHost::SetStart> CompetitiveHost::TakeSetStart()
{
    std::lock_guard<std::mutex> lock(mutex_);
    auto start = std::move(set_start_);
    set_start_.reset();
    // A set this game already plays (the launcher's).
    if (set_) return std::nullopt;
    return start;
}

void CompetitiveHost::ObserveOnline(const ChargedOnlineMatchSnapshot& online)
{
    online_ = online;
    online_known_ = true;
}

void CompetitiveHost::TakeOnlineGames()
{
    // This series' own record: begun after it was set up, holding at least
    // the games counted here.
    if (!online_known_ || online_.series <= series_online_before_ || online_.games < 1
        || online_.games > int(std::size(online_.game)) || std::size_t(online_.games) < series_.size())
        return;
    const int mine = online_.side == 1 ? 1 : 0, theirs = 1 - mine;
    std::vector<MatchGame> games;
    for (int index = 0; index < online_.games; ++index)
    {
        const ChargedOnlineGame& seen = online_.game[index];
        MatchGame game;
        game.my_score = seen.scores[mine];
        game.opponent_score = seen.scores[theirs];
        game.my_captain = CaptainKey(seen.captains[mine]);
        game.opponent_captain = CaptainKey(seen.captains[theirs]);
        game.stadium = StadiumKey(seen.stadium);
        if (game.my_captain.empty() || game.opponent_captain.empty() || game.stadium.empty()) return;
        if (seen.ending == 2 || seen.ending == 4)
        {
            // Only the last game can have broken off.
            if (index != online_.games - 1) return;
            game.interrupted = true;
            game.played = series_time_ > 0 ? std::clamp(seen.seconds / float(series_time_), 0.0f, 1.0f) : 0.0f;
        }
        games.push_back(game);
    }
    for (std::size_t index = 0; index < games.size() && log_; ++index)
    {
        const MatchGame& game = games[index];
        const std::string score = std::to_string(game.my_score) + "-" + std::to_string(game.opponent_score);
        if (game.interrupted)
            log_("Competitive: the series' game " + std::to_string(index + 1) + " broke off at " + score + " ("
                 + std::to_string(int(game.played * 100.0f + 0.5f)) + "% of its clock)");
        else if (index >= series_.size())
            log_("Competitive: the series' game " + std::to_string(index + 1) + " ended " + score
                 + " (its end screen went by unseen)");
    }
    series_ = std::move(games);
}

void CompetitiveHost::CallOff(std::int64_t match)
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        call_off_match_ = match;
    }
    Kick();
}

void CompetitiveHost::PlayAgain(std::int64_t match, bool rematch)
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        again_match_ = match;
        again_rematch_ = rematch;
    }
    Kick();
}

bool CompetitiveHost::ReadyDue() const
{
    // Only a set's game says it is ready for its set (that counts as the
    // player's Ready to play!); before sets, any game for the accepted match.
    return match_ && rules_ && proven_profile_ && proven_profile_ == login_profile_
        && (match_->stage.empty()
            || (match_->id == set_ && !set_called_off_ && !set_unready_ && set_stage_ != CHARGED_SET_FAILED
                && set_stage_ != CHARGED_SET_OVER && set_stage_ != CHARGED_SET_LEFT));
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
            next = ReadyDue() ? std::min(next_match_, next_ready_) : next_match_;
        }
        catch (const ProfileError& e)
        {
            if (e.Cancelled()) return;
            if (e.SignedOut())
            {
                Said("signed out (the session ended); sign in again on the launcher's Online pages");
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
        // Woken early by new work (kick_), or at the next due call.
        wake_.wait_until(lock, next, [this] { return stop_ || kick_; });
        kick_ = false;
    }
    // The game closes during its set's way to the opponent's: the set is
    // called off, so the opponent's game leaves it now and both are free.
    // During the match or after it: the series goes out now, with the game
    // the close broke off, so the server settles the set at once.
    std::vector<Report> reports;
    reports.swap(reports_);
    const bool withdraw = withdraw_on_stop_;
    if (!withdraw && reports.empty()) return;
    const std::int64_t set = set_;
    auto cancel = std::make_shared<HttpCancel>();
    cancel_ = cancel;
    lock.unlock();
    try
    {
        ProfileService api(server_, token_);
        api.SetCancel(cancel);
        if (withdraw)
        {
            api.Cancel(set);
            Said("set " + std::to_string(set) + ": the game closes; the set is called off");
        }
        for (const auto& report : reports)
        {
            const auto settled = api.Report(report.match, report.games);
            Said("set " + std::to_string(report.match) + ": the game closes; its series is reported ("
                 + settled.state + ")");
        }
    }
    catch (const std::exception&)
    {
        // Too late or refused: the server gives up on the game by itself.
    }
    lock.lock();
    withdrawn_ = true;
    wake_.notify_all();
}

void CompetitiveHost::Work(ProfileService& api)
{
    const auto now = Clock::now();
    // The card's answer. An accept goes on with the set in this game (the
    // match check below sees it go).
    std::int64_t answer = 0, again = 0;
    bool accept = false, rematch = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        api.SetActivity(activity_, match_kind_);
        answer = answer_match_;
        accept = answer_accept_;
        answer_match_ = 0;
        again = again_match_;
        rematch = again_rematch_;
        again_match_ = 0;
    }
    std::int64_t call_off = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        call_off = call_off_match_;
        call_off_match_ = 0;
    }
    if (call_off)
    {
        try
        {
            api.Cancel(call_off);
            Said("called off match " + std::to_string(call_off) + " (Not now)");
        }
        catch (const ProfileError& e)
        {
            // Ended already, or played: nothing to call off.
            if (e.Cancelled()) throw;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        next_match_ = {};
    }
    if (answer)
    {
        std::optional<std::string> notice;
        try
        {
            const auto answered = accept ? api.Accept(answer) : api.Decline(answer);
            Said(std::string(accept ? "accepted" : "declined") + " the challenge of match " + std::to_string(answer)
                 + " in the game (" + answered.stage + ")");
        }
        catch (const ProfileError& e)
        {
            if (e.Cancelled()) throw;
            Said(std::string("the answer to match ") + std::to_string(answer) + " was not taken: " + e.what());
            notice = e.Status() == 0 ? std::string("The server didn't answer. Try again on the launcher's Online pages.")
                                     : std::string("The challenge is no longer open.");
        }
        std::lock_guard<std::mutex> lock(mutex_);
        invitation_.reset();
        if (notice) notice_ = notice;
        next_match_ = {};
    }
    // The set again, after it ended in this game: a rematch, or Ready to
    // play! again.
    if (again)
    {
        std::optional<std::string> notice;
        try
        {
            const auto asked = rematch ? api.Rematch(again) : api.ReadyToPlay(again, true);
            Said((rematch ? "asked for a rematch of match " : "ready again for match ") + std::to_string(again)
                 + " in the game (match " + std::to_string(asked.id) + ", " + asked.stage + ")");
        }
        catch (const ProfileError& e)
        {
            if (e.Cancelled()) throw;
            Said(std::string(rematch ? "the rematch of match " : "ready again for match ") + std::to_string(again)
                 + " was not taken: " + e.what());
            notice = e.Status() == 0 ? std::string("The server didn't answer. Try again on the launcher's Online pages.")
                                     : std::string(rematch ? "No rematch: the set is no longer there."
                                                           : "The set is no longer open.");
        }
        std::lock_guard<std::mutex> lock(mutex_);
        if (notice) notice_ = notice;
        next_match_ = {};
    }

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
        // A challenge to this player: the card's, while the game is not a set's.
        std::optional<Invitation> invitation;
        if (current && current->state == "challenged" && current->side == "challenged")
            invitation = Invitation{current->id, current->opponent.name, current->opponent.avatar_url,
                                    current->rule_set + ": " + DescribeRules(current->rules),
                                    current->rematch_of != 0, current->random};
        // This game's set as the server ended it (the opponent called it off,
        // or closed Recharged), before the match it no longer is goes.
        std::string set_ended;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (current && set_ && current->id == set_ && current->state != "accepted") set_ended = current->state;
        }
        if (current && current->state != "accepted") current.reset();
        std::optional<ChargedCompetitiveRules> rules;
        if (current)
        {
            std::string error;
            rules = ModuleRules(current->rules, current->opponent, &error);
            if (!rules) Said(error);
        }
        std::unique_lock<std::mutex> lock(mutex_);
        invitation_ = set_ ? std::nullopt : invitation;
        const bool news = (current ? current->id : 0) != (match_ ? match_->id : 0);
        match_ = current;
        if (!SameRules(rules, rules_))
        {
            rules_ = rules;
            ++rules_version_;
        }
        next_match_ = Clock::now()
                    + (set_ && set_stage_ <= CHARGED_SET_SEARCH ? kSetWaitEvery
                       : activity_ == "game" && !set_          ? kMenuMatchEvery
                                                               : kMatchEvery);
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
        if (set_ && set_stage_ < CHARGED_SET_PLAYING && !set_called_off_
            && (!current || current->id != set_ || current->stage == "open" || current->stage == "invited"))
        {
            set_called_off_ = true;
            // The opponent's doing, as far as the server shows it: Recharged
            // closed, or their Ready to play! taken back (their Back, their
            // launcher) while this player's still counts.
            called_off_how_ = set_ended == "cancelled" ? SetEnd::How::OpponentLeft
                            : set_ended == "dissolved" ? SetEnd::How::OpponentGone
                            : !current || current->id != set_ ? SetEnd::How::CalledOff
                            : !current->opponent_online       ? SetEnd::How::OpponentGone
                            : current->you_ready && !current->opponent_ready ? SetEnd::How::OpponentLeft
                                                                             : SetEnd::How::CalledOff;
        }
        // A set that went: this game plays it, unless it plays one already
        // (then that go counts as started) or is in a match.
        if (current && current->stage == "go")
        {
            const auto key = GoKey(*current);
            if (set_ == current->id) started_go_ = key;
            else if (!set_ && activity_ == "game" && key != started_go_)
            {
                started_go_ = key;
                set_start_ = SetStart{current->id, current->opponent.name};
            }
        }
    }

    // A set's game that stopped or was called off before a result: the
    // player's Ready to play! is taken back, so the set waits for both again;
    // a player who left its search calls it off, which frees both players.
    std::int64_t unready = 0;
    bool left = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (set_ && !set_unready_
            && (set_stage_ == CHARGED_SET_FAILED || set_stage_ == CHARGED_SET_LEFT || set_called_off_) && unsent_ == 0)
            unready = set_;
        left = set_stage_ == CHARGED_SET_LEFT;
    }
    if (unready)
    {
        try
        {
            if (left) api.Cancel(unready);
            else api.ReadyToPlay(unready, false);
        }
        catch (const ProfileError& e)
        {
            // A set that ended or is playing: nothing to take back.
            if (e.Status() == 0) throw;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        if (set_ == unready) set_unready_ = true;
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
        ready = ReadyDue() && Clock::now() >= next_ready_;
        profile = proven_profile_;
    }
    if (ready)
    {
        ReadyAnswer answer;
        try
        {
            answer = api.Ready(match->id, profile, match->rules_hash);
        }
        catch (const ProfileError& e)
        {
            if (e.Cancelled()) throw;
            Said(std::string("the server did not take the game's ready: ") + e.what());
            std::lock_guard<std::mutex> lock(mutex_);
            next_ready_ = Clock::now() + kReadyEvery;
            answer = {};
        }
        bool paired_now;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            paired_now = answer.paired && !paired_;
            paired_ = answer.paired;
            next_ready_ = Clock::now() + kReadyEvery;
            // Paired: the opponent's friend codes now include its game's.
            if (paired_now) next_match_ = {};
        }
        // Another game of this player's took the set (a second Recharged):
        // this one leaves it, and nobody meets it in RANKED meanwhile.
        if (answer.replaced)
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (match->id == set_ && !set_called_off_)
            {
                set_called_off_ = true;
                called_off_how_ = SetEnd::How::Replaced;
                if (set_done_ == Clock::time_point{}) set_done_ = Clock::now();
            }
        }
        if (answer.replaced) Said("another game of this player took the set");
        else if (answer.other_release) Said("the opponent plays another release of the game: no match possible");
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
            // A server before 1.2.8 doesn't know a game that broke off (it
            // answers 422 for the unknown fields): the series without it.
            auto& games = reports[n].games;
            if ((e.Status() == 400 || e.Status() == 422) && !games.empty() && games.back().interrupted)
            {
                games.pop_back();
                if (!games.empty())
                {
                    --n;
                    continue;
                }
            }
            Said(std::string("the result was not taken: ") + e.what());
        }
        std::lock_guard<std::mutex> lock(mutex_);
        if (unsent_ > 0) --unsent_;
        next_match_ = {};
    }
}
std::optional<CompetitiveHost::Invitation> CompetitiveHost::PendingInvitation()
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (!invitation_ || set_ || activity_ != "game" || invitation_->match == answered_match_)
        return std::nullopt;
    return invitation_;
}

void CompetitiveHost::Answer(std::int64_t match, bool accept)
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        answer_match_ = answered_match_ = match;
        answer_accept_ = accept;
    }
    Kick();
}

bool CompetitiveHost::InMatch()
{
    std::lock_guard<std::mutex> lock(mutex_);
    return activity_ == "match";
}

std::optional<std::string> CompetitiveHost::TakeNotice()
{
    std::lock_guard<std::mutex> lock(mutex_);
    auto notice = std::move(notice_);
    notice_.reset();
    return notice;
}
} // namespace mscharged::platform
