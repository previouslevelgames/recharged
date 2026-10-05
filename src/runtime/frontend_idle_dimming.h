#pragma once
#include <cstdint>
#include <thread>

namespace mscharged
{
// Native display policy for the Title VI timing requests. Actual rendering
// consumes Opacity(); genuine focused host activity wakes the image. This
// does not emulate the Wii's video hardware or claim its dimming curve.
class FrontendIdleDimming
{
    std::thread::id thread_ = std::this_thread::get_id();
    std::uint64_t current_, activity_;
    unsigned threshold_retraces_ = 18000;
public:
    explicit FrontendIdleDimming(std::uint64_t actual_retrace);
    void Sample(std::uint64_t actual_retrace, bool actual_host_activity);
    // Source NTSC thresholds: VI_DM_DEFAULT(0)=18000 retraces;
    // VI_DM_15M(2)=54000. Use the same raw HostRetraceClock as movies.
    // Applying a policy preserves elapsed inactivity; input owns wake/reset.
    void Select(unsigned original_mode);
    unsigned ThresholdRetraces() const;
    float Opacity() const; // Native50% black overlay after the selected timeout.
};
}
