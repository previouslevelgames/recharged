#pragma once
#include "platform/ax_bootstrap_device.h"
#include "platform/ax_frame_commands.h"
#include "platform/dsp_instruction_core.h"
#include <memory>

namespace mscharged::platform {
struct NativeAXNormalDeviceStatus {
    NativeAXCommandHistory history;
    std::uint64_t processed_frames{};
    std::uint16_t last_active_voices{},last_aux_commands{};
};
// Bounded native hardware attachment, not full firmware/kernel readiness.
// Actual init-prefix gains and a valid cold compressor cell are prerequisites.
// A supplied bank must match the retained chip; authenticity remains external.
// Unsupported branches fault before PCM/PB stores and completion causes.
class NativeAXNormalCommandDevice {
public:
    NativeAXNormalCommandDevice(NativeAXBootstrapDevice& device,
        NativeDSPMemoryEndpoint memory,DSPInstructionCore& chip,
        const NativeAXSuppliedCoefficientROM& coefficients);
    ~NativeAXNormalCommandDevice();
    NativeAXNormalCommandDevice(const NativeAXNormalCommandDevice&)=delete;
    NativeAXNormalCommandDevice& operator=(const NativeAXNormalCommandDevice&)=delete;
    NativeAXNormalDeviceStatus Status() const;
    // Source hardware must have actually halted/drained; then detach the frame
    // processor before closing the parent device, releasing pins or source code.
    void Close();
private:
    struct State;
    std::unique_ptr<State> state_;
};
}
