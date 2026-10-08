#include "platform/ax_active_voice.h"
#include <algorithm>
#include <cstring>
#include <string>

namespace mscharged::platform {
namespace {
std::uint16_t Half(const unsigned char* p) { return (std::uint16_t(p[0]) << 8) | p[1]; }
std::uint32_t Word(const unsigned char* p) { return (std::uint32_t(Half(p)) << 16) | Half(p + 2); }
std::int16_t Signed(std::uint16_t bits) {
    std::int16_t value; std::memcpy(&value, &bits, sizeof(value)); return value;
}
void PutHalf(unsigned char* p, std::uint16_t value) {
    p[0] = static_cast<unsigned char>(value >> 8); p[1] = static_cast<unsigned char>(value);
}
void PutWord(unsigned char* p, std::uint32_t value) {
    PutHalf(p, static_cast<std::uint16_t>(value >> 16)); PutHalf(p + 2, static_cast<std::uint16_t>(value));
}
std::int64_t Floor(std::int64_t value, std::int64_t divisor) {
    return value >= 0 ? value / divisor : -((-value + divisor - 1) / divisor);
}
std::int16_t Saturate(std::int64_t value) {
    return static_cast<std::int16_t>(std::max<std::int64_t>(-32768, std::min<std::int64_t>(32767, value)));
}
[[noreturn]] void Fail(NativeAXVoiceFailure reason, const char* message) {
    throw NativeAXVoiceError(reason, message);
}
// Owned06AB..06C2 one-pole LPF. Both callers run it with SET15/M2/SET40 live
// (set at0356/034C/0364), so the AX1.L coefficient operands of MULX/MADDX are
// unsigned16 while PCM/history in AX0.H stay signed. The two products are
// added before dropping15 fractional bits; the saturated accumulator middle
// is both output and feedback. There is no added rounding.
std::int16_t LowPass06AB(const std::array<std::int16_t, 96>& input, std::array<std::int16_t, 96>& output,
                         std::int16_t history, std::uint16_t a0, std::uint16_t b0) {
    for (std::size_t i = 0; i < 96; ++i) {
        history = Saturate(Floor(std::int64_t(input[i]) * a0 + std::int64_t(history) * b0, 32768));
        output[i] = history;
    }
    return history;
}
struct Bus { unsigned enabled, ramp, depop; };
constexpr std::array<Bus, 12> MixBuses{{
    {0,2,0},{1,2,4},{16,18,1},{17,18,5},{21,23,2},{22,23,6},
    {26,28,3},{27,28,7},{3,4,8},{19,20,9},{24,25,10},{29,30,11}
}};
}
void NativeAXSuppliedCoefficientROM::Load(NativeDSPMemoryEndpoint endpoint,
                                         std::uint32_t address) {
    if (address & 1) throw std::invalid_argument("AX coefficient DROM must be BE16 aligned");
    std::array<unsigned char, 4096> wire{};
    DSPBackendReadMemory(endpoint, address, wire.data(), wire.size());
    std::array<std::int16_t, 2048> next{};
    for (std::size_t i = 0; i < next.size(); ++i) next[i] = Signed(Half(wire.data() + i * 2));
    words_ = next; loaded_ = true;
}
std::array<std::int16_t, 4> NativeAXSuppliedCoefficientROM::Row(
    std::uint16_t bank, std::uint16_t fraction) const {
    if (!loaded_) Fail(NativeAXVoiceFailure::MissingCoefficientBank,
                       "AX four-tap source selection requires supplied authentic coefficient DROM");
    if (bank > 2) Fail(NativeAXVoiceFailure::UnknownCoefficientBank, "AX coefficient selection is outside the three original banks");
    // Owned axDspSlave DB A: 1000/1200/1400; 0739..073D: fraction >>9,
    // <<18 then AC1.M -> AR3, giving128 phases and four consecutive words.
    const auto at = std::size_t(bank) * 512 + std::size_t(fraction >> 9) * 4;
    return {words_[at], words_[at + 1], words_[at + 2], words_[at + 3]};
}
NativeAXCoefficientView NativeAXSuppliedCoefficientROM::View() const noexcept {
    return {NativeAXCoefficientPolicy::SuppliedBank,this,
        [](const void* context,std::uint16_t bank,std::uint16_t fraction) {
            return static_cast<const NativeAXSuppliedCoefficientROM*>(context)->Row(bank,fraction);
        }};
}
NativeAXRemoteBank NativeAXRemoteCoefficientBank(const NativeAXCoefficientView& coefficients) {
    NativeAXRemoteBank bank{};
    for (std::uint16_t phase = 0; phase < 128; ++phase) {
        const auto row = coefficients.Row(0, static_cast<std::uint16_t>(phase << 9));
        std::copy(row.begin(), row.end(), bank.begin() + phase * 4);
    }
    return bank;
}
NativeAXRemoteVoice NativeAXProcessRemoteVoice(const std::array<std::int16_t, 96>& filtered,
                                               unsigned char* pb, const NativeAXRemoteBank& bank0) {
    NativeAXRemoteVoice result;
    // 03A7..03CB: rmtIIR selector2 is the06C3 biquad, any other nonzero
    // value the shared06AB LPF (SET15 still live). Its output, or a plain
    // copy, lands at0D0C behind the four rmtSrc history samples at0D08.
    std::array<std::int16_t, 100> window{};
    for (std::size_t t = 0; t < 4; ++t) window[t] = Signed(Half(pb + 0x10c + t * 2));
    std::array<std::int16_t, 96> input = filtered;
    if (const auto iir = Half(pb + 0x114)) {
        if (iir == 2)
            Fail(NativeAXVoiceFailure::UnsupportedRemote, "AX active remote biquad processing is unqualified");
        const auto history = LowPass06AB(filtered, input, Signed(Half(pb + 0x116)),
                                         Half(pb + 0x118), Half(pb + 0x11a));
        PutHalf(pb + 0x116, static_cast<std::uint16_t>(history));
    }
    std::copy(input.begin(), input.end(), window.begin() + 4);
    // 03CC..03FD (CLR15, M2, SET40): the position starts at rmtSrc's stored
    // fraction and ADDAX advances it by0x00055555 before each of18 outputs.
    // Integer part indexes the window; fraction>>9 selects a bank-0 row. Four
    // signed doubled products are summed by MULAC/ADDP and stored as the
    // saturated AC1.M. The final window position and fraction are the next
    // history (03F3..03FD).
    std::uint32_t position = Half(pb + 0x10a);
    for (std::size_t n = 0; n < 18; ++n) {
        position += 0x55555;
        const auto index = position >> 16;
        const auto* taps = bank0.data() + ((position & 0xffff) >> 9) * 4;
        std::int64_t product = 0;
        for (std::size_t t = 0; t < 4; ++t) product += std::int64_t(window[index + t]) * taps[t] * 2;
        result.resampled[n] = Saturate(Floor(product, 65536));
    }
    const auto last = position >> 16;
    PutHalf(pb + 0x10a, static_cast<std::uint16_t>(position));
    for (std::size_t t = 0; t < 4; ++t)
        PutHalf(pb + 0x10c + t * 2, static_cast<std::uint16_t>(window[last + t]));
    // 02CD..0310 select IRAM[0DB3 + ((rmtMixerCtrl >> 2*channel) & 3)]: 0CB0
    // none, 0CB3 constant, 0CD0 ramp (twice) for main0, aux0, main1, aux1, ...
    // 03FE..0446 call them with SET15 (unsigned volume, signed sample) and M2.
    // A ramp volume advances by delta modulo2^16 per sample and is stored back
    // after18 samples. The last product's saturated AC0.M is the channel's
    // rmtDpop; the none handler stores zero.
    const auto control = Half(pb + 0xd8);
    for (std::size_t ch = 0; ch < 8; ++ch) {
        const auto handler = (control >> (ch * 2)) & 3;
        const std::size_t depop = (ch % 2 ? 0x102 : 0xfa) + (ch / 2) * 2;
        if (!handler) {
            PutHalf(pb + depop, 0);
            continue;
        }
        auto volume = Half(pb + 0xda + ch * 4);
        const auto delta = Half(pb + 0xdc + ch * 4);
        for (std::size_t i = 0; i < 18; ++i) {
            result.channels[ch][i] =
                static_cast<std::int32_t>(Floor(std::int64_t(result.resampled[i]) * volume, 32768));
            if (handler >= 2) volume = static_cast<std::uint16_t>(volume + delta);
        }
        if (handler >= 2) PutHalf(pb + 0xda + ch * 4, volume);
        PutHalf(pb + depop, static_cast<std::uint16_t>(Saturate(result.channels[ch].back())));
        result.mixed |= static_cast<std::uint8_t>(1u << ch);
    }
    return result;
}
NativeAXPreparedVoiceFrame PrepareNativeAXADPCMVoiceFrame(
    NativeDSPMemoryEndpoint endpoint, std::uint32_t address,
    const NativeAXCoefficientView& coefficients) {
    NativeAXPreparedVoiceFrame result; result.parameter_address = address;
    DSPBackendReadMemory(endpoint, address, result.parameters_before.data(), result.parameters_before.size());
    if (Word(result.parameters_before.data() + 4) != address)
        Fail(NativeAXVoiceFailure::InvalidSelfAddress, "AX voice PB has no genuine self address");
    auto state = ReadNativeAXADPCMState(endpoint, address);
    // There is no whole-job lease in per-transfer DSP memory calls. Detect a
    // source mutation even during these two reads; the original device fence
    // must still exclude all CPU changes for this whole prepare/commit job.
    std::array<unsigned char, 320> verified{};
    DSPBackendReadMemory(endpoint, address, verified.data(), verified.size());
    if (verified != result.parameters_before)
        Fail(NativeAXVoiceFailure::SourceChanged, "AX source PB changed while preparing its device frame");
    result.parameters_after = result.parameters_before;
    result.was_running = state.running;
    if (!state.running) return result;
    if (state.src_select > 1)
        Fail(NativeAXVoiceFailure::UnsupportedSRC,
             ("AX active SRC selector=" + std::to_string(state.src_select) +
              " ratio16.16=" + std::to_string(state.ratio) +
              " fraction=" + std::to_string(state.fraction) +
              "; direct/unknown processing is unqualified").c_str());
    if (state.src_select == 0)
        coefficients.Row(state.coefficient_select, state.fraction); // Fail before any sample reads/stores.
    if (state.ratio > 0x40000)
        Fail(NativeAXVoiceFailure::UnsupportedRate, "AX active slice qualifies ratios0..4 only, never clamps a source ratio");
    if (state.gain != 0)
        Fail(NativeAXVoiceFailure::UnsupportedGain, "AX ADPCM nonzero accelerator gain is outside the owned sample proof");
    const auto* before = result.parameters_before.data();
    if (Half(before + 0x44)) Fail(NativeAXVoiceFailure::UnsupportedITD, "AX active ITD/history processing is unqualified");
    if (Half(before + 0xc2))
        Fail(NativeAXVoiceFailure::UnsupportedFilter, "AX active biquad processing is unqualified");
    const bool remote = Half(before + 0xd6) != 0;
    if (remote) {
        // Fail before any store: rmtIIR biquad and a missing bank-0 row.
        if (Half(before + 0x114) == 2)
            Fail(NativeAXVoiceFailure::UnsupportedRemote, "AX active remote biquad processing is unqualified");
        coefficients.Row(0, Half(before + 0x10a));
    }
    const auto mixer = Word(before + 0x0c);
    if (mixer & ~std::uint32_t(0x7fff001f))
        Fail(NativeAXVoiceFailure::UnsupportedMixer, "AX DPL2/reserved mixer selection is outside this voice slice");

    auto history = state.src_history;
    std::uint32_t fraction = state.fraction;
    auto* after = result.parameters_after.data();
    // Original active voice processing clears12 main and8 remote depop cells
    // at firmware038E..0395. Stopped voices skip these stores entirely.
    std::fill(after + 0x52, after + 0x6a, 0);
    std::fill(after + 0xfa, after + 0x10a, 0);
    for (std::size_t i = 0; i < 96; ++i) {
        const auto position = fraction + state.ratio;
        const auto count = position >> 16;
        fraction = position & 0xffff;
        for (std::uint32_t n = 0; n < count; ++n) {
            std::int16_t sample = 0;
            if (state.running) {
                const auto decoded = DecodeNativeAXRawADPCM(endpoint, state, 1);
                state = decoded.next;
                sample = decoded.samples[0];
                result.decoded_samples += decoded.samples_decoded;
                result.loops += decoded.loops;
                result.end_reached |= decoded.end_reached;
            }
            // Original end IRQ F61..F71 switches AR2 to the initialized zero
            // cell0CE9 for a nonloop voice; the rest of its96-sample FIR/VE
            // frame still executes. Its accelerator histories stay terminal.
            std::rotate(history.begin(), history.begin() + 1, history.end());
            history.back() = sample;
        }
        if (state.src_select == 0) {
            const auto taps = coefficients.Row(state.coefficient_select, static_cast<std::uint16_t>(fraction));
            std::int64_t product = 0;
            for (std::size_t t = 0; t < 4; ++t) product += std::int64_t(history[t]) * taps[t] * 2;
            // Firmware0744..074A: four signed products, ADDP, saturated AC1.M
            // store in40-bit mode. No added rounding or float interpolation.
            result.resampled[i] = Saturate(Floor(product, 65536));
        } else {
            // Original076D..07AE selects unsigned low multiply operands and
            // no product shift. The circular history pointer names its first
            // two chronological cells after the carry reads. At zero phase
            // 0795 copies the first cell; 0799..079D adds signedPCM * unsigned
            // (65536-phase) and signedPCM * unsigned phase before AC1.M drops
            // the low16 bits. This path neither reads nor substitutes DROM.
            result.resampled[i] = fraction == 0 ? history[0] :
                Saturate(Floor(std::int64_t(history[0]) * (65536 - fraction) +
                               std::int64_t(history[1]) * fraction, 65536));
        }
    }
    state.fraction = static_cast<std::uint16_t>(fraction);
    state.src_history = history;
    PutHalf(after + 0x10, state.running ? 1 : 0);
    PutWord(after + 0x7a, state.current_nibble);
    PutHalf(after + 0xa0, state.predictor_scale);
    PutHalf(after + 0xa2, static_cast<std::uint16_t>(state.history1));
    PutHalf(after + 0xa4, static_cast<std::uint16_t>(state.history2));
    PutHalf(after + 0xaa, state.fraction);
    for (std::size_t i = 0; i < 4; ++i) PutHalf(after + 0xac + i * 2, static_cast<std::uint16_t>(history[i]));

    auto envelope = Half(before + 0x6a);
    const auto delta = Signed(Half(before + 0x6c));
    for (std::size_t i = 0; i < 96; ++i) {
        // Original034D..0374 makes96 unsigned16 VE values in16-bit mode,
        // then signedPCM * unsignedVE in40-bit mode, with middle saturation.
        result.enveloped[i] = Saturate(Floor(std::int64_t(result.resampled[i]) * envelope, 32768));
        envelope = static_cast<std::uint16_t>(envelope + delta);
    }
    PutHalf(after + 0x6a, envelope);
    result.filtered = result.enveloped;
    if (Half(before + 0xba)) {
        // The LPF follows VE and precedes mixing (firmware0375..0381).
        const auto history = LowPass06AB(result.enveloped, result.filtered, Signed(Half(before + 0xbc)),
                                         Half(before + 0xbe), Half(before + 0xc0));
        PutHalf(after + 0xbc, static_cast<std::uint16_t>(history));
    }
    for (std::size_t bus = 0; bus < MixBuses.size(); ++bus) {
        const auto& selected = MixBuses[bus];
        if (!(mixer & (std::uint32_t(1) << selected.enabled))) continue;
        auto volume = Half(before + 0x14 + bus * 4);
        const auto volume_delta = Signed(Half(before + 0x16 + bus * 4));
        const bool ramp = (mixer & (std::uint32_t(1) << selected.ramp)) != 0;
        for (std::size_t i = 0; i < 96; ++i) {
            result.buses[bus][i] = static_cast<std::int32_t>(Floor(std::int64_t(result.filtered[i]) * volume, 32768));
            if (ramp) volume = static_cast<std::uint16_t>(volume + volume_delta);
        }
        if (ramp) PutHalf(after + 0x14 + bus * 4, volume);
        // The actual mixer returns last signed contribution via AX0.L,
        // saturated AC0.M in40-bit mode, for the original depop field.
        PutHalf(after + 0x52 + selected.depop * 2,
                static_cast<std::uint16_t>(Saturate(result.buses[bus].back())));
    }
    if (remote) {
        result.remote = NativeAXProcessRemoteVoice(result.filtered, after, NativeAXRemoteCoefficientBank(coefficients));
        result.remote_enabled = true;
    }
    return result;
}
NativeAXPreparedVoiceFrame PrepareNativeAXADPCMVoiceFrame(
    NativeDSPMemoryEndpoint endpoint,std::uint32_t address,
    const NativeAXSuppliedCoefficientROM& coefficients) {
    return PrepareNativeAXADPCMVoiceFrame(endpoint,address,coefficients.View());
}
void ValidateNativeAXVoiceCommit(NativeDSPMemoryEndpoint endpoint,
                                const NativeAXPreparedVoiceFrame& frame) {
    DSPBackendValidateMemory(endpoint, frame.parameter_address, frame.parameters_after.size(), true);
    std::array<unsigned char, 320> current{};
    DSPBackendReadMemory(endpoint, frame.parameter_address, current.data(), current.size());
    if (current != frame.parameters_before)
        Fail(NativeAXVoiceFailure::SourceChanged, "AX source PB changed before its original device completion boundary");
}
void CommitNativeAXVoiceFrame(NativeDSPMemoryEndpoint endpoint,
                             const NativeAXPreparedVoiceFrame& frame) {
    ValidateNativeAXVoiceCommit(endpoint, frame);
    DSPBackendWriteMemory(endpoint, frame.parameter_address, frame.parameters_after.data(), frame.parameters_after.size());
}
} // namespace mscharged::platform
