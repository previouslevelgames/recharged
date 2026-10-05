#pragma once
#include <cstdint>
#include <optional>

namespace mscharged
{
struct HostRetraceRate
{
    std::uint32_t numerator;
    std::uint32_t denominator;
    // Standard VI field/retrace cadence, independent of monitor refresh and
    // THP header FPS. NTSC interlaced and progressive use the same cadence.
    static constexpr HostRetraceRate Ntsc() { return {60000,1001}; }
    static constexpr HostRetraceRate Pal() { return {50,1}; }
};

// Counts elapsed VI periods from explicit raw monotonic host nanoseconds.
// The first observation anchors the clock. Input locks, focus and rendering
// do not stop it; callers keep sampling the same authority throughout its
// lifetime. A delayed sample includes every elapsed period without dispatching
// invented VI callbacks. This is not a display-presentation counter or a full
// implementation of the console VI service.
class HostRetraceClock
{
    HostRetraceRate rate_;
    std::optional<std::uint64_t> last_;
    std::uint64_t count_,remainder_=0;
public:
    explicit HostRetraceClock(HostRetraceRate,std::uint64_t initial_retrace=0);
    std::uint64_t Observe(std::uint64_t monotonic_nanoseconds);
    std::uint64_t Count() const { return count_; }
    HostRetraceRate Rate() const { return rate_; }
};
}
