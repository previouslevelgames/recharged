#pragma once
#include "Game/Debug/FrameCounter.h"
#include <array>
#include <cstdint>
namespace mscharged
{
struct FrameTimingSnapshot
{
    std::array<float,2> average_ms;
    std::array<float,2> accumulated_ms;
    std::array<float,2> last_ms;
    std::uint32_t pending_frames;
    unsigned next_history, next_continuous;
};
FrameTimingSnapshot ReadFrameTiming(const FrameCounter& counter);
}
