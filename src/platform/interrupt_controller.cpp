#include "platform/interrupt_controller.h"
#include "platform/interrupts.h"

#include <dolphin/os.h>
#include <dolphin/os/OSContext.h>
#include <dolphin/os/OSTime.h>

#include <array>
#include <cstring>
#include <mutex>
#include <stdexcept>
#include <thread>

// These describe genuine native delivery, not a fabricated PPC register state.
extern "C" {
volatile __OSInterrupt __OSLastInterrupt{};
volatile u32 __OSLastInterruptSrr0{};
volatile OSTime __OSLastInterruptTime{};
}

namespace {
using mscharged::platform::NativeInterruptGuard;
using mscharged::platform::NativeDSPMaskObserver;

struct Controller {
    std::mutex mutex;
    std::array<__OSInterruptHandler, __OS_INTERRUPT_MAX> handlers{};
    std::thread::id owner{};
    bool initialized{};
    u32 user_mask{};
    u32 current_mask{};
    u32 pending{};
    unsigned depth{};
    std::uint64_t generation{};
    std::uint64_t dispatched{};
    NativeDSPMaskObserver dsp_mask_observer{};
    void* dsp_mask_context{};
};

Controller& State() { static Controller controller; return controller; }

u32 Bit(__OSInterrupt interrupt) {
    if (interrupt < 0 || interrupt >= __OS_INTERRUPT_MAX)
        throw std::out_of_range("native interrupt index outside SDK table");
    return u32(0x80000000u) >> interrupt;
}

void RequireInitialized(const Controller& state) {
    if (!state.initialized)
        throw std::logic_error("native interrupt controller is not initialized");
}

void RequireOwner(const Controller& state) {
    RequireInitialized(state);
    if (state.owner != std::this_thread::get_id())
        throw std::logic_error("native interrupt SDK operation requires the game owner thread");
}

// Exact priority groups from the original Wii OSInterrupt source. Within one
// group the original __cntlzw selects the smallest interrupt index.
constexpr u32 PriorityGroups[]{
    0x00000100, 0x00000040, 0xF8000000, 0x00000200, 0x00000080, 0x00000010,
    0x00003000, 0x00000020, 0x03FF8C00, 0x04000000, 0x00004000, 0xFFFFFFFF};

__OSInterrupt Select(u32 pending) {
    for (u32 group : PriorityGroups) {
        const u32 eligible = pending & group;
        if (!eligible) continue;
        for (__OSInterrupt interrupt = 0; interrupt < __OS_INTERRUPT_MAX; ++interrupt)
            if (eligible & (u32(0x80000000u) >> interrupt)) return interrupt;
    }
    throw std::logic_error("native interrupt priority selected an empty mask");
}

struct EndDispatch {
    Controller& state;
    ~EndDispatch() {
        std::lock_guard lock(state.mutex);
        --state.depth;
    }
};
} // namespace

extern "C" __OSInterruptHandler __OSSetInterruptHandler(
    __OSInterrupt interrupt, __OSInterruptHandler handler) {
    Bit(interrupt);
    NativeInterruptGuard exclusion;
    auto& state = State();
    std::lock_guard lock(state.mutex);
    RequireOwner(state);
    const auto previous = state.handlers[interrupt];
    state.handlers[interrupt] = handler;
    return previous;
}

extern "C" __OSInterruptHandler __OSGetInterruptHandler(__OSInterrupt interrupt) {
    Bit(interrupt);
    NativeInterruptGuard exclusion;
    auto& state = State();
    std::lock_guard lock(state.mutex);
    RequireInitialized(state);
    return state.handlers[interrupt];
}

