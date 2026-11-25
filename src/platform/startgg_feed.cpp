#include "platform/startgg_feed.h"

#include <utility>

namespace mscharged::platform
{
StartggFeed::StartggFeed(std::unique_ptr<StartggSource> source, std::string slug, Log log,
                         Clock::duration interval, Clock::duration rate_wait)
    : source_(std::move(source)), slug_(std::move(slug)), log_(std::move(log)), interval_(interval),
      rateWait_(rate_wait)
{
    thread_ = std::thread([this] { Run(); });
}

StartggFeed::~StartggFeed()
{
    {
        std::lock_guard lock(mutex_);
        stop_ = true;
    }
    source_->Cancel();
    wake_.notify_all();
    thread_.join();
}

void StartggFeed::SetOpen(bool open)
{
    {
        std::lock_guard lock(mutex_);
        if (open_ == open) return;
        open_ = open;
    }
    wake_.notify_all();
}

StartggFeed::Snapshot StartggFeed::Latest() const
{
    std::lock_guard lock(mutex_);
    return snapshot_;
}

std::uint64_t StartggFeed::Version() const
{
    std::lock_guard lock(mutex_);
    return snapshot_.version;
}

void StartggFeed::Run()
{
    // The next fetch may go from then on (later after a rate limit).
    Clock::time_point next{};
    bool failing = false;
    std::unique_lock lock(mutex_);
    while (!stop_)
    {
        if (!open_)
        {
            wake_.wait(lock, [this] { return stop_ || open_; });
            continue;
        }
        if (Clock::now() < next)
        {
            wake_.wait_until(lock, next, [this] { return stop_ || !open_; });
            continue;
        }
        lock.unlock();
        StartggAnswer answer = source_->Fetch(slug_);
        const auto now = Clock::now();
        if (!answer.error.empty() && (!failing || answer.rate_limited) && log_) log_("start.gg: " + answer.error);
        else if (answer.error.empty() && failing && log_) log_("start.gg: answering again");
        failing = !answer.error.empty();
        lock.lock();
        snapshot_.loaded = true;
        ++snapshot_.version;
        if (answer.event) snapshot_.event = std::move(answer.event);
        snapshot_.error = std::move(answer.error);
        snapshot_.rate_limited = answer.rate_limited;
        snapshot_.fetched = now;
        next = now + (answer.rate_limited ? rateWait_ : interval_);
    }
}
} // namespace mscharged::platform
