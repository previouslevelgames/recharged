#pragma once
#include <revolution/ipc.h>
#include <cstddef>
#include <cstdint>

namespace mscharged::platform {
struct NativeIOSDevice {
    void* context;
    bool (*owns_path)(void*, const char*);
    // File descriptors in this request belong to the device, not the caller.
    // Execute performs hardware work only; it must not invoke game callbacks.
    s32 (*execute)(void*, const IPCRequest&);
    // Optional persistent device event, such as STM's one-shot power interrupt.
    // The device retains/delivers this callback through its own real event path.
    s32 (*ioctl_async)(void*, const IPCRequest&, IPCAsyncCallback, void*){};
};
struct NativeIOSDeviceLease { std::uint64_t generation; };
struct NativeIOSStatus { std::size_t devices, descriptors, pending; bool active; };

NativeIOSDeviceLease RegisterNativeIOSDevice(NativeIOSDevice device);
// Device storage and callbacks must still be alive. Retirement rejects queued
// or active requests; the device closes its actual handles before retiring.
void UnregisterNativeIOSDevice(NativeIOSDeviceLease lease);
bool ServiceNativeIOSRequests();
NativeIOSStatus GetNativeIOSStatus();
}
