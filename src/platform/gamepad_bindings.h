#pragma once

#include "bootstrap/gamepad_actions.h"

#include <SDL3/SDL_gamepad.h>

#include <array>
#include <cstdint>
#include <string>
#include <string_view>

namespace mscharged::platform {
// A gamepad input: an SDL button (0 .. SDL_GAMEPAD_BUTTON_COUNT - 1), a
// trigger (kGamepadTriggerBase + its SDL axis) or kGamepadNone.
using GamepadInput = int;
inline constexpr GamepadInput kGamepadNone = -1;
inline constexpr GamepadInput kGamepadTriggerBase = 64;
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

// SDL names ("a", "leftshoulder", "righttrigger"); unknown names, analog
// stick axes and kGamepadNone give kGamepadNone / an empty string.
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
} // namespace mscharged::platform
