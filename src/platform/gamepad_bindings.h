#pragma once

#include "bootstrap/gamepad_actions.h"

#include <SDL3/SDL_gamepad.h>

#include <array>
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
// A trigger counts as pressed past half its travel.
inline constexpr Sint16 kGamepadTriggerPressed = 16384;
bool GamepadInputHeld(SDL_Gamepad* pad, GamepadInput input);
bool GamepadBindingHeld(SDL_Gamepad* pad, const GamepadBinding& binding);
} // namespace mscharged::platform
