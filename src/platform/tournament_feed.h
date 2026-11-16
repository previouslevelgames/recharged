#pragma once

#include "platform/profile_service.h"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace mscharged::platform
{
// Of the profile server's tournaments, the one the TOURNAMENT page shows:
// a running one the player is in, else one open for entries they joined,
// else the newest open for entries, else the newest running, else the last
// that ended. Its index, none for an empty list.
std::optional<std::size_t> ChooseTournament(const std::vector<ProfileTournament>& tournaments);

// The TOURNAMENT page's tournament from the profile server, asked on a
// thread of its own so the game never waits: only while the page is open,
// at once when it opens, then every poll interval the server gives. The
// player's actions (join, leave, READY) go on the same thread, then the page
// is asked again. The session is read from its file at each round, so a
// sign-in while the game runs counts.
class TournamentFeed
{
public:
    using Clock = std::chrono::steady_clock;
    using Log = std::function<void(const std::string&)>;

    // Nothing without online.profile_server (the reason goes to `off`).
    static std::unique_ptr<TournamentFeed> Start(const std::string& server_setting,
                                                 std::filesystem::path session_file, Log log, std::string* off);
    ~TournamentFeed();
    TournamentFeed(const TournamentFeed&) = delete;
    TournamentFeed& operator=(const TournamentFeed&) = delete;

    // The page opened or closed: it asks only while open.
    void SetOpen(bool open);
    // The player's actions on the shown tournament (and their set).
    void Join();
    void Leave();
    void Ready(bool ready);

    struct Snapshot
    {
        bool loaded = false; // an answer came
        std::uint64_t version = 0;
        bool signed_in = false; // a session for this server
        std::vector<ProfileTournament> tournaments;
        std::optional<TournamentPage> page; // the one shown, kept when a later ask fails
        // The player's set's match while it is open: their READY and the
        // opponent's (GET /api/v1/matches/current).
        std::optional<ProfileMatch> match;
        std::string error;        // the last ask's, empty when it worked
        std::string action_error; // the last action's, until the next one
        bool busy = false;        // an action is on its way
        Clock::time_point fetched{};
    };
    Snapshot Latest() const;
    std::uint64_t Version() const;
    // The match the player's READY made or readied last (0: none): its set
    // goes on from the trophy room when it starts.
    std::int64_t ReadyMatch() const;

private:
    TournamentFeed(std::string server, std::filesystem::path session_file, Log log);
    enum class Action { None, Join, Leave, Ready, Unready };
    void Ask(Action action);
    void Run();
    void Round(Action action);

    std::string server_;
    std::filesystem::path sessionFile_;
    Log log_;
    mutable std::mutex mutex_;
    std::condition_variable wake_;
    bool open_ = false, stop_ = false;
    Action pending_ = Action::None;
    Snapshot snapshot_;
    std::int64_t readyMatch_ = 0;
    int pollSeconds_ = 5;
    std::thread thread_;
};
} // namespace mscharged::platform
