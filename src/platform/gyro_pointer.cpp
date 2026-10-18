#include "platform/gyro_pointer.h"

#include "bootstrap/gamepad_actions.h"

#include <algorithm>
#include <cmath>

namespace mscharged::platform {
namespace {
constexpr float kDegree = 3.14159265358979f / 180.0f;
constexpr double kMaxGap = 0.1;
// Still: turning slower than 2 degrees a second with a steady accelerometer,
// for a second. The offset then follows the readings with a 2 s time
// constant; real offsets are well below 5 degrees a second.
constexpr float kStillRate = 2.0f * kDegree;
constexpr float kStillAccel = 0.3f;
constexpr float kStillSeconds = 1.0f;
constexpr float kBiasSeconds = 2.0f;
constexpr float kMaxBias = 5.0f * kDegree;
constexpr float kGravitySeconds = 0.25f;
// Below this the turn fades towards zero: the sensor's noise at rest.
constexpr float kTightenRate = 0.5f * kDegree;
// Player-space yaw (as described on GyroWiki): a turn about the vertical,
// allowed up to 1.41 times the controller's own yaw and roll.
constexpr float kYawRelax = 1.41f;
// Without a reading for this long the pointer stops instead of holding the
// last turning speed; the turn not shown yet is made up over kCatchUpSeconds.
constexpr float kHoldSeconds = 0.05f;
constexpr float kCatchUpSeconds = 0.1f;

double Seconds(std::uint64_t& last, std::uint64_t now) {
    const double seconds = last && now > last ? double(now - last) * 1e-9 : 0.0;
    last = now;
    return seconds > kMaxGap ? 0.0 : seconds;
}

bool Finite(const float values[3]) {
    return std::isfinite(values[0]) && std::isfinite(values[1]) && std::isfinite(values[2]);
}
} // namespace

void AddAccelReading(GyroMotion& motion, const float accel[3], std::uint64_t timestamp_ns) {
    if (!Finite(accel)) return;
    const double seconds = Seconds(motion.accel_ns, timestamp_ns);
    if (!motion.accel_known) {
        motion.gravity = {accel[0], accel[1], accel[2]};
        motion.accel_known = true;
        motion.accel_calm = true;
        return;
    }
    const float follow = float(std::min(1.0, seconds / kGravitySeconds));
    float deviation = 0;
    for (int n = 0; n < 3; ++n) {
        const float difference = accel[n] - motion.gravity[n];
        deviation += difference * difference;
        motion.gravity[n] += difference * follow;
    }
    motion.accel_calm = std::sqrt(deviation) < kStillAccel;
}

void AddGyroReading(GyroMotion& motion, const float rate[3], std::uint64_t timestamp_ns) {
    if (!Finite(rate)) return;
    const float seconds = float(Seconds(motion.gyro_ns, timestamp_ns));
    std::array<float, 3> turn{rate[0] - motion.bias[0], rate[1] - motion.bias[1], rate[2] - motion.bias[2]};
    const float speed = std::sqrt(turn[0] * turn[0] + turn[1] * turn[1] + turn[2] * turn[2]);
    motion.still_seconds = speed < kStillRate && motion.accel_calm ? motion.still_seconds + seconds : 0.0f;
    if (motion.still_seconds >= kStillSeconds) {
        const float follow = std::min(1.0f, seconds / kBiasSeconds);
        for (int n = 0; n < 3; ++n) {
            motion.bias[n] = std::clamp(motion.bias[n] + (rate[n] - motion.bias[n]) * follow, -kMaxBias, kMaxBias);
            turn[n] = rate[n] - motion.bias[n];
        }
    }
    // The pitch axis (x) stays out of the sideways turn, so tilting the
    // controller up never moves the pointer sideways.
    float yaw = turn[1];
    const float pitch = turn[0];
    const float g = std::sqrt(motion.gravity[0] * motion.gravity[0] + motion.gravity[1] * motion.gravity[1]
                              + motion.gravity[2] * motion.gravity[2]);
    if (motion.accel_known && g > 1.0f) {
        const float vertical = (turn[1] * motion.gravity[1] + turn[2] * motion.gravity[2]) / g;
        yaw = std::copysign(std::min(std::fabs(vertical) * kYawRelax, std::hypot(turn[1], turn[2])), vertical);
    }
    float scale = 1.0f;
    if (const float moving = std::hypot(yaw, pitch); moving < kTightenRate) scale = moving / kTightenRate;
    motion.yaw += yaw * scale * seconds;
    motion.pitch += pitch * scale * seconds;
    motion.seconds += seconds;
}

GyroTurn TakeGyroTurn(GyroMotion& motion, float seconds) {
    seconds = std::isfinite(seconds) ? std::clamp(seconds, 0.0f, float(kMaxGap)) : 0.0f;
    if (motion.seconds > 0) {
        motion.yaw_rate = motion.yaw / motion.seconds;
        motion.pitch_rate = motion.pitch / motion.seconds;
        motion.held = 0;
    } else if ((motion.held += seconds) > kHoldSeconds) motion.yaw_rate = motion.pitch_rate = 0;
    motion.yaw_owed += motion.yaw;
    motion.pitch_owed += motion.pitch;
    motion.yaw = motion.pitch = motion.seconds = 0;
    GyroTurn turn{motion.yaw_rate * seconds, motion.pitch_rate * seconds};
    const float catch_up = std::min(1.0f, seconds / kCatchUpSeconds);
    turn.yaw += (motion.yaw_owed - turn.yaw) * catch_up;
    turn.pitch += (motion.pitch_owed - turn.pitch) * catch_up;
    motion.yaw_owed -= turn.yaw;
    motion.pitch_owed -= turn.pitch;
    return turn;
}

void ClearGyroTurn(GyroMotion& motion) {
    motion.yaw = motion.pitch = motion.seconds = 0;
    motion.yaw_owed = motion.pitch_owed = 0;
}

void MoveGyroPointer(float& x, float& y, GyroTurn turn, int speed, float aspect) {
    const float width = kGyroDegreesPerPicture * kDegree * 100.0f
        / float(std::clamp(speed, kMinGyroSpeed, kMaxGyroSpeed));
    const float height = width * (std::isfinite(aspect) && aspect > 0 ? aspect : 0.75f);
    if (std::isfinite(turn.yaw)) x = std::clamp(x - turn.yaw / width, 0.0f, 1.0f);
    if (std::isfinite(turn.pitch)) y = std::clamp(y - turn.pitch / height, 0.0f, 1.0f);
}
} // namespace mscharged::platform
