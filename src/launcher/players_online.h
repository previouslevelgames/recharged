#pragma once

#include "platform/http_client.h"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>

namespace mscharged::launcher
{
// How many players are online on the WFC server, for the Play page: its
// GET /api/v1/online (Recharged WFC 1.2.19 and later), asked at the start and
// then every minute on a thread of its own. Only an answer of the last ask
// counts: after a failure nothing is shown until the server answers again. A
// server without the count (404, 405: an older or another WFC server) is not
// asked again while the launcher runs.
class PlayersOnline
{
public:
    struct Count
    {
        std::int64_t online = 0;  // logged in to WFC on the server, from any game (its "wfc")
        std::int64_t playing = 0; // of them, in an online match
    };
    using Log = std::function<void(const std::string& line)>;
    // One HTTP request (HttpGet; a test answers for the server).
    using Request = std::function<platform::HttpResponse(const platform::HttpRequest& request)>;

    // Asks `server` (online.server: a host name or address) at once and then
    // every `every`, through `request` (HttpGet when empty).
    PlayersOnline(std::string server, Log log, std::chrono::milliseconds every = std::chrono::minutes(1),
                  Request request = {});
    // Stops the ask under way.
    ~PlayersOnline();
    PlayersOnline(const PlayersOnline&) = delete;
    PlayersOnline& operator=(const PlayersOnline&) = delete;

    // The count of the last answer; nothing before the first one, after a
    // failed ask and from a server without the count.
    std::optional<Count> Snapshot() const;

private:
    void Run();
    void Ask();

    const std::string server_;
    const Log log_;
    const std::chrono::milliseconds every_;
    const Request request_;
    const std::shared_ptr<platform::HttpCancel> cancel_ = std::make_shared<platform::HttpCancel>();
    mutable std::mutex mutex_;
    std::condition_variable wake_;
    bool stop_ = false;
    bool unsupported_ = false; // 404 or 405: not asked again
    bool failing_ = false;     // the last ask failed (logged once until an answer)
    bool answered_ = false;    // an answer came (logged once)
    std::optional<Count> count_;
    std::thread thread_;
};

// The address PlayersOnline asks: plain http like the game's own WFC requests.
std::string PlayersOnlineUrl(const std::string& server);
// The server's answer, {"wfc": N, "playing": M, ...}; nothing for anything
// else (another shape, negative or missing numbers, more playing than online).
std::optional<PlayersOnline::Count> ParsePlayersOnline(std::string_view body);
} // namespace mscharged::launcher
