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

// Caller-supplied complete coefficient DROM, read through the genuine bus.
// No built-in coefficients, guessed phase-zero row or ratio-one bypass.
// Loading bytes does not establish their authenticity or kernel readiness;
// owner provenance belongs to the device attachment, not sample selection.
class NativeAXSuppliedCoefficientROM {
public:
    void Load(NativeDSPMemoryEndpoint endpoint, std::uint32_t physical_address);
    bool loaded() const noexcept { return loaded_; }
    std::array<std::int16_t, 4> Row(std::uint16_t bank, std::uint16_t fraction) const;
private:
    std::array<std::int16_t, 2048> words_{};
    bool loaded_{};
};

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
    std::array<std::int16_t, 96> resampled{}, enveloped{};
    std::uint32_t decoded_samples{};
    std::uint16_t loops{};
    bool was_running{}, end_reached{};
};
NativeAXPreparedVoiceFrame PrepareNativeAXADPCMVoiceFrame(
    NativeDSPMemoryEndpoint endpoint, std::uint32_t parameter_address,
    const NativeAXSuppliedCoefficientROM& coefficients);
void ValidateNativeAXVoiceCommit(NativeDSPMemoryEndpoint endpoint,
                                const NativeAXPreparedVoiceFrame& frame);
void CommitNativeAXVoiceFrame(NativeDSPMemoryEndpoint endpoint,
                             const NativeAXPreparedVoiceFrame& frame);
} // namespace mscharged::platform
