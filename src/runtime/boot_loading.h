#pragma once
#include "runtime/interpreter.h"
#include <optional>
#include <thread>

class GLResourcePool;
namespace mscharged
{
class BootEffectsResources;
// An opaque binding minted only by the concrete checked effects owner. Keeping
// it alive retains that provider. Boot releases it before its persistent pool.
class BootEffectsBinding final
{
    friend class BootEffectsResources;
    friend class BootLoading;
    const void* owner_=nullptr;
    std::function<void(GLResourcePool&)> start_;
    std::function<bool()> finish_;
    std::function<void()> release_;
    BootEffectsBinding(std::function<void(GLResourcePool&)> start,
        std::function<bool()> finish,std::function<void()> release)
        :start_(std::move(start)),finish_(std::move(finish)),release_(std::move(release)){}
public:
    using Handle=std::shared_ptr<BootEffectsBinding>;
    BootEffectsBinding(const BootEffectsBinding&)=delete;
    BootEffectsBinding& operator=(const BootEffectsBinding&)=delete;
};
class BootNpcResources;
// Minted only by the concrete retained template loader. One boot owner admits
// this provider; arbitrary success callbacks cannot supply NPC readiness.
class BootNpcBinding final
{
    friend class BootNpcResources;
    friend class BootLoading;
    const void* owner_=nullptr;
    std::function<void(GLResourcePool&,std::string_view,bool)> create_;
    std::function<bool()> select_;
    std::function<void()> begin_;
    std::function<bool()> finish_;
    std::function<void()> release_;
    BootNpcBinding(decltype(create_) create,decltype(select_) select,decltype(begin_) begin,
        decltype(finish_) finish,decltype(release_) release)
        :create_(std::move(create)),select_(std::move(select)),begin_(std::move(begin)),
         finish_(std::move(finish)),release_(std::move(release)){}
public:
    using Handle=std::shared_ptr<BootNpcBinding>;
    BootNpcBinding(const BootNpcBinding&)=delete;
    BootNpcBinding& operator=(const BootNpcBinding&)=delete;
};
// Both zero retain original console requirements. Explicit native allocations
// remain bounded and never auto-grow after failure; the source constants stay
// intact. MEM1 owns native headers/indices, MEM2 owns vertices/tiled textures.
struct BootLoadingMemory { std::uint32_t headers=0, resources=0; };
// Complete means this script entry returned through the original sequence rule;
// it is not a claim that the full original game/frontend globals are initialized.
enum class BootLoadingState { Idle, Running, Complete, Blocked, Failed, Cancelled };
struct BootLoadingStop
{
    unsigned service;
    std::string description;
};

// Bounded original BootLoadingToFE sequence. Only original diagnostic/stack
// primitives and the real persistent graphics pool are supplied by default.
// An explicit checked effects binding adds original services41/28; a retained
// NPC binding adds template resource services1/108/138/76. Unsupported
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
    BootLoading(resources::Bytes bytes, InterpreterLimits limits,
        std::function<std::uint32_t()> ticker, BootEffectsBinding::Handle effects, BootLoadingMemory memory = {});
    BootLoading(resources::Bytes bytes, InterpreterLimits limits,
        std::function<std::uint32_t()> ticker, BootEffectsBinding::Handle effects,
        BootLoadingMemory memory, BootNpcBinding::Handle npcs);
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
