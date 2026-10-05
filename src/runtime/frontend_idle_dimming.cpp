#include "runtime/frontend_idle_dimming.h"
#include <stdexcept>

namespace mscharged
{
FrontendIdleDimming::FrontendIdleDimming(std::uint64_t now) : current_(now), activity_(now) {}
void FrontendIdleDimming::Sample(std::uint64_t now, bool activity)
{
    if(thread_!=std::this_thread::get_id())throw std::logic_error("Idle dimming requires its owner thread");
    if(now<current_)throw std::logic_error("Idle dimming clock moved backwards");
    current_=now;
    if(activity)activity_=now;
}
void FrontendIdleDimming::Select(unsigned mode)
{
    if(thread_!=std::this_thread::get_id())throw std::logic_error("Idle dimming requires its owner thread");
    if(mode!=0&&mode!=2)throw std::invalid_argument("Unsupported Title dimming policy");
    threshold_retraces_=mode==2?54000:18000;
}
unsigned FrontendIdleDimming::ThresholdRetraces() const
{
    if(thread_!=std::this_thread::get_id())throw std::logic_error("Idle dimming requires its owner thread");
    return threshold_retraces_;
}
float FrontendIdleDimming::Opacity() const
{
    const auto timeout=ThresholdRetraces();
    return current_-activity_>=timeout?.5f:0.f;
}
}
