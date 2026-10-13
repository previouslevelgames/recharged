#pragma once

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>

namespace mscharged::runtime {

// Original game frames completed per second, measured over windows of at
// least one second. Bookkeeping only; the caller shows the rate in the
// window title (FrameRateTitle) and/or the on-screen counter.
class FrameRateMeter {
public:
    using Clock = std::chrono::steady_clock;

    // Returns the rate each time a measurement window closes.
    std::optional<double> Sample(std::uint64_t frames, Clock::time_point now)
    {
        if (!start_ || frames < startFrames_) {
            start_ = now;
            startFrames_ = frames;
            return std::nullopt;
        }
        const std::chrono::duration<double> elapsed = now - *start_;
        if (elapsed < std::chrono::seconds(1)) return std::nullopt;
        const double rate = static_cast<double>(frames - startFrames_) / elapsed.count();
        start_ = now;
        startFrames_ = frames;
        return rate;
    }

private:
    std::optional<Clock::time_point> start_;
    std::uint64_t startFrames_ = 0;
};

// The window title with a measured rate: "<base> | 59.8 FPS".
inline std::string FrameRateTitle(const std::string& base, double rate)
{
    char suffix[32];
    std::snprintf(suffix, sizeof suffix, " | %.1f FPS", rate);
    return base + suffix;
}

} // namespace mscharged::runtime
