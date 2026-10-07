#pragma once
#include "platform/dsp_memory.h"
#include "platform/native_ax_module_memory_abi.h"
#include <cstdint>
#include <memory>

namespace mscharged::platform {
struct NativeAXModuleMemoryStatus {
    bool armed, reserved, loaded, retired;
    std::uint32_t spans, reserved_bytes;
    std::uintptr_t image_base;
    ChargedAXModuleArenaSnapshot before, after;
};
// Linux host-loader ownership only. Construct after real OS arenas, before
// loading the original module. No game allocation/initialization/task/audio
// decision is supplied. Actual module static owners are never unloaded here.
class NativeAXModuleMemory {
public:
    explicit NativeAXModuleMemory(const char* module_path);
    ~NativeAXModuleMemory();
    NativeAXModuleMemory(const NativeAXModuleMemory&)=delete;
    NativeAXModuleMemory& operator=(const NativeAXModuleMemory&)=delete;
    void ConfirmLoaded(void* actual_loader_handle);
    NativeAXModuleMemoryStatus Status() const;
    NativeDSPMemoryEndpoint Endpoint() const;
    std::uint32_t PhysicalAddress(unsigned source_span) const;
    // Caller must halt/drain every actual DSP job first. Per-transfer memory
    // exclusion is not a complete job fence. AI must already be stopped/drained.
    // The original module's initial loader handle and source statics stay live.
    void ReleaseAfterDeviceDrain();
private:
    struct State;
    std::unique_ptr<State> state_;
    friend void ::ChargedNativeAXReserveModuleStorage(const ChargedAXStorage*,
        uint32_t, ChargedAXStorage, ChargedAXModuleArenaSnapshot,
        ChargedAXModuleArenaObserver);
};
}
