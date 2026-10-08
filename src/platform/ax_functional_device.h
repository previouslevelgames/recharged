#pragma once
#include "platform/ax_bootstrap_device.h"
#include "platform/ax_frame_commands.h"
#include "platform/ax_native_filter.h"
#include <array>
#include <memory>

namespace mscharged::platform {
class NativeOSAudioRegisterOwner;
// Actual source storage order from ax_storage_abi.h: command,PB,ITD,AUX A/B/C,
// compressor,Studio,PCM16,surround,remote,DRAM context,source firmware. Source
// pins/module leases and source exclusion must outlive every complete job.
struct NativeAXFunctionalBindings { std::array<std::uint32_t,13> addresses; };
struct NativeAXFunctionalStatus {
    NativeAXBootstrapStatus protocol;
    NativeAXStoppedVoiceStatus frames;
    NativeAXCommandHistory history;
    NativeAXCoefficientPolicy coefficient_policy;
    std::uint64_t initialization_count{},processed_frames{};
    std::uint16_t last_active_voices{},last_aux_commands{};
    bool native_initialized{};
};
// Explicit functional-native Wii AX hardware contract. It creates no ISA core,
// supplies no console ROM and reports no authentic init-prefix completion. The
// genuine original task/handler/AI still owns requests, flags and callbacks.
// Only NativeWindowedSinc4TapV1 is admitted here; strict supplied-bank device
// remains separate. Unsupported source commands/voices fail before completion.
class NativeAXFunctionalDevice {
public:
    NativeAXFunctionalDevice(NativeDSPMemoryEndpoint memory,
        NativeDSPMailboxEndpoint mailboxes,NativeDSPControlEndpoint control,
        const NativeAXFunctionalBindings& bindings,NativeAXCoefficientPolicy policy);
    ~NativeAXFunctionalDevice();
    NativeAXFunctionalDevice(const NativeAXFunctionalDevice&)=delete;
    NativeAXFunctionalDevice& operator=(const NativeAXFunctionalDevice&)=delete;
    void ServiceOwner();
    NativeAXFunctionalStatus Status() const;
    // Requires actual AI stop/drain and source HALT/mail/IRQ/job drain. Source
    // flags/task globals stay unchanged. Retire pins/source image afterwards.
    void Close();
private:
    std::uint16_t ReadOSDSP(std::uint32_t reg);
    void WriteOSDSP(std::uint32_t reg,std::uint16_t value);
    std::uint32_t ReadOSDSPPair(std::uint32_t reg);
    struct State;
    std::unique_ptr<State> state_;
    std::unique_ptr<NativeAXBootstrapDevice> protocol_;
    std::unique_ptr<NativeOSAudioRegisterOwner> registers_;
};
} // namespace mscharged::platform
