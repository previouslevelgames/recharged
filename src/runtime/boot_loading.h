#pragma once
#include "runtime/interpreter.h"
#include <optional>
#include <thread>

class GLResourcePool;
namespace mscharged
{
// Complete means this script entry returned through the original sequence rule;
// it is not a claim that the full original game/frontend globals are initialized.
enum class BootLoadingState { Idle, Running, Complete, Blocked, Failed, Cancelled };
struct BootLoadingStop
{
    unsigned service;
    std::string description;
};

// Bounded original BootLoadingToFE sequence. Only original diagnostic/stack
// primitives and the real persistent graphics pool are supplied. Unsupported
// services block explicitly; file arrival never stands in for frontend readiness.
// Initialize Aurora OS (including its clock) and graphics memory before Begin.
// Destroy/cancel before graphics/arena shutdown. The pool remains owned across
// yields and terminal failures for inspection, until Cancel/Reset/destruction.
// No global loading manager changes.
class BootLoading
{
    struct Implementation;
    std::unique_ptr<Implementation> impl_;
public:
    // Limits apply to each VM dispatch; host_calls also caps the retained trace
    // across a session. Counters below aggregate actual dispatches. The optional
    // ticker supplies original u32 ticks (default nlGetTicker), for deterministic
    // host-clock qualification; callbacks may not reenter this owner.
    explicit BootLoading(resources::Bytes bytes, InterpreterLimits limits = {},
        std::function<std::uint32_t()> ticker = {});
    ~BootLoading();
    BootLoading(const BootLoading&) = delete;
    BootLoading& operator=(const BootLoading&) = delete;
    void Begin();
    BootLoadingState Update();
    void Cancel();
    // Replays from the beginning after releasing the prior pool. Original VM
    // Reset preserves numeric globals unless restore_globals is requested.
    void Reset(bool restore_globals = false);
    BootLoadingState State() const;
    std::optional<BootLoadingStop> Stop() const;
    std::string Error() const;
    std::span<const unsigned> Calls() const;
    std::size_t Instructions() const;
    std::size_t HostCalls() const;
    const GLResourcePool* PersistentPool() const;
};
}
