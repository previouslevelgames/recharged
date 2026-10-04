#include "runtime/boot_loading.h"
#include "Game/AsyncLoadingShared.h"
#include "NL/nlTicker.h"
#include "NL/nlMemory.h"
#include <algorithm>
#include <cmath>
#include <iterator>
#include <stdexcept>

namespace mscharged
{
namespace
{
constexpr std::uint32_t boot_hash = 0xb53474ff;
class UnqualifiedService : public std::runtime_error
{
public:
    unsigned id;
    UnqualifiedService(unsigned service, const char* description)
        : std::runtime_error(description), id(service) {}
};
const char* ServiceDescription(unsigned id)
{
    switch (id)
    {
    case 11: return "AsyncFELocalizationBegin requires original FontManager and FE resource bindings";
    case 19: return "AsyncFinalizeCameraLoading requires the original camera factory and registry";
    case 28: return "AsyncInitializeEmissionManager requires loaded particle resources and frame discard";
    case 33: return "AsyncFELocalizationFinalize requires completed original font/localization bindings";
    case 36: return "AsyncStartCameraLoading requires the original camera factory and registry";
    case 41: return "AsyncStartLoadingEmissionManager requires ParticleUpdateTask::StartLoading(false, false, false, true)";
    case 44: return "AsyncStartupFEWorldBegin requires the original FE world resource owner";
    case 45: return "AsyncStartupFEWorldFinalize requires completed original FE world registration";
    case 81: return "Boot finalization requires the real file cache and THP initialization";
    default: return "Original AsyncLoading host service is outside the selected native profile";
    }
}
}
struct BootLoading::Implementation
{
    NativeInterpreter interpreter;
    const std::thread::id thread = std::this_thread::get_id();
    BootLoadingState state = BootLoadingState::Idle;
    std::optional<BootLoadingStop> stop;
    std::string error;
    std::vector<unsigned> calls;
    GLResourcePool* pool = nullptr;
    bool busy = false, dispatched = false;
    const std::size_t trace_limit;
    std::function<std::uint32_t()> ticker;
    unsigned mSequenceState = ASYNC_LOADING_IDLE;
    std::uint32_t mStageStartTick = 0;
    InterpreterFlow flow = InterpreterFlow::Continue;
    std::size_t instructions = 0, host_calls = 0;

