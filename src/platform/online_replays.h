#pragma once

#include "platform/match_replay_status.h"
#include "platform/online_reports.h"
#include "platform/tcp_client.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

// The replays of online games on their way to the WFC server, from a thread of
// their own and at the lowest priority this computer offers (tcp_client.h).
// While a game the server takes streams is played (its answer to the
// series' start, OnlineReplayAnswers), its steps go to the server as each
// becomes final (docs/reports.md of the server, "Live streams"): the game
// module puts them in its ring (live_ring.h), this thread takes them every
// 100 ms and sends what the connection takes, a few KB at most each time. It
// sends nothing while the match plays far ahead of the inputs that have
// arrived (the connection is in trouble), and gives the live stream up rather
// than let data pile up: the match never waits for it, and its replay still
// comes after the game. After a game, outside matches, a replay the server
// asked for (its answer to the game's report) goes as its file.
namespace mscharged::platform
{
class OnlineReplays
{
public:
    using Log = std::function<void(const std::string& line)>;

    // Sends to `server` (online.server) what `answers` (the reports',
    // OnlineReports::ReplayAnswers) say the server takes; takes the live
    // stream through `take` (charged_take_live; none: replays after the game
    // only).
    OnlineReplays(std::string server, std::shared_ptr<OnlineReplayAnswers> answers, ChargedTakeLiveFunction take,
                  Log log);
    // Stops at once: a live stream or a replay under way is dropped.
    ~OnlineReplays();
    OnlineReplays(const OnlineReplays&) = delete;
    OnlineReplays& operator=(const OnlineReplays&) = delete;

    // A replay of an online game kept in `file` (the game's thread): sent
    // once the server asked for it.
    void Kept(std::uint64_t series, int game, std::filesystem::path file);

    // The traffic's limits, for tests.
    struct Limits
    {
        std::chrono::milliseconds tick{100};
        // How often it looks while no game streams and no replay waits.
        std::chrono::milliseconds idle_tick{500};
        // Bytes written to the connection each tick, at most: the live
        // stream's, and a replay's after the game.
        std::size_t live_per_tick = 4u << 10;
        std::size_t replay_per_tick = 16u << 10;
        // The match runs this many steps ahead of its inputs: send nothing.
        int trouble_steps = 8;
        // No progress for this long with data waiting: the stream is given up.
        std::chrono::seconds stall{10};
        std::chrono::seconds retry{5};
        int attempts = 5;
    };
    OnlineReplays(std::string server, std::shared_ptr<OnlineReplayAnswers> answers, ChargedTakeLiveFunction take,
                  Log log, Limits limits);

private:
    struct Live;
    struct Upload;
    void Run();
    // One look; whether there is work under way (a game, a replay).
    bool Tick();
    void Drain();
    void SendLive(int unconfirmed);
    void SendSkips();
    void SendReplays();

    std::string server_;
    std::shared_ptr<OnlineReplayAnswers> answers_;
    ChargedTakeLiveFunction take_;
    Log log_;
    Limits limits_;

    std::mutex mutex_;
    std::condition_variable wake_;
    bool stop_ = false;
    struct KeptFile
    {
        std::uint64_t series = 0;
        int game = 0;
        std::filesystem::path file;
    };
    std::vector<KeptFile> kept_; // the game's thread hands them over here

    // The thread's own.
    std::unique_ptr<Live> live_;
    std::vector<std::unique_ptr<Upload>> uploads_;
    std::vector<std::uint8_t> taken_;
    bool replays_off_ = false; // the server takes none (404)
    bool skips_off_ = false;   // the server passes no cutscene skips on (404, 405)
    std::thread thread_;
};
} // namespace mscharged::platform
