#include "platform/wiimote_calibration.h"

#include <cmath>
#include <iomanip>
#include <locale>
#include <sstream>

namespace mscharged::platform {
const std::uint8_t kWiimoteIrBlock1[5][9] = {
    {0x02, 0x00, 0x00, 0x71, 0x01, 0x00, 0x64, 0x00, 0xfe},
    {0x02, 0x00, 0x00, 0x71, 0x01, 0x00, 0x96, 0x00, 0xb4},
    {0x02, 0x00, 0x00, 0x71, 0x01, 0x00, 0xaa, 0x00, 0x64},
    {0x02, 0x00, 0x00, 0x71, 0x01, 0x00, 0xc8, 0x00, 0x36},
    {0x07, 0x00, 0x00, 0x71, 0x01, 0x00, 0x72, 0x00, 0x20}};
const std::uint8_t kWiimoteIrBlock2[5][2] = {{0xfd, 0x05}, {0xb3, 0x04}, {0x63, 0x03}, {0x35, 0x03}, {0x1f, 0x03}};

NativeDpdObservation DecodeWiimoteBasicIr(const std::uint8_t* ir) {
    // Two 5-byte groups of two objects with 10-bit coordinates, converted like
    // original WPADHIDParser (WPAD_DPD_BASIC): x as sent, y = 767 - sent y;
    // x 1023 or y 767 means no object (0, 767, size 0), a present one has size 12.
    NativeDpdObservation dots{};
    for (int group = 0; group < 2; ++group) {
        const std::uint8_t* bytes = ir + group * 5;
        const int x[2]{bytes[0] | ((bytes[2] >> 4) & 3) << 8, bytes[3] | (bytes[2] & 3) << 8};
        const int y[2]{bytes[1] | ((bytes[2] >> 6) & 3) << 8, bytes[4] | ((bytes[2] >> 2) & 3) << 8};
        for (int n = 0; n < 2; ++n) {
            auto& object = dots[group * 2 + n];
            object.trace_id = static_cast<std::uint8_t>(group * 2 + n);
            const int flipped = 767 - y[n];
            const bool absent = x[n] == 1023 || flipped == 767;
            object.x = static_cast<std::int16_t>(absent ? 0 : x[n]);
            object.y = static_cast<std::int16_t>(absent ? 767 : flipped);
            object.size = absent ? 0 : 12;
        }
    }
    return dots;
}

std::array<double, 2> KpadPointerCentre(double x, double y, std::uint8_t sensor_bar_position) {
    // KPAD get_kobj: raw*2/1024 - (resolution-1)/1024. KPADCalibrateDPD
    // requests height +/-0.2; KPADSetSensorHeight stores the negative height.
    // calc_dpd2pos_scale gives sqrt(1^2 + .75^2)/(.75 - .2).
    constexpr double camera_scale = 1.25 / 0.55;
    const double centre_y = sensor_bar_position == 1 ? -0.2 : 0.2;
    return {(1023.0 - 1024.0 * (2.0 * x - 1.0) / camera_scale) * 0.5,
            (767.0 + 1024.0 * (centre_y - (2.0 * y - 1.0) / camera_scale)) * 0.5};
}

namespace {
double Determinant(const double m[3][3]) {
    return m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) - m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]) +
           m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
}

// Cramer's rule for m * a = v.
std::array<double, 3> Solve(const double m[3][3], const double v[3], double det) {
    std::array<double, 3> result{};
    for (int column = 0; column < 3; ++column) {
        double replaced[3][3];
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j) replaced[i][j] = j == column ? v[i] : m[i][j];
        result[column] = Determinant(replaced) / det;
    }
    return result;
}

bool Usable(const WiimoteCalibration& calibration) {
    for (const double value : calibration.affine)
        if (!std::isfinite(value)) return false;
    const auto& a = calibration.affine;
    return std::fabs(a[0] * a[4] - a[1] * a[3]) > 1e-12;
}
} // namespace

std::optional<WiimoteCalibration> FitWiimoteCalibration(std::span<const WiimoteCalibrationSample> samples) {
    if (samples.size() < 3) return std::nullopt;
    double cx = 0, cy = 0, min_x = 1e9, max_x = -1e9, min_y = 1e9, max_y = -1e9;
    for (const auto& sample : samples) {
        cx += sample.camera_x;
        cy += sample.camera_y;
        min_x = std::fmin(min_x, sample.camera_x);
        max_x = std::fmax(max_x, sample.camera_x);
        min_y = std::fmin(min_y, sample.camera_y);
        max_y = std::fmax(max_y, sample.camera_y);
    }
    // The targets must have been aimed at different places.
    if (max_x - min_x < 16 || max_y - min_y < 16) return std::nullopt;
    cx /= double(samples.size());
    cy /= double(samples.size());
    // Normal equations of [mx - cx, my - cy, 1] -> screen x and y.
    double m[3][3]{}, vx[3]{}, vy[3]{};
    for (const auto& sample : samples) {
        const double row[3]{sample.camera_x - cx, sample.camera_y - cy, 1.0};
        for (int i = 0; i < 3; ++i) {
            for (int j = 0; j < 3; ++j) m[i][j] += row[i] * row[j];
            vx[i] += row[i] * sample.screen_x;
            vy[i] += row[i] * sample.screen_y;
        }
    }
    const double det = Determinant(m);
    if (!(std::fabs(det) > 1e-9)) return std::nullopt;
    const auto ax = Solve(m, vx, det), ay = Solve(m, vy, det);
    WiimoteCalibration result;
    result.affine = {ax[0], ax[1], ax[2] - ax[0] * cx - ax[1] * cy,
                     ay[0], ay[1], ay[2] - ay[0] * cx - ay[1] * cy};
    if (!Usable(result)) return std::nullopt;
    // Aims that do not line up (a slip on one target) are not a calibration.
    double squared = 0;
    for (const auto& sample : samples) {
        const auto& a = result.affine;
        const double x = a[0] * sample.camera_x + a[1] * sample.camera_y + a[2] - sample.screen_x;
        const double y = a[3] * sample.camera_x + a[4] * sample.camera_y + a[5] - sample.screen_y;
        squared += x * x + y * y;
    }
    if (std::sqrt(squared / double(samples.size())) > 0.15) return std::nullopt;
    return result;
}

