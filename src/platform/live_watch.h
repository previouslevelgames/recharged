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
    void Feed(ChargedFeedLiveReplayFunction feed);
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
    std::string error_;
    std::atomic<bool> next_game_{false};
    std::atomic<bool> over_{false};
    std::atomic<bool> failed_{false};
    std::atomic<bool> stop_{false};
    TcpClient connection_;
    std::thread thread_;
};
} // namespace mscharged::platform
