#pragma once

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <utility>

namespace mscharged::runtime {

// Window-title frame rate: original game frames completed per second,
// measured over windows of at least one second. Bookkeeping only; the
// caller sets the title on the window's thread.
class FrameRateTitle {
public:
    using Clock = std::chrono::steady_clock;

    explicit FrameRateTitle(std::string base) : base_(std::move(base)) {}

    // Returns the new title each time a measurement window closes.
    std::optional<std::string> Sample(std::uint64_t frames, Clock::time_point now)
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
        char suffix[32];
        std::snprintf(suffix, sizeof suffix, " | %.1f FPS", rate);
        return base_ + suffix;
    }

private:
    std::string base_;
    std::optional<Clock::time_point> start_;
    std::uint64_t startFrames_ = 0;
};

} // namespace mscharged::runtime