// Streams in the classic locale, so a saved calibration reads back the same
// on every system and language setting.
std::string FormatWiimoteCalibration(const WiimoteCalibration& calibration) {
    std::ostringstream text;
    text.imbue(std::locale::classic());
    text << std::setprecision(10);
    for (std::size_t n = 0; n < calibration.affine.size(); ++n) text << (n ? " " : "") << calibration.affine[n];
    return text.str();
}

std::optional<WiimoteCalibration> ParseWiimoteCalibration(const std::string& text) {
    std::istringstream input(text);
    input.imbue(std::locale::classic());
    WiimoteCalibration calibration;
    for (auto& value : calibration.affine)
        if (!(input >> value)) return std::nullopt;
    input >> std::ws;
    if (!input.eof() || !Usable(calibration)) return std::nullopt;
    return calibration;
}

std::optional<std::array<double, 2>> TrackWiimotePair(WiimotePairTracker& tracker,
                                                       const NativeDpdObservation& dots) {
    std::array<std::array<double, 2>, 4> seen{};
    int count = 0;
    for (const auto& object : dots)
        if (object.size) seen[count++] = {double(object.x), double(object.y)};
    if (count >= 2) {
        int a = 0, b = 1;
        if (count > 2 && tracker.valid) {
            // More objects than the bar's two (a reflection): take the pair
            // most like the last one in length and place.
            double best = 1e300;
            for (int i = 0; i < count; ++i)
                for (int j = i + 1; j < count; ++j) {
                    const double length = std::hypot(seen[j][0] - seen[i][0], seen[j][1] - seen[i][1]) / 2;
                    const double mx = (seen[i][0] + seen[j][0]) / 2, my = (seen[i][1] + seen[j][1]) / 2;
                    const double cost = std::fabs(length - std::hypot(tracker.hx, tracker.hy)) +
                                        std::hypot(mx - tracker.mx, my - tracker.my);
                    if (cost < best) { best = cost; a = i; b = j; }
                }
        }
        double hx = (seen[b][0] - seen[a][0]) / 2, hy = (seen[b][1] - seen[a][1]) / 2;
        // Keep the half vector pointing the same way from frame to frame.
        if (tracker.valid ? hx * tracker.hx + hy * tracker.hy < 0 : hx < 0) { hx = -hx; hy = -hy; }
        tracker = {true, (seen[a][0] + seen[b][0]) / 2, (seen[a][1] + seen[b][1]) / 2, hx, hy};
        return std::array<double, 2>{tracker.mx, tracker.my};
    }
    if (count == 1 && tracker.valid) {
        // One end of the last pair: the end it is closer to.
        const double x = seen[0][0], y = seen[0][1];
        const double to_first = std::hypot(x - (tracker.mx - tracker.hx), y - (tracker.my - tracker.hy));
        const double to_second = std::hypot(x - (tracker.mx + tracker.hx), y - (tracker.my + tracker.hy));
        const double sign = to_first <= to_second ? 1.0 : -1.0;
        tracker.mx = x + sign * tracker.hx;
        tracker.my = y + sign * tracker.hy;
        return std::array<double, 2>{tracker.mx, tracker.my};
    }
    tracker.valid = false;
    return std::nullopt;
}

NativeDpdObservation ApplyWiimoteCalibration(const WiimoteCalibration& calibration, WiimotePairTracker& tracker,
                                             const NativeDpdObservation& dots, std::uint8_t sensor_bar_position) {
    NativeDpdObservation result = dots;
    const auto absent = [](NativeDpdObject& object) {
        object.x = 0;
        object.y = 767;
        object.size = 0;
    };
    const auto midpoint = TrackWiimotePair(tracker, dots);
    if (!midpoint) {
        for (auto& object : result) absent(object);
        return result;
    }
    const auto& a = calibration.affine;
    const double mx = (*midpoint)[0], my = (*midpoint)[1];
    const auto centre = KpadPointerCentre(a[0] * mx + a[1] * my + a[2], a[3] * mx + a[4] * my + a[5],
                                          sensor_bar_position);
    for (auto& object : result) {
        if (!object.size) continue;
        const double x = std::round(object.x + centre[0] - mx), y = std::round(object.y + centre[1] - my);
        if (!(x >= 0 && x < 1023 && y >= 0 && y < 767)) { absent(object); continue; }
        object.x = static_cast<std::int16_t>(x);
        object.y = static_cast<std::int16_t>(y);
    }
    return result;
}
} // namespace mscharged::platform
