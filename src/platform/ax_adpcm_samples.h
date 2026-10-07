#pragma once
#include "platform/dsp_memory.h"
#include <array>
#include <cstddef>
#include <cstdint>

namespace mscharged::platform {
// Snapshot of the actual typed physical AXPB sample fields. This is device
// arithmetic state, not a game voice owner or a replacement sample selector.
struct NativeAXADPCMState {
    std::array<std::array<std::int16_t,2>,8> coefficients{};
    std::uint32_t current_nibble{}, end_nibble{}, loop_nibble{}, ratio{};
    std::uint16_t predictor_scale{}, gain{}, loop_predictor_scale{};
    std::int16_t history1{}, history2{}, loop_history1{}, loop_history2{};
    std::uint16_t src_select{}, coefficient_select{}, fraction{}, voice_type{}, loop_flag{};
    std::array<std::int16_t,4> src_history{};
    bool running{};
};
NativeAXADPCMState ReadNativeAXADPCMState(NativeDSPMemoryEndpoint endpoint,
                                       std::uint32_t parameter_block_address);
struct NativeAXRawADPCMBlock {
    std::array<std::int16_t,96> samples{};
    NativeAXADPCMState next{};
    std::uint16_t samples_decoded{}, loops{};
    bool end_reached{};
};
// Raw accelerator/codec samples only: no SRC, gain/envelope/filter/mix/output,
// source PB writeback, callback, DMA, ready/boot mail. A source-selected four-tap
// voice MUST NOT substitute these raw samples for its resampled frame.
// Input pins/fields stay live and unmodified across this read-only device job.
// Supported end geometry is an inclusive payload nibble (offset2..15); the
// hardware's special header-nibble end addresses remain explicit unsupported.
NativeAXRawADPCMBlock DecodeNativeAXRawADPCM(NativeDSPMemoryEndpoint endpoint,
                                           const NativeAXADPCMState& state,
                                           std::size_t raw_sample_limit=96);
// Explicit admission boundary for future 96-frame voice processing. In
// particular ratio 1 does not authorize changing default srcSelect 0 to direct.
void RequireNativeAXADPCMFrameProcessing(const NativeAXADPCMState& state);
} // namespace mscharged::platform
