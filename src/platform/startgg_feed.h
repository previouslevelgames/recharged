#pragma once

#include "platform/startgg_source.h"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

namespace mscharged::platform
{
// The TOURNAMENT page's event, fetched on a thread of its own so the game
// never waits for start.gg: only while the page is open, at once when it
// opens (unless the last answer is fresh), then every `interval`; after a
// rate limit it waits `rate_wait` before asking again.
class StartggFeed
{
public:
    using Clock = std::chrono::steady_clock;
    using Log = std::function<void(const std::string&)>;

    // `log` gets a line on the feed's thread when a fetch fails (once until
    // one works again) and when it works again.
    StartggFeed(std::unique_ptr<StartggSource> source, std::string slug, Log log,
                Clock::duration interval = std::chrono::seconds(20),
                Clock::duration rate_wait = std::chrono::seconds(60));
    ~StartggFeed();
    StartggFeed(const StartggFeed&) = delete;
    StartggFeed& operator=(const StartggFeed&) = delete;

    // The page opened or closed: it asks only while open.
    void SetOpen(bool open);

    struct Snapshot
    {
        bool loaded = false; // an answer came (an event or an error)
        std::uint64_t version = 0;
        // The last event read; kept when a later fetch fails.
        std::optional<StartggEvent> event;
        std::string error; // the last fetch's, empty when it worked
        bool rate_limited = false;
        Clock::time_point fetched{};
    };
    Snapshot Latest() const;
    // Latest's version, without copying the event.
    std::uint64_t Version() const;

private:
    void Run();

    std::unique_ptr<StartggSource> source_;
    std::string slug_;
    Log log_;
    Clock::duration interval_, rateWait_;
    mutable std::mutex mutex_;
    std::condition_variable wake_;
    bool open_ = false, stop_ = false;
    Snapshot snapshot_;
    std::thread thread_;
};
} // namespace mscharged::platform
