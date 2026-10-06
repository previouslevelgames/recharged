#pragma once

#include <dolphin/os/OSInterrupt.h>
#include <cstdint>

namespace mscharged::platform {

// A real native device retains this line for one controller lifetime. It may
// assert/deassert the level from a worker, but never execute source handlers.
struct NativeInterruptSource {
    __OSInterrupt interrupt;
    std::uint64_t generation;
};

struct NativeInterruptControllerStatus {
    bool initialized;
    bool owner_thread;
    u32 user_mask;
    u32 current_mask;
    u32 pending_mask;
    unsigned dispatch_depth;
    std::uint64_t generation;
    std::uint64_t dispatched;
};

// Host SDK setup precedes original module/static construction. Shutdown follows
// device-worker drain; old device handles are rejected after restart.
// Pure host DSP hardware sink, called on the owner after controller unlock.
// It mirrors original OSInterrupt CSR mask writes; it executes no source handler.
using NativeDSPMaskObserver = void (*)(u32 effective_mask, void* context);
void AttachNativeDSPMaskObserver(NativeDSPMaskObserver observer, void* context);
void DetachNativeDSPMaskObserver(NativeDSPMaskObserver observer, void* context);
void InitializeNativeInterruptController();
void ShutdownNativeInterruptController();
NativeInterruptSource GetNativeInterruptSource(__OSInterrupt interrupt);
bool SetNativeInterruptPending(NativeInterruptSource source, bool pending);
bool ServiceNativeInterruptController();
NativeInterruptControllerStatus GetNativeInterruptControllerStatus();

} // namespace mscharged::platform
