// The Wii Remote speaker's 8-bit PCM against the game's original encoder
// (WENC, compiled from the prepared source): decoding must give exactly the
// encoder's own reconstruction, sample for sample, and the 3000 Hz PCM must
// keep the sound below 1200 Hz and drop what would fold back from above.
#include "platform/wiimote_speaker.h"

#include <revolution/wenc.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {
unsigned checks{}, failures{};
void Check(bool value, const char* message) {
    ++checks;
    if (!value) { ++failures; std::fprintf(stderr, "Wii Remote speaker: %s\n", message); }
}
constexpr double kPi = 3.14159265358979323846;

std::int32_t Field(const WENCInfo& info, int index) {
    std::int32_t value;
    std::memcpy(&value, info.data + index * 4, 4);
    return value;
}

std::vector<std::int16_t> Signal(int kind, std::size_t count) {
    std::vector<std::int16_t> samples(count);
    std::uint32_t random = 12345;
    for (std::size_t i = 0; i < count; ++i) {
        const double t = double(i) / 6000;
        double value = 0;
        switch (kind) {
        case 0: value = 0; break;
        case 1: value = 8000 * std::sin(2 * kPi * 440 * t); break;
        case 2: value = 30000 * std::sin(2 * kPi * (100 + 1400 * t) * t); break; // sweep
        case 3: value = (i / 7) % 2 ? 32767 : -32768; break;                      // hard square
        case 4: random = random * 1664525 + 1013904223; value = std::int16_t(random >> 16); break;
        case 5: value = i % 300 == 0 ? 32767 : 0; break; // clicks
        }
        samples[i] = static_cast<std::int16_t>(std::lround(std::clamp(value, -32768.0, 32767.0)));
    }
    return samples;
}

// One sample per WENC call exposes its reconstruction after every sample.
void CheckDecoder(int kind) {
    const auto input = Signal(kind, 6000);
    WENCInfo info{};
    std::vector<std::uint8_t> stream(input.size() / 2, 0);
    std::vector<std::int16_t> expected(input.size());
    for (std::size_t i = 0; i < input.size(); ++i) {
        std::uint8_t code = 0;
        WENCGetEncodeData(&info, i ? WENC_FLAG_USER_INFO : 0, &input[i], 1, &code);
        stream[i / 2] |= std::uint8_t(i & 1 ? code >> 4 : code); // a lone sample sits in the high nibble
        expected[i] = static_cast<std::int16_t>(Field(info, 0));
    }
    // The game's 40-sample calls give the same bytes.
    WENCInfo framed{};
    std::vector<std::uint8_t> blocks(input.size() / 2);
    for (std::size_t f = 0; f < input.size() / 40; ++f)
        WENCGetEncodeData(&framed, f ? WENC_FLAG_USER_INFO : 0, &input[f * 40], 40, &blocks[f * 20]);
    Check(blocks == stream, "40-sample frames encode differently from single samples");

    mscharged::platform::WiimoteAdpcm state{};
    std::vector<std::int16_t> decoded(input.size());
    for (std::size_t f = 0; f < input.size() / 40; ++f)
        mscharged::platform::DecodeWiimoteAdpcm(state, &blocks[f * 20], 20, &decoded[f * 40]);
    std::size_t first = decoded.size();
    for (std::size_t i = 0; i < decoded.size() && first == decoded.size(); ++i)
        if (decoded[i] != expected[i]) first = i;
    if (first != decoded.size())
        std::fprintf(stderr, "signal %d: sample %zu decodes to %d, the encoder had %d\n", kind, first, decoded[first],
                     expected[first]);
    Check(first == decoded.size(), "the decoder left the encoder's reconstruction");
    Check(state.sample == Field(framed, 0) && state.step == Field(framed, 1), "the decoder's final state differs");

    if (kind == 1) { // a plain tone survives 4-bit ADPCM well
        double signal = 0, noise = 0;
        for (std::size_t i = 600; i < input.size(); ++i) {
            signal += double(input[i]) * input[i];
            noise += (double(decoded[i]) - input[i]) * (double(decoded[i]) - input[i]);
        }
        Check(10 * std::log10(signal / noise) > 20, "a 440 Hz tone decodes with too much error");
    }
}

// Amplitude of a tone at `hz` in 3000 Hz PCM bytes (after the filter's delay).
double Amplitude(const std::vector<std::int8_t>& pcm, double hz) {
    double re = 0, im = 0;
    std::size_t count = 0;
    for (std::size_t i = 300; i < pcm.size(); ++i, ++count) {
        re += pcm[i] * std::cos(2 * kPi * hz * double(i) / 3000);
        im += pcm[i] * std::sin(2 * kPi * hz * double(i) / 3000);
    }
    return 2 * std::hypot(re, im) / double(count);
}

std::vector<std::int8_t> Convert(const std::vector<std::int16_t>& input, std::size_t chunk) {
    mscharged::platform::WiimotePcmHalfRate converter;
    std::vector<std::int8_t> pcm(input.size() / 2);
    for (std::size_t at = 0; at < input.size(); at += chunk)
        converter.Convert(&input[at], std::min(chunk, input.size() - at), &pcm[at / 2]);
    return pcm;
}

std::vector<std::int16_t> Tone(double hz, double amplitude, std::size_t count) {
    std::vector<std::int16_t> samples(count);
    for (std::size_t i = 0; i < count; ++i)
        samples[i] = static_cast<std::int16_t>(std::lround(amplitude * std::sin(2 * kPi * hz * double(i) / 6000)));
    return samples;
}
} // namespace

int main() {
    for (int kind = 0; kind < 6; ++kind) CheckDecoder(kind);

    // 500 Hz passes at its level (16000 / 256 = 62.5 in 8 bits).
    const auto low = Amplitude(Convert(Tone(500, 16000, 6000), 40), 500);
    Check(std::fabs(20 * std::log10(low / 62.5)) < 0.5, "a 500 Hz tone changed its level");
    // 2400 Hz would fold back to 600 Hz at 3000 Hz: it must be gone.
    const auto folded = Convert(Tone(2400, 16000, 6000), 40);
    Check(Amplitude(folded, 600) < 0.5, "a 2400 Hz tone folds back into the PCM");
    // Frame size does not matter: the filter keeps its history between calls.
    const auto sweep = Signal(2, 6000);
    Check(Convert(sweep, 40) == Convert(sweep, 6000) && Convert(sweep, 40) == Convert(sweep, 2),
          "the PCM depends on how the samples are split");
    // DC and the 8-bit limits.
    const auto level = [](std::int16_t value) {
        return Convert(std::vector<std::int16_t>(400, value), 40).back();
    };
    Check(level(10000) == 39 && level(-10000) == -39 && level(0) == 0, "constant levels in 8 bits");
    Check(level(32767) == 127 && level(-32768) == -128, "full scale in 8 bits");

    std::printf("Wii Remote speaker decoding and PCM: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
