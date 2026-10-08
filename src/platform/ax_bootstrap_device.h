#pragma once
#include "platform/dsp_control.h"
#include "platform/dsp_memory.h"
#include "platform/ax_command_service.h"
#include <array>
#include <cstdint>
#include <memory>

namespace mscharged::platform {
class DSPInstructionCore;
class NativeAXFunctionalDevice;
// Explicit native hardware processor only: callback returns after genuine
// checked command/PB/output stores. It never invokes source callbacks or mails.
struct NativeAXDeviceFrameResult {
    std::uint16_t consumed_words{},stopped_voices{},stereo_frames{},remote_samples_per_channel{};
    std::uint32_t written_bytes{};
};
struct NativeAXFrameProcessor {
    void* context{};
    NativeAXDeviceFrameResult (*process)(void*,NativeDSPMemoryEndpoint,std::uint32_t,std::size_t){};
};
// Sealed typed native processor. Only the actual functional device can create
// this lifetime contract; arbitrary ready/frame callbacks cannot select it.
class NativeAXFunctionalProcessor {
private:
    void* context_;
    void (*initialize_)(void*,NativeDSPMemoryEndpoint);
    void (*validate_reset_)(void*);
    void (*reset_)(void*);
    NativeAXDeviceFrameResult (*process_)(void*,NativeDSPMemoryEndpoint,std::uint32_t,std::size_t);
    NativeAXFunctionalProcessor(void* context,decltype(initialize_) initialize,
        decltype(validate_reset_) validate_reset,decltype(reset_) reset,decltype(process_) process)
        :context_(context),initialize_(initialize),validate_reset_(validate_reset),reset_(reset),process_(process) {}
    friend class NativeAXFunctionalDevice;
    friend class NativeAXBootstrapDevice;
};
// NativeKernelInitialized is a distinct functional platform state: never ISA
// prefix completion or ROM/conformance readiness.
enum class NativeAXBootstrapPhase { Cold, LoaderReady, Loading, InitPrefixCompleted, Faulted, Retired,
                                    NativeKernelInitialized };
struct NativeAXBootstrapStatus {
    NativeAXBootstrapPhase phase;
    std::uint16_t loader_words;
    std::uint64_t firmware_instructions, resets;
    bool hardware_halted;
};
enum class NativeAXFrameMode { BootstrapOnly, StoppedVoices };
enum class NativeAXFramePhase {
    Unavailable, ReadyForListSize, ReadyForListAddress,
    WaitingSyncAcknowledgment, WaitingContinue, Faulted
};
struct NativeAXStoppedVoiceStatus {
    NativeAXFramePhase phase;
    std::uint64_t processed_frames, completed_frames;
    std::uint64_t sync_interrupts, yield_interrupts, source_continues;
    std::uint32_t last_list_address, written_bytes;
    std::uint16_t stopped_voices, stereo_frames, remote_samples;
};
// Explicit processor conformance endpoint, NOT a full AX kernel or production
// ready service. All callers retain actual source-image/pin/SDK lifetimes.
// Only the exact8192-byte source firmware and its ten-word loader are accepted.
// Its real21instruction init prefix emits INIT+DIRQ; original source __DSPHandler
// owns all task flags/callbacks. The default still rejects every frame request.
// Explicit StoppedVoices mode implements native hardware command semantics
// for the qualified zero-input slice only, not full DSP firmware execution.
// Explicit native providers can attach only after the acknowledged init prefix;
// each retains its own real processing/bounds/unsupported gates. An attachment
// is not full firmware/kernel readiness or admission of an original game cue.
class NativeAXBootstrapDevice {
public:
    NativeAXBootstrapDevice(NativeDSPMemoryEndpoint memory,
                            NativeDSPMailboxEndpoint mailboxes,
                            NativeDSPControlEndpoint control,
                            std::uint32_t firmware_address);
    NativeAXBootstrapDevice(NativeDSPMemoryEndpoint memory,
                            NativeDSPMailboxEndpoint mailboxes,
                            NativeDSPControlEndpoint control,
                            std::uint32_t firmware_address,
                            NativeAXFrameMode frame_mode);
    // Adopt the same halted chip after original OS boot. Reset/loader overwrite
    // instruction execution only; actual cold DRAM and supplied ROMs survive.
    NativeAXBootstrapDevice(NativeDSPMemoryEndpoint memory,
                            NativeDSPMailboxEndpoint mailboxes,
                            NativeDSPControlEndpoint control,
                            std::uint32_t firmware_address,
                            NativeAXFrameMode frame_mode,DSPInstructionCore& retained_chip);
    // Explicit ROM-free functional hardware selection. Actual initialization
    // resources and supported work are provided by a sealed native device;
    // this never creates a core or marks InitPrefixCompleted.
    NativeAXBootstrapDevice(NativeDSPMemoryEndpoint memory,
                            NativeDSPMailboxEndpoint mailboxes,
                            NativeDSPControlEndpoint control,
                            std::uint32_t firmware_address,
                            const NativeAXFunctionalProcessor& processor);
    ~NativeAXBootstrapDevice();
    NativeAXBootstrapDevice(const NativeAXBootstrapDevice&)=delete;
    NativeAXBootstrapDevice& operator=(const NativeAXBootstrapDevice&)=delete;
    // Bounded alternative hardware processing after the actual init prefix and
    // its source acknowledgment. Default/bootstrap behavior stays unchanged.
    // Caller owns this provider until actual halt/drain and explicit detach.
    // Identity gate for borrowing actual initialized chip state, not a caller
    // context or a separate initialized core.
    void RequireRetainedChip(const DSPInstructionCore& chip) const;
    void AttachFrameProcessor(NativeAXFrameProcessor processor);
    void DetachFrameProcessor(void* context);
    void ServiceOwner();
    NativeAXBootstrapStatus Status() const;
    NativeAXStoppedVoiceStatus FrameStatus() const;
    // Read actual initialized master/AUX words; reject cold/loading/fault/reset.
    // Does not expose or invent compressor history or command/kernel readiness.
    std::array<std::uint16_t,4> InitializedGainWords() const;
    // Requires actual HALT; no thread/job/source flags are repaired here.
    void Close();
private:
    struct State;
    std::unique_ptr<State> state_;
};
} // namespace mscharged::platform
