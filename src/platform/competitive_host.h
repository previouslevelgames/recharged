#pragma once

#include "platform/competitive_rules.h"
#include "platform/online_match_snapshot.h"
#include "platform/presence_snapshot.h"
#include "platform/profile_service.h"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace mscharged::platform
{
// The game's side of competitive play, in the host: with the session the
// launcher signed in for this settings file, it proves the game's WFC login
// to the profile service, says the game is ready for the player's accepted
// match (the server then keeps it for the opponent in RANKED), gives the game
// module the match's rules and reports the series it played. Its calls run
// on a thread of their own; the game loop calls Observe() after a frame.
class CompetitiveHost
{
public:
    using Log = std::function<void(const std::string&)>;

    // Nothing without online.profile_server or a session for it (the reason
    // goes to `off`).
    static std::unique_ptr<CompetitiveHost> Start(const std::string& server_setting,
                                                  const std::filesystem::path& session_file, Log log,
                                                  std::string* off);
    ~CompetitiveHost();
    CompetitiveHost(const CompetitiveHost&) = delete;
    // The game powers off: the process ends without unwinding. The last calls
    // (a set called off, a series reported) get their time, then this waits
    // at most `wait` for the worker to end; one still busy (a call that
    // won't give up) is left to the process's end instead of being joined.
    // True when it ended.
    bool StopForExit(std::chrono::milliseconds wait);
    CompetitiveHost& operator=(const CompetitiveHost&) = delete;

    const std::string& PlayerName() const { return player_; }

    // On the game's thread after a frame (once a second is enough): what the
    // game reports. True when the module's rules changed: `rules` is then
    // what it should have (nullopt: none, RANKED stays retail).
    bool Observe(const ChargedWfcSnapshot& wfc, const ChargedPresenceSnapshot* presence,
                 std::optional<ChargedCompetitiveRules>& rules);

    // This game is set `set`'s (the launcher's Ready to play!, or a
    // TakeSetStart): it starts itself into the set's RANKED search (the
    // module's set stages). A set it leaves without a result goes back to
    // waiting for Ready to play!.
    void PlaySet(std::int64_t set);
    // On the game's thread after Observe: a set that went (both are ready:
    // the card's accept, a rematch) while this game is in its menus and
    // plays none. The caller hands the module the player's Mii and calls
    // PlaySet: the game goes on into RANKED without closing. Once per go.
    struct SetStart
    {
        std::int64_t match = 0;
        std::string opponent;
    };
    std::optional<SetStart> TakeSetStart();
    // On the game's thread after Observe: the set this game played is done
    // (the series is over and reported, the set failed, or the server called
    // it off); the game is no set's any more and stays where it is (the
    // caller ends the module's set). Once.
    struct SetEnd
    {
        enum class How
        {
            Over,         // the series was played
            NoResult,     // back from the match without a game played (one left)
            Failed,       // a popup ended it (`popup`)
            CalledOff,    // the set stopped waiting for this game
            Left,         // this player left its search (Back)
            OpponentLeft, // the opponent left its search or took their Ready to play! back
            OpponentGone, // the opponent closed Recharged
            Replaced      // another game of this player took the set
        };
        std::int64_t match = 0;
        std::string opponent;
        std::string opponent_avatar; // their picture (URL), or empty
        int won = 0, lost = 0, games = 0; // the games this game counted
        How how = How::Over;
        int popup = -1;
        bool can_play_again = false; // the server keeps it open for Ready to play!
    };
    std::optional<SetEnd> TakeSetEnd();
    // After a SetEnd: the same set again. `rematch`: a new set by its rules
    // (the opponent's game asks its player); otherwise the player's Ready to
    // play! again for a set that stopped.
    void PlayAgain(std::int64_t match, bool rematch);
    // After a SetEnd: the player doesn't play a stopped set again now; the
    // server calls it off, so neither player keeps it.
    void CallOff(std::int64_t match);
    // On the game's thread, before Observe: the game's online series
    // (charged_online_match_snapshot), for a game that broke off.
    void ObserveOnline(const ChargedOnlineMatchSnapshot& online);

    // On the game's thread after Observe: the set this game plays, for the
    // Discord status: the opponent, the games won and lost so far and how
    // many the series has; none outside a set's game.
    struct SetStatus
    {
        std::string opponent;
        int won = 0, lost = 0, games = 0;
    };
    std::optional<SetStatus> CurrentSet();

    // A challenge to this player while the game is in its menus (not in a
    // match, not a set's game): what the host's card asks, or none.
    struct Invitation
    {
        std::int64_t match = 0;
        std::string from;  // the challenger's name
        std::string avatar; // the challenger's picture (URL), or empty
        std::string rules; // the rule set and what it plays by
        bool rematch = false; // the set they played last, again
        bool random = false;  // their search for a random opponent asks this player
    };
    std::optional<Invitation> PendingInvitation();
    // The card's answer. An accept is also the player's Ready to play!: the
    // set then goes, and TakeSetStart() starts it in this game.
    void Answer(std::int64_t match, bool accept);
    // On the game's thread: the game plays a match (no cards or reminders).
    bool InMatch();
    // What the host shows in a message (an answer the server did not take),
    // once.
    std::optional<std::string> TakeNotice();

private:
    CompetitiveHost(std::string server, std::string token, std::string player, Log log);
    void Run();
    void Work(ProfileService& api);
    void Said(const std::string& line);
    // Wakes the worker for new work (no wake-up gets lost while it works).
    void Kick();
    // With mutex_ held: the game is due to say it is ready for match_.
    bool ReadyDue() const;

    const std::string server_, token_, player_;
    const Log log_;
    std::mutex mutex_;
    std::condition_variable wake_;
    bool stop_ = false, kick_ = false;
    // The game closes while its set waits for it: the worker's last call calls
    // the set off; during its match, its last call reports the series with
    // the game in play as broken off (withdrawn_ once they are through).
    bool withdraw_on_stop_ = false, withdrawn_ = false;
    bool finished_ = false; // the worker is done
    // Stops the worker: its last calls go out (or time out); no join.
    void Stop();
    std::shared_ptr<class HttpCancel> cancel_;
    std::thread worker_;

    // Shared with the worker.
    std::optional<ProfileMatch> match_; // the accepted match, as the server knows it
    std::optional<ChargedCompetitiveRules> rules_;
    std::uint64_t rules_version_ = 0;
    unsigned login_profile_ = 0; // the game's WFC login
    std::string login_ticket_;
    unsigned proven_profile_ = 0; // the login the server took as proof
    std::string proven_ticket_, refused_ticket_;
    bool paired_ = false;
    // What the game is doing, for the server's lists: "game", or "match"
    // from a match's loading until it is unloaded (no challenges then).
    std::string activity_ = "game";
    std::string match_kind_; // in a match: its kind (MatchKindOf)
    // A challenge to answer in the game's menus, the card's answer still to
    // send and the last one sent.
    std::optional<Invitation> invitation_;
    std::int64_t answer_match_ = 0, answered_match_ = 0;
    bool answer_accept_ = false;
    // A set that went for this game to start (TakeSetStart), the last go
    // started (go_at, or the set's id), and a PlayAgain still to send.
    std::optional<SetStart> set_start_;
    std::int64_t started_go_ = 0;
    std::int64_t again_match_ = 0;
    bool again_rematch_ = false;
    std::int64_t call_off_match_ = 0; // CallOff still to send
    std::optional<std::string> notice_;
    struct Report
    {
        std::int64_t match = 0;
        std::vector<MatchGame> games;
    };
    std::vector<Report> reports_; // series played, to report
    std::string last_said_;
    std::chrono::steady_clock::time_point next_match_{}, next_ready_{};

    // A set's game: the set, the module's stage of it (ChargedSetStage), the
    // series reports not out yet, whether the set was called off or its
    // Ready to play! taken back, and since when it is done.
    std::int64_t set_ = 0;
    int set_stage_ = 0;
    int set_popup_ = -1;
    int unsent_ = 0;
    bool set_called_off_ = false, set_unready_ = false;
    // Why it was called off, as the server showed the opponent then.
    SetEnd::How called_off_how_ = SetEnd::How::CalledOff;
    // The set's opponent and series length, kept once the server has gone
    // on from the set (for the Discord status of its last screens).
    std::string set_opponent_;
    std::string set_opponent_avatar_;
    int set_games_ = 0;
    std::chrono::steady_clock::time_point set_done_{};

    // The game's thread only.
    std::uint64_t rules_seen_ = 0;
    unsigned series_setup_ = 0; // ChargedWfcSnapshot::competitive_setups of the series
    std::int64_t series_match_ = 0;
    int series_games_ = 0;
    std::vector<MatchGame> series_;
    bool game_recorded_ = false, series_done_ = true;
    // The series' game length (a time limit), for a game that broke off.
    int series_time_ = 0;
    // The series' game in play as the last Observe saw it (score, clock):
    // broken off if this game closes during it.
    std::optional<MatchGame> playing_;
    // The game's online series (ObserveOnline): how each game ended.
    ChargedOnlineMatchSnapshot online_{};
    bool online_known_ = false;
    // Its number when this series was set up: this series' is a later one.
    unsigned series_online_before_ = 0;
    // With the series about to be reported: its games as the game reported
    // each end (its online series), when that holds at least the games
    // counted here. This host looks once a second and can miss a game's end
    // screen (a sync error right after the whistle unloads the match within
    // it); the last game is broken off when it ended by a lost connection or
    // out of sync.
    void TakeOnlineGames();
};
} // namespace mscharged::platform
