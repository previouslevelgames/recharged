#include "runtime/nis_playback.h"
#include "Game/NisPlaybackTiming.h"
#include <algorithm>
#include <cmath>
#include <exception>
#include <limits>
#include <stdexcept>

namespace mscharged
{
namespace
{
void Finite(float value, const char* message)
{
    if (!std::isfinite(value)) throw std::invalid_argument(message);
}
void CheckFrame(NisPlaybackFrame frame, float carry)
{
    CheckCameraDelta(frame.real_delta); CheckCameraDelta(frame.time_dilation);
    const double product = double(frame.real_delta) * frame.time_dilation;
    if (product > std::numeric_limits<float>::max())
        throw std::overflow_error("NIS frame product exceeds finite original arithmetic");
    // Model each original rounded float operation without overflowing it first.
    const float clamped = std::min(static_cast<float>(product), .5f);
    if (double(clamped) + carry > std::numeric_limits<float>::max())
        throw std::overflow_error("NIS frame carry exceeds finite original arithmetic");
}
}
class NisPlayback::Operation
{
    NisPlayback& owner_;
    int exceptions_ = std::uncaught_exceptions();
public:
    explicit Operation(NisPlayback& owner) : owner_(owner) { owner_.busy_ = true; }
    ~Operation()
    {
        owner_.failed_ |= std::uncaught_exceptions() > exceptions_;
        owner_.busy_ = false;
    }
};
NisPlayback::NisPlayback(NisCameras& cameras) : cameras_(cameras) { Ready(); }
NisPlayback::~NisPlayback()
{
    if (thread_ != std::this_thread::get_id() || busy_) std::terminate();
}
void NisPlayback::CheckThread() const
{
    if (thread_ != std::this_thread::get_id()) throw std::logic_error("NIS playback requires its owner thread");
}
void NisPlayback::Ready() const
{
    CheckThread();
    if (busy_ || failed_) throw std::logic_error("NIS playback is busy or failed");
    cameras_.Camera(0); // Check the camera owner's session/lifetime/mutation guard.
}
void NisPlayback::AddTable(NisPlaybackTable table)
{
    Ready();
    if (tables_.size() >= 8 || table.triggers.size() > 48)
        throw std::length_error("NIS playback exceeds eight sources or 48 triggers per source");
    if (table.render_mode > 2) throw std::invalid_argument("NIS render mode must be 0, 1 or 2");
    for (const auto& trigger : table.triggers)
    {
        if (trigger.type >= services_.size()) throw std::invalid_argument("Unsupported NIS trigger type");
        Finite(trigger.frame, "NIS trigger frame must be finite");
        Finite(trigger.value, "NIS trigger parameter must be finite");
        if (trigger.name.find('\0') != std::string::npos || trigger.target.find('\0') != std::string::npos)
            throw std::invalid_argument("NIS trigger strings contain embedded NUL bytes");
    }
    tables_.push_back(std::move(table));
}
void NisPlayback::BindService(unsigned type, std::function<void(const NisTriggerDispatch&)> service)
{
    Ready();
    if (type >= services_.size() || !service) throw std::invalid_argument("NIS trigger requires an explicit supported service");
    services_[type] = std::move(service);
}
NisPlaybackStep NisPlayback::Advance(NisPlaybackFrame frame)
{
    Ready(); CheckFrame(frame, carry_);
    Operation operation(*this);
    NisPlaybackStep step;
    step.delta = NisPlaybackTiming::FrameDelta(frame.real_delta, frame.time_dilation, carry_);
    step.active = frame.task_state == 0x10;
    if (step.active)
    {
        std::array<float, 2> duration{};
        for (unsigned slot = 0; slot < 2; ++slot)
            if (cameras_.Camera(slot)) { step.old_time[slot] = cameras_.Time(slot); duration[slot] = cameras_.Duration(slot); }
        const auto overrun = cameras_.Advance(step.delta);
        NisPlaybackTiming::KeepPrimaryOverrun(overrun[0], carry_);
        step.primary_overrun = carry_;
        for (unsigned slot = 0; slot < 2; ++slot)
            if (cameras_.Camera(slot)) step.new_time[slot] = cameras_.Time(slot);
        for (std::size_t i = 0; i < tables_.size(); ++i)
        {
            const auto& table = tables_[i];
            const auto slot = table.render_mode == 2 ? 1 : table.render_mode;
            for (const auto& trigger : table.triggers)
                if (NisPlaybackTiming::Crossed(step.old_time[slot], step.new_time[slot], duration[slot], trigger.frame))
                {
                    auto& service = services_[trigger.type];
                    if (!service) throw std::runtime_error("Unimplemented NIS trigger service " + std::to_string(trigger.type));
                    // Trigger services can mutate their own real subsystem, but
                    // cannot replace/free cameras while this frame is dispatching.
                    NativeCameraCall guard;
                    service({i, table.render_mode, slot, trigger, step.old_time[slot], step.new_time[slot], duration[slot]});
                    ++step.dispatched;
                }
        }
    }
    last_ = step;
    return step;
}
void NisPlayback::Swap()
{
    Ready(); Operation operation(*this); cameras_.Swap();
    for (auto& table : tables_)
        if (table.render_mode < 2) table.render_mode = 1 - table.render_mode;
}
float NisPlayback::Carry() const { Ready(); return carry_; }
NisPlaybackStep NisPlayback::LastStep() const { Ready(); return last_; }
bool NisPlayback::WorldIsFrozen(std::uint32_t task_state) const
{ Ready(); return task_state == 0x10 && cameras_.TimeLeft() == 0; }
bool NisPlayback::Failed() const { CheckThread(); return failed_; }
void NisPlayback::Reset()
{
    CheckThread();
    if (busy_) throw std::logic_error("Cannot reset NIS playback during a callback");
    cameras_.Camera(0);
    carry_ = 0; last_ = {}; tables_.clear(); failed_ = false;
}
}
