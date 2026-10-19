#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace mscharged::platform {
// The game's Wii Remote speaker sound as original WENC encodes it: 4-bit
// Yamaha ADPCM at 6000 Hz, two samples a byte, high nibble first. Decoding
// repeats WENC's own reconstruction, so from the encoder's start (the first
// frame after the speaker is switched on) it follows the encoder exactly.
struct WiimoteAdpcm {
    std::int32_t sample = 0; // WENC nXN
    std::int32_t step = 127; // WENC nDL
};
// Writes 2 * bytes samples.
void DecodeWiimoteAdpcm(WiimoteAdpcm& state, const std::uint8_t* data, std::size_t bytes, std::int16_t* samples);

// The same sound as signed 8-bit PCM at 3000 Hz: a low-pass below the new
// Nyquist frequency, every second sample, rounded to 8 bits. PCM keeps no
// decoder state in the Remote, so a lost packet only skips a moment.
class WiimotePcmHalfRate {
public:
    static constexpr std::size_t kTaps = 47;
    // Takes an even number of samples and writes count / 2 bytes.
    void Convert(const std::int16_t* samples, std::size_t count, std::int8_t* pcm);
    void Reset() { history_ = {}; }

private:
    std::array<std::int16_t, kTaps - 1> history_{};
};
} // namespace mscharged::platform
