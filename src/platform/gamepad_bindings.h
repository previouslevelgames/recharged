#pragma once

#include "bootstrap/gamepad_actions.h"

#include <SDL3/SDL_gamepad.h>

#include <array>
#include <cstdint>
#include <string>
#include <string_view>

namespace mscharged::platform {
// A gamepad input: an SDL button (0 .. SDL_GAMEPAD_BUTTON_COUNT - 1), a
// trigger (kGamepadTriggerBase + its SDL axis), a stick direction
// (kGamepadStickBase + GamepadStickDirection) or kGamepadNone.
using GamepadInput = int;
inline constexpr GamepadInput kGamepadNone = -1;
inline constexpr GamepadInput kGamepadTriggerBase = 64;
inline constexpr GamepadInput kGamepadStickBase = 80;
// Pushing a stick one way presses a button (issue #14), e.g. the right stick
// for the D-pad: leftstick_up ... rightstick_right in recharged.ini.
enum GamepadStickDirection {
    LeftStickUp, LeftStickDown, LeftStickLeft, LeftStickRight,
    RightStickUp, RightStickDown, RightStickLeft, RightStickRight,
    GamepadStickDirectionCount
};
// One or two inputs per gamepad action; kGamepadNone = unused.
using GamepadBinding = std::array<GamepadInput, 2>;
using GamepadBindings = std::array<GamepadBinding, GamepadActionCount>;

// Settings of one gamepad player.
struct GamepadProfile {
    GamepadBindings bindings{};
    // false: the left stick moves the Nunchuk stick and the right stick points;
    // true swaps them.
    bool swap_sticks = false;
    // The built-in Default: the bindings are replaced by the default layout
    // of the controller that connects (DefaultGamepadBindings(family)).
    bool follow_controller = false;
    // Dead zones of both sticks and the triggers' press point, in percent of
    // travel (see gamepad_actions.h).
    int inner_deadzone = kDefaultInnerDeadzone;
    int outer_deadzone = kDefaultOuterDeadzone;
    int trigger_press = kDefaultTriggerPress;
    // The Nunchuk stick plays like a real one (OctagonNunchukStickCounts); the
    // dead zones then shape the pointer stick only.
    bool octagon_stick = false;
    // The controller's gyro moves the pointer instead of the stick, when it
    // has one (issue #14); gyro_speed in percent (see gamepad_actions.h).
    bool gyro_pointer = false;
    int gyro_speed = kDefaultGyroSpeed;
};

// Controller families with their own default layout and button names.
enum class GamepadFamily { Xbox, PlayStation, Nintendo, GameCube };
inline constexpr int kGamepadFamilyCount = 4;
GamepadFamily GamepadFamilyOf(SDL_GamepadType type);
// A typical SDL type of the family, for its button names.
SDL_GamepadType GamepadFamilyType(GamepadFamily family);
const char* GamepadFamilyName(GamepadFamily family);
// Profiles are kept per kind: "xbox" (Xbox and PlayStation share button
// positions), "nintendo" or "gamecube". A kind's family for names/defaults.
const char* GamepadProfileKind(GamepadFamily family);
GamepadFamily GamepadFamilyOfKind(std::string_view kind);
// "Xbox and PlayStation", "Nintendo" or "GameCube".
const char* GamepadKindName(std::string_view kind);
// The default layout of a family as INI text per action. Every family keeps
// the Wii buttons where that controller's own labels suggest: Nintendo's A
// is its right face button; GameCube lacks buttons for 1, - and HOME.
std::array<std::string, GamepadActionCount> DefaultGamepadInputs(GamepadFamily family);

// SDL names ("a", "leftshoulder", "righttrigger") and stick directions
// ("rightstick_up"); unknown names, analog stick axes ("leftx") and
// kGamepadNone give kGamepadNone / an empty string.
GamepadInput ParseGamepadInput(std::string_view name);
std::string GamepadInputName(GamepadInput input);
// "a | start", or "none" for no input. Unknown names are left out.
GamepadBinding ParseGamepadBinding(std::string_view text);
std::string FormatGamepadBinding(const GamepadBinding& binding);
// An action without a usable input keeps its default; "none" leaves it
// without one.
GamepadBindings ParseGamepadBindings(const std::array<std::string, GamepadActionCount>& texts);
GamepadBindings DefaultGamepadBindings();
GamepadBindings DefaultGamepadBindings(GamepadFamily family);
// Half a trigger's travel: the launcher takes a trigger press there.
inline constexpr Sint16 kGamepadTriggerPressed = 16384;
// A trigger counts as pressed past `press` percent of its travel.
bool GamepadInputHeld(SDL_Gamepad* pad, GamepadInput input, int press = kDefaultTriggerPress);
bool GamepadBindingHeld(SDL_Gamepad* pad, const GamepadBinding& binding, int press = kDefaultTriggerPress);
// The trigger value (0-32767) past which it counts as pressed.
Sint16 GamepadTriggerThreshold(int press);
bool IsGamepadStickDirection(GamepadInput input);
// A stick direction is pressed once the stick leans that way past half its
// travel and stays pressed until it falls back below 40 % (`held`: pressed so
// far). Pushed diagonally, the stick presses two directions.
inline constexpr float kStickDirectionPress = 0.5f, kStickDirectionRelease = 0.4f;
bool GamepadStickDirectionHeld(SDL_Gamepad* pad, GamepadInput input, bool held = false);

// A stick position (each axis -1..1) after a profile's dead zones, which are
// round: 0 up to the inner dead zone, then rising evenly to length 1 at the
// outer one, in the stick's own direction.
struct StickTilt {
    float x = 0, y = 0;
};
StickTilt ApplyStickDeadzones(float x, float y, int inner, int outer);
// The stick of `pad` (SDL axes, y down) after the dead zones.
StickTilt GamepadStickTilt(SDL_Gamepad* pad, SDL_GamepadAxis x, SDL_GamepadAxis y, int inner, int outer);
// Nunchuk stick counts (after WPAD's centre calibration, y up) that the game
// reads as `tilt` (y up). The game itself drops the first 15 counts of each
// axis and is fully pushed 56 counts further (ClampWiiStick, and KPAD's 15 and
// 71), so a moving axis gets those 15 counts added: the dead zones above are
// then the only ones the player feels.
inline constexpr int kNunchukStickDeadCounts = 15;
inline constexpr int kNunchukStickReachCounts = 56;
std::array<std::int8_t, 2> NunchukStickCounts(StickTilt tilt);
// The counts of a real Nunchuk pushed to `tilt` (y up, length up to 1): the
// pad's round travel is stretched onto the Nunchuk's octagonal gate, whose
// corners in the eight directions reach about kNunchukGateCounts, and nothing
// is added, so the game's own dead zone of 15 counts on each axis and its full
// push at 71 (ClampWiiStick) are the ones the player feels.
inline constexpr int kNunchukGateCounts = 96;
std::array<std::int8_t, 2> OctagonNunchukStickCounts(StickTilt tilt);
// How far the game reads Nunchuk `counts` pushed (ClampWiiStick), 0..1.
float NunchukStickPushed(std::array<std::int8_t, 2> counts);
} // namespace mscharged::platform
