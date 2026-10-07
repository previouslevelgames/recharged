#pragma once

#include <dolphin/types.h>

namespace mscharged::platform {

// One shared host exclusion boundary represents source critical sections and
// hardware interrupt dispatch. Device workers never execute game callbacks.
class NativeInterruptGuard {
public:
    NativeInterruptGuard();
    ~NativeInterruptGuard();
    NativeInterruptGuard(const NativeInterruptGuard&) = delete;
    NativeInterruptGuard& operator=(const NativeInterruptGuard&) = delete;
};

// A device worker may copy a DMA buffer only between source critical sections.
// It must not block while SDL holds its stream lock, which would invert locks.
class NativeInterruptRead {
public:
    NativeInterruptRead();
    ~NativeInterruptRead();
    explicit operator bool() const noexcept { return acquired_; }
    NativeInterruptRead(const NativeInterruptRead&) = delete;
    NativeInterruptRead& operator=(const NativeInterruptRead&) = delete;
private:
    bool acquired_;
};

bool NativeInterruptsEnabled() noexcept;
// Remains true if an IRQ callback explicitly enables its interrupt mask.
bool NativeInterruptDispatchActive() noexcept;
// A blocked native waiter can release only the source OSDisableInterrupts lock.
// Extra host guards or an active IRQ context cannot be descheduled this way.
bool NativeInterruptWaitAllowed() noexcept;
bool DispatchNativeInterrupt(void (*callback)());
bool DispatchNativeInterrupt(void (*callback)(void*), void* context);

} // namespace mscharged::platform
