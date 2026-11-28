#pragma once

// A job on a worker thread whose result the launcher's UI thread takes once
// it is there (polled each frame). Starting another job while one runs neither
// waits for it nor takes its result later: the replaced job ends by itself and
// only the newest one counts, so a slow result can't overwrite a newer choice
// and a new job never holds the window.

#include <chrono>
#include <future>
#include <optional>
#include <utility>
#include <vector>

namespace mscharged::launcher
{
template <class T>
class BackgroundJob
{
public:
    BackgroundJob() = default;
    BackgroundJob(const BackgroundJob&) = delete;
    BackgroundJob& operator=(const BackgroundJob&) = delete;
    // Jobs still running are waited for (as std::async's futures do).
    ~BackgroundJob() = default;

    template <class Work>
    void Start(Work&& work)
    {
        Drop();
        current_ = std::async(std::launch::async, std::forward<Work>(work));
    }

    // A job runs whose result is still to be taken.
    bool Busy() const { return current_.valid(); }

    // The newest job's result, once it is there (then the job is no longer
    // busy); none while it runs or when there is none. Rethrows what the job
    // threw.
    std::optional<T> Take()
    {
        ForgetEnded();
        if (!Ready(current_)) return std::nullopt;
        return std::optional<T>(current_.get());
    }

    // The running job's result is not wanted any more.
    void Drop()
    {
        ForgetEnded();
        if (current_.valid()) replaced_.push_back(std::move(current_));
    }

private:
    static bool Ready(const std::future<T>& job)
    {
        return job.valid() && job.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
    }

    // Replaced jobs that ended (destroying their futures waits for nothing).
    void ForgetEnded()
    {
        for (auto it = replaced_.begin(); it != replaced_.end();)
            it = Ready(*it) ? replaced_.erase(it) : std::next(it);
    }

    std::future<T> current_;
    std::vector<std::future<T>> replaced_;
};
} // namespace mscharged::launcher
