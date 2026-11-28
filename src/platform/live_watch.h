#pragma once

#include "platform/match_replay_file.h"
#include "platform/match_replay_status.h"
#include "platform/tcp_client.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

// Watching an online match as it is played: the WFC server's dashboard hands
// an admin a ticket (a replay file of version 2, its header naming the match),
// Recharged opens it like a replay and plays the match from the server's live
// stream (docs/reports.md of the server, "Live streams"), a moment behind the
// players. A thread of its own reads the stream; the game module plays it as
// it arrives (charged_configure_live_replay, charged_feed_live_replay).
namespace mscharged::platform
{
using WatchTicket = mscharged::replay::WatchTicket;
using mscharged::replay::ReadWatchTicket;

// A game's replay the server keeps (a ticket naming the game, from the
// dashboard's Replay button): the file as the game sent it from `server`
// (online.server), and the match's next game whose replay the server keeps
// (0 none). Waits at most `timeout`; false with `error` set.
bool FetchKeptReplay(const std::string& server, const WatchTicket& ticket, std::vector<std::uint8_t>& file,
                     int& next_game, std::string& error,
                     std::chrono::milliseconds timeout = std::chrono::seconds(30));

// Who plays a watched match and how far its set is, as the server says
// (GET /api/v1/wfc/streams/<match>/info, its docs/reports.md, "Watching"):
// what a spectator shows over the match.
struct WatchedMatch
{
    struct Side
    {
        std::string name;    // the Recharged player, else the Mii
        std::string country; // the player's flag: two capital letters, or empty
    };
    Side home, away;
    int best_of = 1;
    // The games that count so far: their numbers and goals (home, away).
    struct Game
    {
        int number = 0;
        int home = 0, away = 0;
    };
    std::vector<Game> games;

    // The games each side won before game `game` (home, away): a game's
    // replay shows the set as it stood when the game began.
    std::pair<int, int> WinsBefore(int game) const;
};
// The server's answer read; false with `error` set for one that is not one.
bool ReadWatchedMatch(const std::string& json, WatchedMatch& match, std::string& error);
// Asks `server` (online.server) with `ticket`; waits at most `timeout`. False
// with `error` set; `absent` too when the server has no such answer (one
// before 1.3.7: it says 404 to every match), which asking again won't change.
bool FetchWatchedMatch(const std::string& server, const WatchTicket& ticket, WatchedMatch& match, bool& absent,
                       std::string& error, std::chrono::milliseconds timeout = std::chrono::seconds(10));

class LiveWatch
{
public:
    using Log = std::function<void(const std::string& line)>;

    // Reads `ticket`'s match from `server` (online.server).
    LiveWatch(std::string server, WatchTicket ticket, Log log);
    ~LiveWatch();
    LiveWatch(const LiveWatch&) = delete;
    LiveWatch& operator=(const LiveWatch&) = delete;

    // Waits at most `timeout` for a game of the match whose start (its
    // stream's setup) has arrived: its number and the bytes so far. False
    // with `error` set otherwise.
    bool WaitForGame(std::chrono::milliseconds timeout, int& game, std::vector<std::uint8_t>& start,
                     std::string& error);
    // From now on the game's further bytes go to `feed` (the module's), from
    // the reader's thread; the bytes WaitForGame gave are not given again.
    // The parts of cutscenes its players skipped go to `skip`, those that came
    // before too.
    void Feed(ChargedFeedLiveReplayFunction feed, ChargedFeedLiveSkipFunction skip = nullptr);
    // A later game of the match began; the match is over (the server ended
    // the stream); the connection broke or the server refused it.
    bool NextGame() const { return next_game_; }
    bool Over() const { return over_; }
    bool Failed() const { return failed_; }

private:
    void Run();
    // A frame of the stream (docs/reports.md: kind, u32 LE length, payload).
    void Frame(std::uint8_t kind, const std::uint8_t* data, std::size_t size);

    std::string server_;
    WatchTicket ticket_;
    Log log_;
    std::mutex mutex_;
    std::condition_variable changed_;
    // The game followed: its number (0 none yet), its bytes until Feed, how
    // many WaitForGame gave, and whether its stream is complete.
    int game_ = 0;
    std::vector<std::uint8_t> bytes_;
    std::size_t given_ = 0;
    bool complete_ = false;
    ChargedFeedLiveReplayFunction feed_ = nullptr;
    ChargedFeedLiveSkipFunction skip_ = nullptr;
    // The game's skips (part, when) before Feed.
    std::vector<std::pair<unsigned, unsigned>> skips_;
    std::string error_;
    std::atomic<bool> next_game_{false};
    std::atomic<bool> over_{false};
    std::atomic<bool> failed_{false};
    std::atomic<bool> stop_{false};
    TcpClient connection_;
    std::thread thread_;
};
} // namespace mscharged::platform
