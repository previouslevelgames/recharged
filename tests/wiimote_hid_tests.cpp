// Native Wii Remote IR decoding against original WPADHIDParser's basic DPD
// conversion: x as sent, y flipped (767 - y), absent objects (0, 767, size 0).
#include "platform/wiimote_hid.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <utility>
#include <vector>

namespace {
unsigned checks{}, failures{};
void Check(bool value, const char* message) {
    ++checks;
    if (!value) { ++failures; std::fprintf(stderr, "Wii Remote IR decoding: %s\n", message); }
}
} // namespace

int main() {
    using mscharged::platform::DecodeWiimoteBasicIr;
    // Bytes from a real remote behind a DolphinBar: two objects in the first
    // group (with high coordinate bits), none in the second.
    const std::uint8_t pair[10]{0x9d, 0x1d, 0x77, 0xc9, 0x47, 0xff, 0xff, 0xff, 0xff, 0xff};
    const auto dots = DecodeWiimoteBasicIr(pair);
    Check(dots[0].x == 925 && dots[0].y == 767 - 285 && dots[0].size == 12 && dots[0].trace_id == 0,
          "first object lost its position, flip or size");
    Check(dots[1].x == 969 && dots[1].y == 767 - 327 && dots[1].size == 12 && dots[1].trace_id == 1,
          "second object lost its position, flip or size");
    for (int n = 2; n < 4; ++n)
        Check(dots[n].x == 0 && dots[n].y == 767 && dots[n].size == 0 && dots[n].trace_id == n,
              "an empty slot is not absent in the original form");

    // Original WPAD also drops an object on the top camera row (sent y 0)
    // and keeps its neighbour.
    const std::uint8_t edge[10]{0x10, 0x00, 0x00, 0x20, 0x05, 0xff, 0xff, 0xff, 0xff, 0xff};
    const auto top = DecodeWiimoteBasicIr(edge);
    Check(top[0].x == 0 && top[0].y == 767 && top[0].size == 0, "an object on the top row was kept");
    Check(top[1].x == 0x20 && top[1].y == 762 && top[1].size == 12, "the neighbour of a dropped object was lost");

    // Second group's high bits use the same layout.
    const std::uint8_t second[10]{0xff, 0xff, 0xff, 0xff, 0xff, 0x01, 0x02, 0xe4, 0x03, 0x04};
    const auto group = DecodeWiimoteBasicIr(second);
    Check(group[0].size == 0 && group[1].size == 0, "an empty first group produced objects");
    Check(group[2].x == (0x01 | 2 << 8) && group[2].y == 767 - (0x02 | 3 << 8), "third object bits");
    Check(group[3].x == (0x03 | 0 << 8) && group[3].y == 767 - (0x04 | 1 << 8), "fourth object bits");

    // Pointer calibration. KPAD's pointer centre for the screen centre.
    using namespace mscharged::platform;
    const auto near = [](double a, double b, double tolerance = 1e-6) { return std::fabs(a - b) <= tolerance; };
    const auto below = KpadPointerCentre(0.5, 0.5, 0), above = KpadPointerCentre(0.5, 0.5, 1);
    Check(near(below[0], 511.5) && near(below[1], (767.0 + 1024.0 * 0.2) / 2), "pointer centre, bar below");
    Check(near(above[1], (767.0 - 1024.0 * 0.2) / 2), "pointer centre, bar above");

    // A fit recovers the mapping from five aimed targets.
    const auto camera_of = [](double x, double y) {
        // Aiming right moves the bar left in the camera, aiming down moves it up.
        return std::array<double, 2>{700.0 - 420.0 * x + 15.0 * y, 600.0 - 380.0 * y};
    };
    std::vector<WiimoteCalibrationSample> samples;
    for (const auto& [x, y] : {std::pair{0.5, 0.5}, {0.1, 0.1}, {0.9, 0.1}, {0.9, 0.9}, {0.1, 0.9}}) {
        const auto camera = camera_of(x, y);
        samples.push_back({camera[0], camera[1], x, y});
    }
    const auto fit = FitWiimoteCalibration(samples);
    Check(fit.has_value(), "five consistent targets did not fit");
    if (fit) {
        const auto camera = camera_of(0.3, 0.7);
        const auto& a = fit->affine;
        Check(near(a[0] * camera[0] + a[1] * camera[1] + a[2], 0.3) &&
              near(a[3] * camera[0] + a[4] * camera[1] + a[5], 0.7), "fit does not map an untrained aim");
        const auto text = FormatWiimoteCalibration(*fit);
        const auto parsed = ParseWiimoteCalibration(text);
        Check(parsed.has_value(), "saved calibration did not read back");
        if (parsed)
            for (int n = 0; n < 6; ++n)
                Check(near(parsed->affine[n], fit->affine[n], 1e-9 * (1 + std::fabs(fit->affine[n]))),
                      "saved calibration lost precision");

        // Shift: the pair keeps its shape; its midpoint lands where KPAD reads the aim.
        NativeDpdObservation dots{};
        const auto aim = camera_of(0.3, 0.7);
        dots[0] = {std::int16_t(std::lround(aim[0] - 150)), std::int16_t(std::lround(aim[1])), 12, 0};
        dots[1] = {std::int16_t(std::lround(aim[0] + 150)), std::int16_t(std::lround(aim[1])), 12, 1};
        WiimotePairTracker tracker;
        const auto moved = ApplyWiimoteCalibration(*fit, tracker, dots, 1);
        const auto expected = KpadPointerCentre(0.3, 0.7, 1);
        Check(moved[0].size == 12 && moved[1].size == 12, "calibration dropped a visible pair");
        Check(near((moved[0].x + moved[1].x) / 2.0, expected[0], 1.0) &&
              near((moved[0].y + moved[1].y) / 2.0, expected[1], 1.0), "pair midpoint is not at the aimed pointer");
        Check(moved[1].x - moved[0].x == dots[1].x - dots[0].x && moved[1].y - moved[0].y == dots[1].y - dots[0].y,
              "calibration changed the pair's shape");
        Check(moved[2].size == 0 && moved[3].size == 0, "calibration made absent objects visible");
        // One end only: the tracker keeps the pair's midpoint.
        NativeDpdObservation one{};
        one[1] = dots[1];
        const auto single = ApplyWiimoteCalibration(*fit, tracker, one, 1);
        Check(single[1].size == 12 && single[1].x == moved[1].x && single[1].y == moved[1].y,
              "one visible end did not follow its pair");
        // Nothing seen: tracking stops; a lone object without a pair is absent.
        ApplyWiimoteCalibration(*fit, tracker, NativeDpdObservation{}, 1);
        const auto lone = ApplyWiimoteCalibration(*fit, tracker, one, 1);
        Check(!tracker.valid && lone[1].size == 0, "a lone object without a pair was kept");
    }
    // Aims at one spot, or that do not line up, are not a calibration.
    std::vector<WiimoteCalibrationSample> same(5, WiimoteCalibrationSample{500, 400, 0.5, 0.5});
    for (int n = 0; n < 5; ++n) same[n].screen_x = 0.1 + 0.2 * n;
    Check(!FitWiimoteCalibration(same), "aims at one spot were accepted");
    auto slipped = samples;
    slipped[3].camera_x += 300;
    Check(!FitWiimoteCalibration(slipped), "a slipped aim was accepted");
    Check(!ParseWiimoteCalibration("none") && !ParseWiimoteCalibration("1 2 3 4 5") &&
          !ParseWiimoteCalibration("1 0 0 0 1 0 7") && !ParseWiimoteCalibration("0 0 0 0 0 0"),
          "a malformed calibration was read");

    std::printf("Wii Remote IR decoding and calibration: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
