#include "platform/wiimote_speaker.h"

#include <algorithm>
#include <cmath>

namespace mscharged::platform {
namespace {
// Half-band low-pass (Blackman window, unity at DC): flat to about 1200 Hz,
// -6 dB at 1500 Hz and below -50 dB from 1800 Hz at 6000 Hz in.
const std::array<double, WiimotePcmHalfRate::kTaps>& HalfBand() {
    static const auto taps = [] {
        constexpr double pi = 3.14159265358979323846;
        constexpr int n = int(WiimotePcmHalfRate::kTaps);
        std::array<double, WiimotePcmHalfRate::kTaps> h{};
        double total = 0;
        for (int i = 0; i < n; ++i) {
            const double x = i - (n - 1) / 2.0;
            const double sinc = x == 0 ? 0.5 : std::sin(pi * x / 2) / (pi * x);
            const double window = 0.42 - 0.5 * std::cos(2 * pi * i / (n - 1)) + 0.08 * std::cos(4 * pi * i / (n - 1));
            h[i] = sinc * window;
            total += h[i];
        }
        for (auto& tap : h) tap /= total;
        return h;
    }();
    return taps;
}
} // namespace

void DecodeWiimoteAdpcm(WiimoteAdpcm& state, const std::uint8_t* data, std::size_t bytes, std::int16_t* samples) {
    // WENC's step factors: 0.8984375 ... 2.3984375 are n / 256.
    static constexpr std::int32_t kScale[8]{230, 230, 230, 230, 307, 409, 512, 614};
    for (std::size_t i = 0; i < bytes * 2; ++i) {
        const unsigned code = i & 1 ? data[i / 2] & 0xfu : data[i / 2] >> 4;
        const std::int32_t step = state.step, half = step / 2, quarter = half / 2;
        std::int32_t delta = quarter / 2;
        if (code & 4) delta += step;
        if (code & 2) delta += half;
        if (code & 1) delta += quarter;
        if (code & 8) delta = -delta;
        delta = std::clamp<std::int32_t>(delta, -0x10000, 0xffff);
        state.sample = std::clamp<std::int32_t>(state.sample + delta, -0x8000, 0x7fff);
        samples[i] = static_cast<std::int16_t>(state.sample);
        state.step = std::clamp<std::int32_t>(step * kScale[code & 7] >> 8, 127, 0x6000);
    }
}

void WiimotePcmHalfRate::Convert(const std::int16_t* samples, std::size_t count, std::int8_t* pcm) {
    const auto& h = HalfBand();
    for (std::size_t out = 0; out < count / 2; ++out) {
        // Input index of this output's newest sample: the second of its pair.
        const std::size_t newest = out * 2 + 1;
        double sum = 0;
        for (std::size_t k = 0; k < kTaps; ++k) {
            const std::ptrdiff_t at = std::ptrdiff_t(newest) - std::ptrdiff_t(k);
            sum += h[k] * (at >= 0 ? samples[at] : history_[history_.size() + at]);
        }
        pcm[out] = static_cast<std::int8_t>(std::clamp<long>(std::lround(sum / 256), -128, 127));
    }
    // Keep the newest kTaps - 1 inputs for the next call.
    const std::size_t keep = history_.size();
    if (count >= keep) {
        std::copy(samples + count - keep, samples + count, history_.begin());
    } else {
        std::move(history_.begin() + count, history_.end(), history_.begin());
        std::copy(samples, samples + count, history_.end() - count);
    }
}
} // namespace mscharged::platform