extern "C" OSInterruptMask __OSMaskInterrupts(OSInterruptMask mask) {
    NativeInterruptGuard exclusion;
    auto& state = State();
    OSInterruptMask previous, effective;
    NativeDSPMaskObserver observer{};
    void* context{};
    {
        std::lock_guard lock(state.mutex);
        RequireOwner(state);
        previous = state.user_mask;
        // Same work selection as original __OSMaskInterrupts. SetInterruptMask
        // rewrites all three DSP mask fields when any DSP group bit is selected.
        const auto work = mask & ~(previous | state.current_mask);
        state.user_mask |= mask;
        effective = state.user_mask | state.current_mask;
        constexpr u32 dsp_group = 0x07000000;
        if (work & dsp_group) {observer=state.dsp_mask_observer;context=state.dsp_mask_context;}
    }
    if (observer) observer(effective,context);
    return previous;
}

extern "C" OSInterruptMask __OSUnmaskInterrupts(OSInterruptMask mask) {
    NativeInterruptGuard exclusion;
    auto& state = State();
    OSInterruptMask previous, effective;
    NativeDSPMaskObserver observer{};
    void* context{};
    {
        std::lock_guard lock(state.mutex);
        RequireOwner(state);
        previous = state.user_mask;
        const auto work = mask & (previous | state.current_mask);
        state.user_mask &= ~mask;
        effective = state.user_mask | state.current_mask;
        constexpr u32 dsp_group = 0x07000000;
        if (work & dsp_group) {observer=state.dsp_mask_observer;context=state.dsp_mask_context;}
    }
    if (observer) observer(effective,context);
    return previous;
}

extern "C" OSInterruptMask OSGetInterruptMask() {
    NativeInterruptGuard exclusion;
    auto& state = State();
    std::lock_guard lock(state.mutex);
    RequireInitialized(state);
    return state.current_mask;
}

extern "C" OSInterruptMask OSSetInterruptMask(OSInterruptMask mask) {
    NativeInterruptGuard exclusion;
    auto& state = State();
    OSInterruptMask previous, effective;
    NativeDSPMaskObserver observer{};
    void* context{};
    {
        std::lock_guard lock(state.mutex);
        RequireOwner(state);
        previous = state.current_mask;
        state.current_mask = mask;
        effective = state.user_mask | state.current_mask;
        constexpr u32 dsp_group = 0x07000000;
        if ((previous ^ mask) & dsp_group) {observer=state.dsp_mask_observer;context=state.dsp_mask_context;}
    }
    if (observer) observer(effective,context);
    return previous;
}

