#include "platform/video_device.h"
#include "platform/interrupt_controller.h"
#include "platform/interrupts.h"

#include <aurora/video.h>
#include <dolphin/os.h>

#include <mutex>
#include <stdexcept>
#include <thread>

namespace {
using namespace mscharged::platform;
struct VideoDevice {
    std::mutex mutex;
    std::thread::id owner;
    bool configured{}, initialized{};
    NativeInterruptSource source{};
    void (*retrace)(void*){};
    void* retrace_context{};
};
VideoDevice& State() { static VideoDevice state; return state; }

void RequireOwner(const VideoDevice& state) {
    if (!state.configured || state.owner != std::this_thread::get_id())
        throw std::logic_error("native VI interrupt device requires its configured owner");
}

void HandleVI(__OSInterrupt interrupt, OSContext* context) {
    auto& state = State();
    void (*retrace)(void*);
    void* retrace_context;
    {
        std::lock_guard lock(state.mutex);
        RequireOwner(state);
        if (!state.initialized || interrupt != __OS_INTERRUPT_PI_VI || !context)
            throw std::logic_error("native VI received an invalid shared-controller delivery");
        retrace = state.retrace; retrace_context = state.retrace_context;
    }
    // The SDK driver acknowledges its genuine cause before source callbacks.
    // The shared controller already supplies source mask/context exclusion.
    retrace(retrace_context);
}

void Initialize(void* pointer, void (*retrace)(void*), void* retrace_context) {
    auto& state = *static_cast<VideoDevice*>(pointer);
    NativeInterruptGuard exclusion;
    {
        std::lock_guard lock(state.mutex);
        RequireOwner(state);
        if (state.initialized || !retrace)
            throw std::logic_error("native VI interrupt endpoint is already initialized or incomplete");
    }
    const auto source = GetNativeInterruptSource(__OS_INTERRUPT_PI_VI);
    if (__OSGetInterruptHandler(__OS_INTERRUPT_PI_VI))
        throw std::logic_error("native VI would overwrite another live interrupt endpoint");
    if (!SetNativeInterruptPending(source, false))
        throw std::logic_error("native VI controller generation retired during initialization");
    __OSSetInterruptHandler(__OS_INTERRUPT_PI_VI, HandleVI);
    __OSUnmaskInterrupts(OS_INTERRUPTMASK_PI_VI);
    std::lock_guard lock(state.mutex);
    state.source = source; state.retrace = retrace; state.retrace_context = retrace_context;
    state.initialized = true;
}

void Pending(void* pointer, bool pending) {
    auto& state = *static_cast<VideoDevice*>(pointer);
    NativeInterruptSource source;
    {
        std::lock_guard lock(state.mutex);
        if (!state.initialized)
            throw std::logic_error("native VI cause has no live controller endpoint");
        source = state.source;
    }
    // Device levels can latch under source CPU/current/user masks. This path
    // never acquires the source exclusion or executes an interrupt handler.
    if (!SetNativeInterruptPending(source, pending))
        throw std::logic_error("native VI live controller generation was retired");
}

bool Service(void* pointer) {
    auto& state = *static_cast<VideoDevice*>(pointer);
    {
        std::lock_guard lock(state.mutex); RequireOwner(state);
        if (!state.initialized) return false;
    }
    // Foreign critical sections must not make an ordinary owner-clock query
    // block. Once acquired, the controller's recursive exclusion is safe.
    NativeInterruptRead exclusion;
    if (!exclusion) return false;
    return ServiceNativeInterruptController();
}

void Shutdown(void* pointer) {
    auto& state = *static_cast<VideoDevice*>(pointer);
    NativeInterruptGuard exclusion;
    NativeInterruptSource source;
    bool initialized;
    {
        std::lock_guard lock(state.mutex); RequireOwner(state);
        source = state.source; initialized = state.initialized;
    }
    if (initialized) {
        if (__OSGetInterruptHandler(__OS_INTERRUPT_PI_VI) != HandleVI)
            throw std::logic_error("native VI interrupt handler ownership changed before retirement");
        __OSMaskInterrupts(OS_INTERRUPTMASK_PI_VI);
        if (!SetNativeInterruptPending(source, false))
            throw std::logic_error("native VI controller retired before device shutdown");
        __OSSetInterruptHandler(__OS_INTERRUPT_PI_VI, nullptr);
    }
    std::lock_guard lock(state.mutex);
    state.initialized = state.configured = false; state.owner = {};
    state.source = {}; state.retrace = nullptr; state.retrace_context = nullptr;
}
} // namespace

namespace mscharged::platform {
void ConfigureNativeVideoHardware(std::uint32_t boot_tv_mode, bool dtv_cable) {
    NativeInterruptGuard exclusion;
    const auto controller = GetNativeInterruptControllerStatus();
    if (!controller.initialized || !controller.owner_thread)
        throw std::logic_error("native VI requires the actual initialized owner interrupt controller");
    auto& state = State();
    {
        std::lock_guard lock(state.mutex);
        if (state.configured) throw std::logic_error("native VI endpoint is already configured");
    }
    const AuroraVIBootConfig boot{boot_tv_mode, dtv_cable, NativeInterruptsEnabled,
                                 DispatchNativeInterrupt};
    aurora_configure_video_hardware(&boot);
    {
        std::lock_guard lock(state.mutex);
        state.configured = true; state.owner = std::this_thread::get_id();
    }
    const AuroraVIInterruptDevice device{&state, Initialize, Pending, Service, Shutdown};
    try { aurora_attach_video_interrupt_device(&device); }
    catch (...) {
        // This call successfully staged the SDK configuration, so it owns
        // retirement; an unrelated existing endpoint is never cleared here.
        aurora_shutdown_video_hardware();
        std::lock_guard lock(state.mutex); state.configured = false; state.owner = {};
        throw;
    }
}
} // namespace mscharged::platform
