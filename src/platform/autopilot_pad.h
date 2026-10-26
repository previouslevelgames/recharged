#pragma once

#include <cmath>
#include <cstdint>

// The test builds' autopilot (src/platform/autopilot.cpp) in the units of a
// Wii Remote with Nunchuk, apart from the game so that the conversions can be
// tested against the game's own reading of the pad.
namespace mscharged::platform::autopilot
{
// The game's angle of a direction on the field: a 16-bit turn, as nlATan2Angle.
inline std::uint16_t AngleOf(float dx, float dy)
{
    return static_cast<std::uint16_t>(static_cast<std::int32_t>(10430.378f * std::atan2(dy, dx)));
}

struct Stick
{
    std::int8_t x = 0;
    std::int8_t y = 0;
};

// The raw Nunchuk stick (WPADFSStatus fsStickX/Y) that runs the player toward
// field angle `direction`, `deflection` of the way out (0 to 1), as the camera
// turns the stick: the game drops 15 counts per axis and reads the rest over
// 56 (ClampWiiStick, WiiNormalizeClampedStick), then runs toward the input's
// remap angle plus the stick's own angle (cAIPad::GetMovementStickDirection),
// as fast as the stick is far out. The stick stays inside the gate of 56:
// the game would cut one beyond it down axis by axis, turning it off course.
inline Stick NunchukStick(std::uint16_t direction, float deflection, std::uint16_t remap)
{
    if (!(deflection > 0.0f)) return {};
    if (deflection > 1.0f) deflection = 1.0f;
    const float angle = static_cast<std::uint16_t>(direction - remap) * (6.28318531f / 65536.0f);
    int x = static_cast<int>(std::lround(std::cos(angle) * 56.0f * deflection));
    int y = static_cast<int>(std::lround(std::sin(angle) * 56.0f * deflection));
    while (x * x + y * y > 56 * 56)
    {
        int& larger = std::abs(x) >= std::abs(y) ? x : y;
        larger += larger > 0 ? -1 : 1;
    }
    const auto raw = [](int counts) {
        return static_cast<std::int8_t>(counts == 0 ? 0 : counts > 0 ? counts + 15 : counts - 15);
    };
    return {raw(x), raw(y)};
}

// A shake of the Remote, ShakeSteps fixed steps long: what step `step` adds to
// the Remote's x acceleration (WPAD counts), three steps one way and three
// back. The game reads a shake from the largest change within five steps of
// its blended history (cAIPad::DetectRightShake, more than 1.33 at 205 counts
// to 1), and the step's inputs carry at most 2.49 of it (PackDetInput).
inline constexpr int ShakeSteps = 6;
inline constexpr int ShakeCounts = 400;
inline int ShakeAccel(int step)
{
    if (step < 0 || step >= ShakeSteps) return 0;
    return step < ShakeSteps / 2 ? ShakeCounts : -ShakeCounts;
}
} // namespace mscharged::platform::autopilot