namespace mscharged::platform {
void AttachNativeDSPMaskObserver(NativeDSPMaskObserver observer,void* context) {
    if (!observer) throw std::invalid_argument("native DSP mask observer is null");
    NativeInterruptGuard exclusion;auto& state=State();u32 effective;
    {
        std::lock_guard lock(state.mutex);RequireOwner(state);
        if (state.dsp_mask_observer) throw std::logic_error("native DSP hardware mask sink already attached");
        state.dsp_mask_observer=observer;state.dsp_mask_context=context;
        effective=state.user_mask|state.current_mask;
    }
    try {observer(effective,context);}
    catch (...) {
        std::lock_guard lock(state.mutex);state.dsp_mask_observer=nullptr;state.dsp_mask_context=nullptr;throw;
    }
}
void DetachNativeDSPMaskObserver(NativeDSPMaskObserver observer,void* context) {
    NativeInterruptGuard exclusion;auto& state=State();std::lock_guard lock(state.mutex);RequireOwner(state);
    if (state.dsp_mask_observer!=observer || state.dsp_mask_context!=context)
        throw std::logic_error("native DSP hardware mask sink ownership differs");
    state.dsp_mask_observer=nullptr;state.dsp_mask_context=nullptr;
}
void InitializeNativeInterruptController() {
    NativeInterruptGuard exclusion;
    auto& state = State();
    std::lock_guard lock(state.mutex);
    if (state.initialized) {
        RequireOwner(state);
        return;
    }
    state.handlers.fill(nullptr);
    state.owner = std::this_thread::get_id();
    // Original Wii __OSInterruptInit masks exactly indices0..27; reserved
    // indices28..31 retain the original zero user-mask bits.
    state.user_mask = 0xfffffff0u;
    state.current_mask = 0;
    state.pending = 0;
    state.depth = 0;
    state.dsp_mask_observer=nullptr;state.dsp_mask_context=nullptr;
    ++state.generation;
    state.dispatched = 0;
    __OSLastInterrupt = 0;
    __OSLastInterruptTime = 0;
    __OSLastInterruptSrr0 = 0;
    state.initialized = true;
}

void ShutdownNativeInterruptController() {
    NativeInterruptGuard exclusion;
    auto& state = State();
    std::lock_guard lock(state.mutex);
    if (!state.initialized) return;
    RequireOwner(state);
    if (state.depth) throw std::logic_error("cannot shut down an active native interrupt handler");
    if (state.dsp_mask_observer) throw std::logic_error("native DSP hardware must detach before controller shutdown");
    state.initialized = false;
    state.handlers.fill(nullptr);
    state.pending = 0;
    state.user_mask = state.current_mask = 0;
    state.owner = {};
}

NativeInterruptSource GetNativeInterruptSource(__OSInterrupt interrupt) {
    Bit(interrupt);
    NativeInterruptGuard exclusion;
    auto& state = State();
    std::lock_guard lock(state.mutex);
    RequireOwner(state);
    return {interrupt, state.generation};
}

bool SetNativeInterruptPending(NativeInterruptSource source, bool pending) {
    const auto bit = Bit(source.interrupt);
    auto& state = State();
    // Device workers do not acquire the source exclusion or wait for a source
    // callback. This mutex never remains held during source handler execution.
    std::lock_guard lock(state.mutex);
    if (!state.initialized || source.generation != state.generation) return false;
    if (pending) state.pending |= bit;
    else state.pending &= ~bit;
    return true;
}

bool ServiceNativeInterruptController() {
    NativeInterruptGuard exclusion;
    auto& state = State();
    __OSInterrupt interrupt;
    __OSInterruptHandler handler;
    {
        std::lock_guard lock(state.mutex);
        if (!state.initialized) return false;
        if (state.owner != std::this_thread::get_id())
            throw std::logic_error("native interrupt delivery requires the game owner thread");
        if (!NativeInterruptsEnabled()) return false;
        const auto eligible = state.pending & ~(state.user_mask | state.current_mask);
        if (!eligible) return false;
        interrupt = Select(eligible);
        handler = state.handlers[interrupt];
        // Original OS dispatch does not select a lower priority line when the
        // chosen line has no handler. The real device must acknowledge its level.
        if (!handler) return false;
        ++state.depth;
    }
    EndDispatch finish{state};
    OSContext* interrupted = OSGetCurrentContext();
    if (!interrupted) throw std::logic_error("native interrupt delivery requires a current SDK context");
    struct Call {
        Controller& state;
        __OSInterrupt interrupt;
        __OSInterruptHandler handler;
        OSContext* interrupted;
        bool invoked{};
        static void Run(void* pointer) {
            auto& call = *static_cast<Call*>(pointer);
            if (call.interrupt > __OS_INTERRUPT_MEM_ADDRESS) {
                u32 address;
                std::memcpy(&address, reinterpret_cast<const u8*>(call.interrupted) + OS_CONTEXT_SRR0,
                            sizeof(address));
                __OSLastInterrupt = call.interrupt;
                __OSLastInterruptTime = OSGetTime();
                __OSLastInterruptSrr0 = address;
            }
            {
                std::lock_guard lock(call.state.mutex);
                ++call.state.dispatched;
            }
            call.invoked = true;
            call.handler(call.interrupt, call.interrupted);
        }
    } call{state, interrupt, handler, interrupted};
    DispatchNativeInterrupt(Call::Run, &call);
    // Level-pending remains asserted until the actual device acknowledges it;
    // a service call alone does not invent or consume DSP mail/completion.
    return call.invoked;
}

NativeInterruptControllerStatus GetNativeInterruptControllerStatus() {
    NativeInterruptGuard exclusion;
    auto& state = State();
    std::lock_guard lock(state.mutex);
    return {state.initialized, state.owner == std::this_thread::get_id(), state.user_mask,
            state.current_mask, state.pending, state.depth, state.generation, state.dispatched};
}
} // namespace mscharged::platform
