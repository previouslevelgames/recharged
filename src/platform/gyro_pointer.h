#pragma once

#include <array>
#include <cstdint>

namespace mscharged::platform {
// Pointing with a gamepad's gyro (issue #14), the way a Wii Remote points:
// turning the controller left or right moves the pointer sideways, tilting its
// front up or down moves it up or down. Readings come in SDL's units and axes:
// rad/s and m/s^2, x to the right, y up, z towards the player.
//
// Sideways turns are measured about the room's vertical, found from the
// accelerometer, so the pointer moves the same with the controller held flat
// or tilted. While the controller is still, the gyro's own offset is measured
// and taken off, so a pointer left alone does not creep.

// At speed 100 % a turn of this many degrees crosses the picture's width;
// up and down take the same angle for the same distance.
inline constexpr float kGyroDegreesPerPicture = 25.0f;

struct GyroMotion {
    std::array<float, 3> bias{};    // the gyro's offset at rest, rad/s
    std::array<float, 3> gravity{}; // smoothed accelerometer, pointing up, m/s^2
    bool accel_known = false, accel_calm = true;
    float still_seconds = 0;
    std::uint64_t gyro_ns = 0, accel_ns = 0; // timestamps of the last readings
    // Turned in the readings since the last TakeGyroTurn, in radians (yaw +
    // to the left, pitch + upwards), and the time those readings cover.
    float yaw = 0, pitch = 0, seconds = 0;
    // The turning speed shown between readings (rad/s), how long it has
    // been held without a new reading, and the turn not shown yet.
    float yaw_rate = 0, pitch_rate = 0, held = 0;
    float yaw_owed = 0, pitch_owed = 0;
};
// One reading as SDL reports it, with its sensor timestamp in nanoseconds.
// Readings further apart than 0.1 s (a pause, a reconnect) add no turn.
void AddGyroReading(GyroMotion& motion, const float rate[3], std::uint64_t timestamp_ns);
void AddAccelReading(GyroMotion& motion, const float accel[3], std::uint64_t timestamp_ns);

struct GyroTurn {
    float yaw = 0, pitch = 0;
};
// The turn to show for the `seconds` since the last call. Readings arrive in
// bunches, so the pointer moves at the latest turning speed instead, evenly,
// like a Wii Remote reporting at a steady rate; what that leaves out or adds
// is made up over the next tenth of a second, so the turns still add up.
GyroTurn TakeGyroTurn(GyroMotion& motion, float seconds);
// Forgets the turn not shown yet, when the pointer goes back to the middle.
void ClearGyroTurn(GyroMotion& motion);
// Moves a pointer (0..1 across the picture, y down) by `turn` at `speed`
// percent (kMinGyroSpeed-kMaxGyroSpeed); `aspect` is the picture's height over
// its width. The pointer stops at the picture's edges.
void MoveGyroPointer(float& x, float& y, GyroTurn turn, int speed, float aspect);
} // namespace mscharged::platform
