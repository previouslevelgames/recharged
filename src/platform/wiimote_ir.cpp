#include "platform/wiimote_ir.h"

#include <cmath>
#include <limits>
#include <utility>

namespace mscharged::platform {
namespace {
// KPAD's pointer centre (its height) and the scale from its object
// coordinates to the pointer for the sensor-bar position. KPADCalibrateDPD
// requests height +/-0.2; KPADSetSensorHeight stores the negative height.
// Without it the centre stays 0. calc_dpd2pos_scale gives
// sqrt(1^2 + .75^2)/min(1, .75 - |centre|): 1.25/.55, or 1.25/.75.
struct KpadCentre {
    double y, scale;
};
KpadCentre Centre(std::uint8_t sensor_bar_position, bool sensor_height) {
    const double y = !sensor_height ? 0.0 : sensor_bar_position == 1 ? -0.2 : 0.2;
    return {y, 1.25 / (0.75 - std::fabs(y))};
}
// The sensor bar's dots as far apart as KPAD accepts them, in camera pixels:
// its unit distance .2/.383864 at 2/1024 a pixel, from kp_err_dist_max (3, a
// bar about 3 m away) to kp_err_dist_min (the unit distance, about .5 m).
constexpr double kUnitDistance = 0.2 / 0.383864;
constexpr double kFarthestSpan = 512.0 * kUnitDistance / 3.0, kNearestSpan = 512.0;
} // namespace

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
    // x 1023 or y 767 means no object (0, 767, size 0), a present one has size 12;
    // a sent y past the sensor's 768 rows (a noisy report) is no object either.
    NativeDpdObservation dots{};
    for (int group = 0; group < 2; ++group) {
        const std::uint8_t* bytes = ir + group * 5;
        const int x[2]{bytes[0] | ((bytes[2] >> 4) & 3) << 8, bytes[3] | (bytes[2] & 3) << 8};
        const int y[2]{bytes[1] | ((bytes[2] >> 6) & 3) << 8, bytes[4] | ((bytes[2] >> 2) & 3) << 8};
        for (int n = 0; n < 2; ++n) {
            auto& object = dots[group * 2 + n];
            object.trace_id = static_cast<std::uint8_t>(group * 2 + n);
            const int flipped = 767 - y[n];
            const bool absent = x[n] == 1023 || flipped == 767 || flipped < 0;
            object.x = static_cast<std::int16_t>(absent ? 0 : x[n]);
            object.y = static_cast<std::int16_t>(absent ? 767 : flipped);
            object.size = absent ? 0 : 12;
        }
    }
    return dots;
}

std::array<double, 2> KpadPointerCentre(double x, double y, std::uint8_t sensor_bar_position,
                                        bool sensor_height) {
    // KPAD get_kobj: raw*2/1024 - (resolution-1)/1024.
    const auto centre = Centre(sensor_bar_position, sensor_height);
    return {(1023.0 - 1024.0 * (2.0 * x - 1.0) / centre.scale) * 0.5,
            (767.0 + 1024.0 * (centre.y - (2.0 * y - 1.0) / centre.scale)) * 0.5};
}

std::optional<std::array<double, 2>> CameraPointer::Update(const NativeDpdObservation& objects,
                                                           std::uint8_t sensor_bar_position, bool sensor_height) {
    using Point = std::array<double, 2>;
    const auto apart = [](const Point& a, const Point& b) { return std::hypot(b[0] - a[0], b[1] - a[1]); };
    Point seen[4]{};
    int count = 0;
    for (const auto& object : objects)
        if (object.size) seen[count++] = {double(object.x), double(object.y)};
    // The pair: the one nearest the last pair, else the most level one.
    std::pair<Point, Point> next{};
    double best = std::numeric_limits<double>::infinity();
    for (int i = 0; i < count; ++i)
        for (int j = i + 1; j < count; ++j) {
            Point left = seen[i], right = seen[j];
            if (left[0] > right[0]) std::swap(left, right);
            const double span = apart(left, right);
            if (span < kFarthestSpan || span > kNearestSpan) continue;
            const double cost = pair_ ? apart(left, left_) + apart(right, right_)
                                      : std::fabs(std::atan2(right[1] - left[1], right[0] - left[0]));
            if (cost < best) best = cost, next = {left, right};
        }
    // None: one dot (the other past the camera's edge) moves the last pair
    // with it, as the end it is nearest to, if it moved less than half the
    // pair's span since.
    if (best == std::numeric_limits<double>::infinity() && pair_) {
        const Point span{right_[0] - left_[0], right_[1] - left_[1]};
        for (int n = 0; n < count; ++n) {
            const double toLeft = apart(seen[n], left_), toRight = apart(seen[n], right_);
            if (std::min(toLeft, toRight) >= std::min(best, 0.5 * std::hypot(span[0], span[1]))) continue;
            best = std::min(toLeft, toRight);
            const Point& dot = seen[n];
            next = toLeft <= toRight ? std::pair<Point, Point>{dot, {dot[0] + span[0], dot[1] + span[1]}}
                                     : std::pair<Point, Point>{{dot[0] - span[0], dot[1] - span[1]}, dot};
        }
    }
    if (best == std::numeric_limits<double>::infinity()) {
        Reset();
        return std::nullopt;
    }
    pair_ = true;
    left_ = next.first;
    right_ = next.second;
    // The midpoint in KPAD's object coordinates (get_kobj), turned back by
    // the pair's angle (calc_dpd_variable, its horizon level: (1, 0)), then
    // KpadPointerCentre's mapping undone.
    const double length = apart(left_, right_);
    const double nx = (right_[0] - left_[0]) / length, ny = (right_[1] - left_[1]) / length;
    const double cx = (left_[0] + right_[0]) / 1024.0 - 1023.0 / 1024.0;
    const double cy = (left_[1] + right_[1]) / 1024.0 - 767.0 / 1024.0;
    const double vx = nx * cx + ny * cy, vy = nx * cy - ny * cx;
    const auto centre = Centre(sensor_bar_position, sensor_height);
    const Point target{-vx * centre.scale, (centre.y - vy) * centre.scale};
    // Smoothed like the game's KPADSetPosParam(0.02, 0.95): a move shorter
    // than 0.02 counts by the fourth power of its share of it (the hand's
    // tremor stays out), 95 % of it is taken.
    if (!smoothed_) pos_ = target;
    else {
        const double dx = target[0] - pos_[0], dy = target[1] - pos_[1];
        const double share = std::min(1.0, std::hypot(dx, dy) / 0.02);
        const double weight = share * share * share * share * 0.95;
        pos_ = {pos_[0] + weight * dx, pos_[1] + weight * dy};
    }
    smoothed_ = true;
    return Point{(pos_[0] + 1.0) * 0.5, (pos_[1] + 1.0) * 0.5};
}

void CameraPointer::Reset() {
    pair_ = smoothed_ = false;
}

} // namespace mscharged::platform
