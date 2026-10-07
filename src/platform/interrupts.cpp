#include "platform/interrupts.h"

#include <dolphin/os.h>
#include <dolphin/os/OSContext.h>

#include <cstring>
#include <mutex>

namespace {
std::recursive_mutex& Exclusion() {
    // Original game static constructors may call SDK critical sections before
    // main. The host lock therefore exists at its first actual use.
    static std::recursive_mutex exclusion;
    return exclusion;
}
thread_local bool enabled = true;
thread_local bool owns_mask_lock = false;
thread_local unsigned dispatch_depth = 0;
thread_local unsigned guard_depth = 0;
thread_local OSContext native_thread_context{};
thread_local OSContext* current_context = &native_thread_context;
}

extern "C" BOOL OSDisableInterrupts() {
    const bool previous = enabled;
    if (previous) {
        Exclusion().lock();
        owns_mask_lock = true;
        enabled = false;
    }
    return previous;
}

extern "C" BOOL OSEnableInterrupts() {
    const bool previous = enabled;
    enabled = true;
    if (owns_mask_lock) {
        owns_mask_lock = false;
        Exclusion().unlock();
    }
    return previous;
}

extern "C" BOOL OSRestoreInterrupts(BOOL level) {
    return level ? OSEnableInterrupts() : OSDisableInterrupts();
}

extern "C" void OSClearContext(OSContext* context) {
    // Original SDK clears only mode/state. Other opaque context bytes are
    // retained. Host thread/FPU machinery owns native execution registers.
    constexpr std::size_t mode_offset = OS_CONTEXT_MODE;
    const u32 zero = 0;
    static_assert(mode_offset + sizeof(zero) <= sizeof(OSContext));
    std::memcpy(reinterpret_cast<u8*>(context) + mode_offset, &zero, sizeof(zero));
}

extern "C" OSContext* OSGetCurrentContext() { return current_context; }
extern "C" void OSSetCurrentContext(OSContext* context) { current_context = context; }

namespace mscharged::platform {
NativeInterruptGuard::NativeInterruptGuard() { Exclusion().lock(); ++guard_depth; }
NativeInterruptGuard::~NativeInterruptGuard() { --guard_depth; Exclusion().unlock(); }
NativeInterruptRead::NativeInterruptRead() : acquired_(Exclusion().try_lock()) { if (acquired_) ++guard_depth; }
NativeInterruptRead::~NativeInterruptRead() { if (acquired_) { --guard_depth; Exclusion().unlock(); } }
bool NativeInterruptsEnabled() noexcept { return enabled; }
bool NativeInterruptDispatchActive() noexcept { return dispatch_depth != 0; }
bool NativeInterruptWaitAllowed() noexcept { return !dispatch_depth && !guard_depth; }

bool DispatchNativeInterrupt(void (*callback)(void*), void* context) {
    if (!callback || !enabled) return false;
    NativeInterruptRead scope;
    if (!scope) return false;
    const bool prior_enabled = enabled;
    const bool prior_mask_lock = owns_mask_lock;
    OSContext temporary{};
    OSContext* prior_context = OSGetCurrentContext();
    OSClearContext(&temporary);
    OSSetCurrentContext(&temporary);
    enabled = false;
    ++dispatch_depth;
    auto restore = [&] {
        --dispatch_depth;
        if (owns_mask_lock && !prior_mask_lock) {
            owns_mask_lock = false;
            Exclusion().unlock();
        }
        enabled = prior_enabled;
        owns_mask_lock = prior_mask_lock;
        OSClearContext(&temporary);
        OSSetCurrentContext(prior_context);
    };
    try {
        callback(context);
    } catch (...) {
        restore();
        throw;
    }
    restore();
    return true;
}

bool DispatchNativeInterrupt(void (*callback)()) {
    if (!callback) return false;
    struct Call { void (*callback)(); } call{callback};
    return DispatchNativeInterrupt([](void* context) {
        static_cast<Call*>(context)->callback();
    }, &call);
}
} // namespace mscharged::platform
