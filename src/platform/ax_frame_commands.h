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
// source-owned AUX callback/ring transfers are retained. Nonzero Studio/prior
// surround, DPL2, compressor attack/history and unsupported voice features fail
// before stores. It never calls effects, selects a cue, changes source flags,
// replies to a DSP mailbox, or marks the device ready.
// Caller holds the actual source/job fence and every pin through prepare/commit;
// per-transfer memory locks do not provide that whole-job lifetime.
NativeAXPreparedCommandFrame PrepareNativeAXCommandFrame(
    NativeDSPMemoryEndpoint memory, std::uint32_t list_address, std::size_t bytes,
    const NativeAXCommandHistory& history,
    const NativeAXSuppliedCoefficientROM& coefficients);
void ValidateNativeAXCommandCommit(NativeDSPMemoryEndpoint memory,
                                  const NativeAXPreparedCommandFrame& frame,
                                  const NativeAXCommandHistory& history);
void CommitNativeAXCommandFrame(NativeDSPMemoryEndpoint memory,
                               const NativeAXPreparedCommandFrame& frame,
                               NativeAXCommandHistory& history);
} // namespace mscharged::platform
