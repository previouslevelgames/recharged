#pragma once

#include "platform/competitive_rules.h"
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
    CompetitiveHost& operator=(const CompetitiveHost&) = delete;

    const std::string& PlayerName() const { return player_; }

    // On the game's thread after a frame (once a second is enough): what the
    // game reports. True when the module's rules changed: `rules` is then
    // what it should have (nullopt: none, RANKED stays retail).
    bool Observe(const ChargedWfcSnapshot& wfc, const ChargedPresenceSnapshot* presence,
                 std::optional<ChargedCompetitiveRules>& rules);

    // This game is set `set`'s (the launcher's Ready to play!): it starts
    // itself into the set's RANKED search (the module's set stages). A set it
    // leaves without a result goes back to waiting for Ready to play!.
    void PlaySet(std::int64_t set);
    // On the game's thread after Observe: the set's game is done (the series
    // is over and reported, the set failed, or the server called it off).
    // The caller closes the game; true once.
    bool CloseNow();

private:
    CompetitiveHost(std::string server, std::string token, std::string player, Log log);
    void Run();
    void Work(ProfileService& api);
    void Said(const std::string& line);

    const std::string server_, token_, player_;
    const Log log_;
    std::mutex mutex_;
    std::condition_variable wake_;
    bool stop_ = false;
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
    bool set_called_off_ = false, set_unready_ = false, set_closed_ = false;
    std::chrono::steady_clock::time_point set_done_{};

    // The game's thread only.
    std::uint64_t rules_seen_ = 0;
    unsigned series_setup_ = 0; // ChargedWfcSnapshot::competitive_setups of the series
    std::int64_t series_match_ = 0;
    int series_games_ = 0;
    std::vector<MatchGame> series_;
    bool game_recorded_ = false, series_done_ = true;
};
} // namespace mscharged::platform
