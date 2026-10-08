#pragma once
#include "platform/ax_active_voice.h"
#include "platform/ax_command_service.h"
#include <array>
#include <vector>

namespace mscharged::platform {
using NativeAXChannel96 = std::array<std::int32_t, 96>;
using NativeAXStereo96 = std::array<std::int16_t, 192>;
// Device history, never source AXCL globals. The caller must supply the actual
// initialized DSP context and retain it across frames. This leaf does not
// establish cold reset validity, a complete kernel, or an authentic FIR bank.
struct NativeAXCommandHistory {
    std::uint16_t master{}, aux_a{}, aux_b{}, aux_c{}, compressor_counter{};
    bool compressor_counter_known{};
};
bool operator==(const NativeAXCommandHistory&, const NativeAXCommandHistory&) noexcept;

// Literal owned04E6/0618 instruction arithmetic. Stored ramp positions follow
// the multiply pipeline:683/683/682, final requested halfword exactly.
std::array<std::uint16_t,96> NativeAXCommandGainRamp(std::uint16_t before,
                                                  std::uint16_t requested);
NativeAXChannel96 NativeAXMixAuxReturn(const NativeAXChannel96& main,
                                      const NativeAXChannel96& returned,
                                      const std::array<std::uint16_t,96>& gain);
NativeAXStereo96 NativeAXPackStereo(const NativeAXChannel96& left,
                                    const NativeAXChannel96& right,
                                    const std::array<std::uint16_t,96>& gain);
// Literal owned057B..0609 compressor. Any main left/right |sample| >= threshold
// selects attack ramp <counter> (byte offset counter*192) and restarts the
// release counter; otherwise a nonzero counter counts down and selects the
// ramp at 0x840+(counter-1)*192. Without either the firmware skips its table
// DMA and leaves both buses untouched (apply is false).
struct NativeAXCompressorDecision {
    bool apply{};
    std::uint32_t offset{};
    std::uint16_t counter{};
};
NativeAXCompressorDecision NativeAXCompressorStep(std::uint16_t counter, std::uint16_t threshold,
                                                  std::uint16_t release_frames,
                                                  const NativeAXChannel96& left,
                                                  const NativeAXChannel96& right);
// Literal owned LSL16/MULXAC/ASR16 accumulation (0CB3/0CD0): the stored bus
// word is shifted left16 inside a40-bit accumulator, so only its low24 bits
// survive; the doubled product is added and ASR16 yields a24-bit result that
// is stored sign-extended. `contribution` is floor(product/65536).
std::int32_t NativeAXMixAccumulate(std::int32_t bus,std::int32_t contribution);
// Literal owned067C..06A9 remote output: each speaker's main accumulator is
// shifted into AC.M (LSL16) and stored saturated in 40-bit mode.
std::array<std::int16_t,18> NativeAXRemoteOutput(const std::array<std::int32_t,18>& accumulator);
// One ramp sample (owned05E1..05EF under SET15/M2): mixed signed-high and
// unsigned-low products, doubled, combined in the 40-bit accumulator, ASR16,
// then the saturated-middle/raw-low bus store.
std::int32_t NativeAXCompressSample(std::int32_t sample, std::uint16_t gain);
struct NativeAXCommandSpan {
    std::uint32_t address{};
    std::vector<unsigned char> before, after;
};
struct NativeAXPreparedCommandFrame {
    NativeAXCommandHistory before{}, after{};
    std::vector<NativeAXCommandSpan> reads, writes;
    std::vector<NativeAXPreparedVoiceFrame> voices;
    std::uint16_t consumed_words{}, voice_count{}, active_voices{}, aux_commands{};
    std::uint32_t written_bytes{};
};
// A staged normal-output hardware conformance slice. Genuine source order and
// source-owned AUX callback/ring transfers are retained. Nonzero prior surround,
// DPL2, unknown compressor history and unsupported voice features fail before
// stores. It never calls effects, selects a cue, changes source flags,
// replies to a DSP mailbox, or marks the device ready.
// Caller holds the actual source/job fence and every pin through prepare/commit;
// per-transfer memory locks do not provide that whole-job lifetime.
NativeAXPreparedCommandFrame PrepareNativeAXCommandFrame(
    NativeDSPMemoryEndpoint memory, std::uint32_t list_address, std::size_t bytes,
    const NativeAXCommandHistory& history,
    const NativeAXSuppliedCoefficientROM& coefficients);
// Explicit native coefficient-policy entry; source list/PB/output transactions
// are identical. This overload performs no boot/init/callback or IRQ operation.
NativeAXPreparedCommandFrame PrepareNativeAXCommandFrame(
    NativeDSPMemoryEndpoint memory, std::uint32_t list_address, std::size_t bytes,
    const NativeAXCommandHistory& history,
    const NativeAXCoefficientView& coefficients);
void ValidateNativeAXCommandCommit(NativeDSPMemoryEndpoint memory,
                                  const NativeAXPreparedCommandFrame& frame,
                                  const NativeAXCommandHistory& history);
void CommitNativeAXCommandFrame(NativeDSPMemoryEndpoint memory,
                               const NativeAXPreparedCommandFrame& frame,
                               NativeAXCommandHistory& history);
} // namespace mscharged::platform
