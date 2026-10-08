#pragma once
#include "platform/ax_adpcm_samples.h"
#include <array>
#include <cstddef>
#include <cstdint>
#include <stdexcept>

namespace mscharged::platform {
enum class NativeAXVoiceFailure {
    MissingCoefficientBank, UnknownCoefficientBank, UnsupportedSRC,
    UnsupportedRate, UnsupportedITD, UnsupportedFilter, UnsupportedRemote,
    UnsupportedMixer, UnsupportedGain, InvalidSelfAddress, SourceChanged
};
class NativeAXVoiceError : public std::runtime_error {
public:
    NativeAXVoiceError(NativeAXVoiceFailure reason, const char* message)
        : std::runtime_error(message), reason_(reason) {}
    NativeAXVoiceFailure reason() const noexcept { return reason_; }
private:
    NativeAXVoiceFailure reason_;
};

enum class NativeAXCoefficientPolicy { SuppliedBank, NativeWindowedSinc4TapV1 };
class NativeAXNativeFilter;
class NativeAXSuppliedCoefficientROM;
// Borrowed immutable processing input, never device/bootstrap readiness. The
// provider and its actual bytes/rows must outlive every prepared hardware job.
class NativeAXCoefficientView {
public:
    NativeAXCoefficientPolicy Policy() const noexcept { return policy_; }
    std::array<std::int16_t,4> Row(std::uint16_t bank,std::uint16_t fraction) const {
        return row_(context_,bank,fraction);
    }
private:
    using Reader=std::array<std::int16_t,4>(*)(const void*,std::uint16_t,std::uint16_t);
    NativeAXCoefficientView(NativeAXCoefficientPolicy policy,const void* context,Reader row)
        : policy_(policy),context_(context),row_(row) {}
    NativeAXCoefficientPolicy policy_;
    const void* context_;
    Reader row_;
    friend class NativeAXNativeFilter;
    friend class NativeAXSuppliedCoefficientROM;
};

// Caller-supplied complete coefficient DROM, read through the genuine bus.
// No built-in coefficients, guessed phase-zero row or ratio-one bypass.
// Loading bytes does not establish their authenticity or kernel readiness;
// owner provenance belongs to the device attachment, not sample selection.
class NativeAXSuppliedCoefficientROM {
public:
    void Load(NativeDSPMemoryEndpoint endpoint, std::uint32_t physical_address);
    bool loaded() const noexcept { return loaded_; }
    NativeAXCoefficientView View() const noexcept;
    std::array<std::int16_t, 4> Row(std::uint16_t bank, std::uint16_t fraction) const;
private:
    std::array<std::int16_t, 2048> words_{};
    bool loaded_{};
};

// Literal owned03A2..0446 remote speaker branch for one running voice, after
// its main mixing and on the same filtered96 samples: rmtIIR off or the shared
// 06AB LPF (the biquad selector fails explicitly), the fixed0x55555 four-tap
// bank-0 resampler to18 samples, then the eight2-bit rmtMixerCtrl handlers
// (0CB0 none, 0CB3 constant, 0CD0 ramp) for main0, aux0, ..., main3, aux3.
// It updates only the PB remote fields0xD8..0x127 in `parameters`; callers add
// `channels` to the frame's remote accumulators (NativeAXMixAccumulate).
struct NativeAXRemoteVoice {
    std::array<std::array<std::int32_t, 18>, 8> channels{};
    std::array<std::int16_t, 18> resampled{};
    std::uint8_t mixed{}; // bit per channel with a nonzero handler
};
// Coefficient DROM0x1000..0x11FF as the remote resampler reads it: 128 phase
// rows of four words selected by fraction>>9 (03E7..03EB), the same row
// selection both coefficient policies implement.
using NativeAXRemoteBank = std::array<std::int16_t, 512>;
NativeAXRemoteBank NativeAXRemoteCoefficientBank(const NativeAXCoefficientView& coefficients);
NativeAXRemoteVoice NativeAXProcessRemoteVoice(const std::array<std::int16_t, 96>& filtered,
                                               unsigned char* parameters, const NativeAXRemoteBank& bank0);

// A staged source-PB transaction, not a command-list/output device. Process
// every contributing voice and actual command prerequisites before committing
// any PB/output; unsupported work cannot produce a SYNC/completion interrupt.
// The source CPU exclusion and real pin/job lifetime must span prepare/commit.
struct NativeAXPreparedVoiceFrame {
    std::uint32_t parameter_address{};
    std::array<unsigned char, 320> parameters_before{}, parameters_after{};
    // Original mix field order: L/R, AuxA L/R, AuxB L/R, AuxC L/R,
    // S/AuxAS/AuxBS/AuxCS. Signed contributions remain wider than PCM16.
    std::array<std::array<std::int32_t, 96>, 12> buses{};
    std::array<std::int16_t, 96> resampled{}, enveloped{}, filtered{};
    NativeAXRemoteVoice remote{};
    bool remote_enabled{};
    std::uint32_t decoded_samples{};
    std::uint16_t loops{};
    bool was_running{}, end_reached{};
};
NativeAXPreparedVoiceFrame PrepareNativeAXADPCMVoiceFrame(
    NativeDSPMemoryEndpoint endpoint, std::uint32_t parameter_address,
    const NativeAXSuppliedCoefficientROM& coefficients);
// Explicit platform-policy entry. Exact source position/history/PB arithmetic
// is shared with the supplied-bank entry; native rows never imply Wii parity.
NativeAXPreparedVoiceFrame PrepareNativeAXADPCMVoiceFrame(
    NativeDSPMemoryEndpoint endpoint, std::uint32_t parameter_address,
    const NativeAXCoefficientView& coefficients);
void ValidateNativeAXVoiceCommit(NativeDSPMemoryEndpoint endpoint,
                                const NativeAXPreparedVoiceFrame& frame);
void CommitNativeAXVoiceFrame(NativeDSPMemoryEndpoint endpoint,
                             const NativeAXPreparedVoiceFrame& frame);
} // namespace mscharged::platform
