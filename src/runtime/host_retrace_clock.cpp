#include "runtime/host_retrace_clock.h"
#include <limits>
#include <numeric>
#include <stdexcept>

namespace mscharged
{
namespace
{
constexpr std::uint64_t NanosecondsPerSecond=1000000000;
void Require(bool condition,const char* message)
{
    if(!condition) throw std::runtime_error(message);
}
}
HostRetraceClock::HostRetraceClock(HostRetraceRate rate,std::uint64_t initial)
    :rate_(rate),count_(initial)
{
    Require(rate.numerator&&rate.denominator,"Host retrace rate must be positive");
    const auto divisor=std::gcd(rate.numerator,rate.denominator);
    rate_.numerator/=divisor;rate_.denominator/=divisor;
    // Bounds guarantee all intermediate products fit ordinary uint64_t on
    // LP64 and LLP64 hosts, including one maximum-duration observation.
    Require(rate_.denominator<=1000000 && rate_.numerator<=240000000 &&
            std::uint64_t(rate_.numerator)<=240*std::uint64_t(rate_.denominator),
            "Host retrace rate exceeds the supported rational clock bounds");
}
std::uint64_t HostRetraceClock::Observe(std::uint64_t now)
{
    if(!last_){last_=now;return count_;}
    Require(now>=*last_,"Host retrace time moved backwards");
    const auto elapsed=now-*last_;
    const auto seconds=elapsed/NanosecondsPerSecond;
    const auto nanoseconds=elapsed%NanosecondsPerSecond;
    const auto numerator=std::uint64_t(rate_.numerator);
    const auto denominator=std::uint64_t(rate_.denominator);
    const auto seconds_fraction=(seconds%denominator)*numerator;
    const auto phase=(seconds_fraction%denominator)*NanosecondsPerSecond
                    +nanoseconds*numerator+remainder_;
    const auto period=denominator*NanosecondsPerSecond;
    const auto increment=(seconds/denominator)*numerator
                        +seconds_fraction/denominator+phase/period;
    Require(increment<=std::numeric_limits<std::uint64_t>::max()-count_,
            "Host retrace count overflow");
    // Commit only after all checks. A rejected observation leaves both the
    // absolute anchor and fractional phase available for a valid retry.
    count_+=increment;remainder_=phase%period;last_=now;
    return count_;
}
}
