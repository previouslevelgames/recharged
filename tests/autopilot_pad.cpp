// The autopilot's controls (src/platform/autopilot_pad.h) read back the way
// the game reads a Wii Remote with Nunchuk, rebuilt here on its own from the
// decomp: ClampWiiStick and the stick over 56 (WiiPad.cpp,
// WiiFreestylePad.cpp), the inputs' bytes (PackDetInput and UnpackDetInput),
// the stick's angle and length (nlCartesianToPolar) turned by the camera
// (cAIPad::GetMovementStickDirection and GetMovementStickMagnitude), and a
// Remote shake from the blended acceleration history (AIPadManager,
// cAIPad::DetectRightShake).
#include "platform/autopilot_pad.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <string>

using namespace mscharged::platform::autopilot;

namespace
{
unsigned checks = 0;
void Check(bool value, const std::string& message)
{
    ++checks;
    if (!value) throw std::runtime_error(message);
}

int Gap(std::uint16_t a, std::uint16_t b)
{
    return std::abs(static_cast<int>(static_cast<std::int16_t>(a - b)));
}

// ClampWiiStick: 15 counts dropped per axis, then at most 56 from the middle.
void Clamp(std::int8_t& x, std::int8_t& y)
{
    const auto dead = [](std::int8_t& v) {
        if (v < 15 && v > -15)
            v = 0;
        else
            v = static_cast<std::int8_t>(v > 0 ? v - 15 : v + 15);
    };
    dead(x);
    dead(y);
    const int squared = x * x + y * y;
    if (squared > 56 * 56)
    {
        const float scale = 56.0f / std::sqrt(static_cast<float>(squared));
        x = static_cast<std::int8_t>(x * scale);
        y = static_cast<std::int8_t>(y * scale);
    }
}

struct Movement
{
    std::uint16_t direction;
    float magnitude;
};
// The direction and speed a raw Nunchuk stick runs the player at, through a
// network input (a byte per axis) and the camera's remap angle.
Movement Read(Stick stick, std::uint16_t remap)
{
    std::int8_t x = stick.x, y = stick.y;
    Clamp(x, y);
    const auto byte = [](float axis) { return static_cast<std::int8_t>(axis * 127.0f) / 127.0f; };
    const float ax = byte(x / 56.0f), ay = byte(y / 56.0f);
    const float r = std::sqrt(ax * ax + ay * ay);
    const auto angle = static_cast<std::uint16_t>(static_cast<std::int32_t>(std::atan2(ay, ax) * 10430.378f));
    return {static_cast<std::uint16_t>(remap + angle), (r - 0.3f) / 0.7f};
}

void StickChecks()
{
    const std::array<std::uint16_t, 6> remaps{0, 0x1234, 0x4000, 0x8000, 0xC100, 0xFF00};
    for (const std::uint16_t remap : remaps)
        for (int direction = 0; direction < 0x10000; direction += 257)
        {
            const Stick stick = NunchukStick(static_cast<std::uint16_t>(direction), 1.0f, remap);
            Check(std::abs(stick.x) <= 71 && std::abs(stick.y) <= 71,
                  "A stick left the Nunchuk's range at " + std::to_string(direction));
            const Movement run = Read(stick, remap);
            // Within 1.5 degrees, and at full speed.
            Check(Gap(run.direction, static_cast<std::uint16_t>(direction)) <= 273,
                  "The player ran off course at " + std::to_string(direction) + " (remap " + std::to_string(remap)
                      + "): " + std::to_string(run.direction));
            Check(run.magnitude > 0.95f, "A full stick did not run at full speed at " + std::to_string(direction));
        }
    // Half out still turns the player the right way; nothing out is the middle.
    const Movement half = Read(NunchukStick(0x3000, 0.5f, 0x8000), 0x8000);
    Check(Gap(half.direction, 0x3000) <= 400 && half.magnitude > 0.0f && half.magnitude < 0.6f,
          "A half stick did not run slower the same way");
    const Stick none = NunchukStick(0x3000, 0.0f, 0x8000);
    Check(none.x == 0 && none.y == 0, "No deflection moved the stick");
    const Stick over = NunchukStick(0x3000, 4.0f, 0);
    const Stick full = NunchukStick(0x3000, 1.0f, 0);
    Check(over.x == full.x && over.y == full.y, "A deflection above 1 went further than full");
    // The field's angles, as the game's own nlATan2Angle.
    Check(AngleOf(1.0f, 0.0f) == 0 && AngleOf(0.0f, 1.0f) == 0x4000 && AngleOf(0.0f, -1.0f) == 0xC000,
          "Field angles are not the game's");
}

// One shake over a Remote at rest (x at `rest` counts): whether the game sees
// it, from the step's input bytes and the blended five-step history.
bool Shaken(int rest, bool shake)
{
    constexpr float kCountsToInput = 0.0048780488f; // DetInput::ReadFromPad
    const auto pack = [](float v) {
        const float scaled = v * 50.849613f;
        Check(scaled > -128.0f && scaled < 128.0f, "A shake left the input's byte");
        return static_cast<std::int8_t>(scaled) * 0.019665834f;
    };
    std::array<std::array<float, 3>, 30> history{};
    for (auto& sample : history) sample = {1000.0f, 0.0f, 0.0f};
    unsigned index = 0;
    bool seen = false;
    for (int step = 0; step < 40; ++step)
    {
        const int shakeStep = step - 10;
        const int x = std::clamp(rest + (shake ? ShakeAccel(shakeStep) : 0), -510, 510);
        const std::array<float, 3> input{pack(x * kCountsToInput), 0.0f, pack(100 * kCountsToInput)};
        const unsigned previous = index;
        index = (index + 1) % 30;
        if (history[previous][0] < 999.0f)
            for (int axis = 0; axis < 3; ++axis)
                history[index][axis] = history[previous][axis] + 0.5f * (input[axis] - history[previous][axis]);
        else
            history[index] = input;
        // GetMaxRemoteAccelDelta(5), then DetectRightShake's test.
        float best = 0.0f;
        std::array<float, 3> delta{};
        for (unsigned offset = 1; offset < 5; ++offset)
        {
            const auto& old = history[(index + 30 - offset) % 30];
            if (old[0] > 999.0f) break;
            const std::array<float, 3> candidate{history[index][0] - old[0], history[index][1] - old[1],
                                                 history[index][2] - old[2]};
            const float length = candidate[0] * candidate[0] + candidate[1] * candidate[1] + candidate[2] * candidate[2];
            if (length > best)
            {
                best = length;
                delta = candidate;
            }
        }
        if (best > 0.0f)
        {
            const float y = std::fabs(delta[1]) < std::fabs(delta[2]) ? delta[2] : -delta[1];
            seen = seen || delta[0] * delta[0] + y * y > 1.33f * 1.33f;
        }
    }
    return seen;
}

void ShakeChecks()
{
    for (const int rest : {-110, 0, 100, 110})
    {
        Check(Shaken(rest, true), "A shake over a Remote at " + std::to_string(rest) + " went unseen");
        Check(!Shaken(rest, false), "A Remote at rest at " + std::to_string(rest) + " shook");
    }
    int sum = 0;
    for (int step = -1; step <= ShakeSteps; ++step) sum += ShakeAccel(step);
    Check(sum == 0 && ShakeAccel(-1) == 0 && ShakeAccel(ShakeSteps) == 0, "A shake did not end where it began");
}
} // namespace

int main()
{
    try
    {
        StickChecks();
        ShakeChecks();
        std::printf("Autopilot pad: %u checks; sticks run the player at the field angle asked through the camera's "
                    "remap and a network input's bytes, shakes reach the game's shake test and stay in the input's "
                    "byte.\n",
                    checks);
        return 0;
    }
    catch (const std::exception& error)
    {
        std::fprintf(stderr, "Autopilot pad: %s\n", error.what());
        return 1;
    }
}