    Implementation(resources::Bytes bytes, InterpreterLimits limits, std::function<std::uint32_t()> clock)
        : interpreter(bytes, limits), trace_limit(limits.host_calls), ticker(clock ? std::move(clock) : nlGetTicker)
    {
        // Validate the entry before a pool or runtime side effect is possible.
        const auto script = resources::ReadScriptBytecode(bytes);
        const auto entry = std::find_if(script->functions.begin(), script->functions.end(),
            [](const auto& f) { return f.hash == boot_hash; });
        if (entry == script->functions.end() || entry->arguments != 0 || entry->flags != 0)
            throw std::invalid_argument("BootLoadingToFE must exist with no arguments or return value");
        calls.reserve(std::min<std::size_t>(limits.host_calls, 256));
        for (unsigned id = 0; id < 144; ++id)
        {
            InterpreterHostCall call{id, {}, id == 119, [this, id](auto args) { return Invoke(id, args); }};
            if (id == 120) call.arguments = {InterpreterValueKind::String};
            if (id == 49 || id == 61) call.arguments = {InterpreterValueKind::Word};
            interpreter.Bind(std::move(call));
        }
    }
    void Ready() const
    {
        if (thread != std::this_thread::get_id()) throw std::logic_error("Boot loading requires its owner thread");
        if (busy) throw std::logic_error("Boot loading cannot be reentered or mutated during execution");
    }
    void RequireGraphics() const
    {
        if (!gMemoryInitialized || !glGetResourcePools() || !glGetCurrentResourcePool())
            throw std::logic_error("Boot loading requires initialized native graphics memory");
    }
    void Release()
    {
        if (!pool) return;
        // Only this owner destroys the pool; callers receive a const observation.
        glDestroyResourcePool(pool);
        pool = nullptr;
    }
    void StopWithoutUndo() { flow = InterpreterFlow::Pause; }
    void StopWithUndo() { flow = InterpreterFlow::Retry; }
    bool IsFinished() const { return interpreter.Status() == InterpreterStatus::Ready; }
    void CallFunction(std::uint32_t hash)
    {
        if (hash != boot_hash) throw std::logic_error("Unexpected original boot sequence function");
        dispatched = true;
        if (!interpreter.Execute(hash)) throw std::logic_error("Missing original boot sequence function");
    }
    void Run()
    {
        // The native API rejects resuming a finished frame. An entry with no
        // wait may finish during BEGIN; completion is still published on RUN.
        if (interpreter.Status() == InterpreterStatus::Paused) { dispatched = true; interpreter.Resume(); }
    }
    InterpreterHostResult Invoke(unsigned id, std::span<const InterpreterValue>)
    {
        if (calls.size() >= trace_limit) throw std::length_error("Boot session host-call trace limit exceeded");
        calls.push_back(id);
        flow = InterpreterFlow::Continue;
        switch (id)
        {
        case 4:
        {
            if (pool) throw std::logic_error("Persistent boot resource pool already exists");
            RequireGraphics();
            fn_80111654(4); // Original empty debug marker, selected from FixedUpdateTask.
            const auto requirements = gPersistentResourceRequirements;
            pool = glCreateResourcePool(requirements.entries, std::size(requirements.entries), "PersistentResourcePool");
            break;
        }
        case 48: case 83: // Original explicit empty cases.
        case 49: case 61: // Original discarded numeric argument; VM performs typed pop.
            break;
        case 89: // Original AudioLoader marker has no audio operation.
            FinishAsyncLoadingStep(this, nlGetTickerDifference(mStageStartTick, ticker()), g_fYieldScriptBlockingTimeMS);
            break;
        case 119:
            return {1u, flow}; // Literal true in the original dispatcher.
        case 120:
            // Original marker ignores its int argument. Consume a checked
            // retained script string without narrowing its host address.
            fn_8011165C(0);
            break;
        default:
            throw UnqualifiedService(id, ServiceDescription(id));
        }
        return {{}, flow};
    }
};
BootLoading::BootLoading(resources::Bytes bytes, InterpreterLimits limits, std::function<std::uint32_t()> ticker)
    : impl_(std::make_unique<Implementation>(bytes, limits, std::move(ticker))) {}
BootLoading::~BootLoading()
{
    try { impl_->Ready(); impl_->Release(); } catch (...) { std::terminate(); }
}
void BootLoading::Begin()
{
    auto& s = *impl_; s.Ready();
    if (s.state != BootLoadingState::Idle) throw std::logic_error("Boot loading must be idle before Begin");
    s.RequireGraphics();
    if (!std::isfinite(g_fYieldScriptBlockingTimeMS) || g_fYieldScriptBlockingTimeMS < 0)
        throw std::logic_error("Invalid original boot yield threshold");
    s.mSequenceState = ASYNC_LOADING_BOOT_TO_FE_BEGIN;
    s.state = BootLoadingState::Running;
}
BootLoadingState BootLoading::Update()
{
    auto& s = *impl_; s.Ready();
    if (s.state != BootLoadingState::Running) return s.state;
    s.RequireGraphics();
    s.busy = true; s.dispatched = false;
    struct BusyGuard { bool& busy; ~BusyGuard() { busy = false; } } guard{s.busy};
    try
    {
        if (!std::isfinite(g_fYieldScriptBlockingTimeMS) || g_fYieldScriptBlockingTimeMS < 0)
            throw std::logic_error("Invalid original boot yield threshold");
        s.mStageStartTick = s.ticker();
        int result = ASYNC_LOADING_NO_TRANSITION;
        bool completed = false;
        AdvanceAsyncLoadingSequence(&s, result, completed);
        if (completed && result == ASYNC_LOADING_FE_READY) s.state = BootLoadingState::Complete;
    }
    catch (const UnqualifiedService& error)
    {
        s.stop = BootLoadingStop{error.id, error.what()}; s.error = error.what(); s.state = BootLoadingState::Blocked;
    }
    catch (const std::exception& error) { s.error = error.what(); s.state = BootLoadingState::Failed; }
    catch (...) { s.error = "Unknown failure during original boot loading"; s.state = BootLoadingState::Failed; }
    if (s.dispatched)
    {
        s.instructions += s.interpreter.Instructions();
        s.host_calls += s.interpreter.HostCalls();
    }
    return s.state;
}
void BootLoading::Cancel()
{
    auto& s = *impl_; s.Ready(); s.Release(); s.interpreter.Reset();
    s.mSequenceState = ASYNC_LOADING_IDLE; s.state = BootLoadingState::Cancelled;
}
void BootLoading::Reset(bool restore_globals)
{
    auto& s = *impl_; s.Ready(); s.Release(); s.interpreter.Reset(restore_globals);
    s.mSequenceState = ASYNC_LOADING_IDLE; s.state = BootLoadingState::Idle;
    s.stop.reset(); s.error.clear(); s.calls.clear(); s.instructions = s.host_calls = 0;
}
BootLoadingState BootLoading::State() const { impl_->Ready(); return impl_->state; }
std::optional<BootLoadingStop> BootLoading::Stop() const { impl_->Ready(); return impl_->stop; }
std::string BootLoading::Error() const { impl_->Ready(); return impl_->error; }
std::span<const unsigned> BootLoading::Calls() const { impl_->Ready(); return impl_->calls; }
std::size_t BootLoading::Instructions() const { impl_->Ready(); return impl_->instructions; }
std::size_t BootLoading::HostCalls() const { impl_->Ready(); return impl_->host_calls; }
const GLResourcePool* BootLoading::PersistentPool() const { impl_->Ready(); return impl_->pool; }
}
