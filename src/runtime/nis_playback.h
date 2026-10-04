#pragma once
#include "runtime/nis_cameras.h"
#include "runtime/nis_trigger_data.h"
#include <functional>

namespace mscharged
{
struct NisPlaybackFrame
{
    float real_delta = 0, time_dilation = 1;
    std::uint32_t task_state = 0;
};
struct NisPlaybackStep
{
    float delta = 0, primary_overrun = 0;
    std::array<float, 2> old_time{}, new_time{};
    std::size_t dispatched = 0;
    bool active = false;
};
struct NisTriggerDispatch
{
    std::size_t table;
    unsigned render_mode, camera_slot;
    const NisPlaybackTrigger& trigger;
    float old_time, new_time, duration;
};

// Bounded camera/trigger scheduling with shared original arithmetic. This does
// not run NisPlayer's actor/loading/overlay update or its full Play/Reset logic.
// The camera owner and its core must outlive this object. Bindings/selections
// remain caller-owned; Advance does not publish the manager pose itself.
class NisPlayback
{
    NisCameras& cameras_;
    std::thread::id thread_ = std::this_thread::get_id();
    std::vector<NisPlaybackTable> tables_;
    std::array<std::function<void(const NisTriggerDispatch&)>, 11> services_;
    float carry_ = 0;
    NisPlaybackStep last_;
    bool busy_ = false, failed_ = false;
    class Operation;
    void CheckThread() const;
    void Ready() const;
public:
    explicit NisPlayback(NisCameras& cameras);
    ~NisPlayback();
    NisPlayback(const NisPlayback&) = delete;
    NisPlayback& operator=(const NisPlayback&) = delete;
    void AddTable(NisPlaybackTable table); // Original playing/trigger order, up to 8 x 48.
    // Services are mandatory when their trigger is reached. This layer never
    // substitutes a successful effect/audio/event/actor implementation.
    void BindService(unsigned type, std::function<void(const NisTriggerDispatch&)> service);
    NisPlaybackStep Advance(NisPlaybackFrame frame);
    void Swap(); // Swaps cameras plus every table's 0/1 mode; mode 2 is unchanged.
    float Carry() const;
    NisPlaybackStep LastStep() const;
    bool WorldIsFrozen(std::uint32_t task_state) const;
    bool Failed() const;
    // Clears carry, tables and recorded dispatch state; keeps bound services.
    // It neither releases nor silently seeks caller-owned camera bindings.
    // A poisoned camera core still requires its own teardown/new session.
    void Reset();
};
}
