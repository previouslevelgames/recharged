#pragma once

#include <array>
#include <cstddef>

namespace mscharged
{
// Actions of a standard gamepad playing as a Wii Remote with Nunchuk. Each
// gamepad profile ([gamepad_profileN] in mscharged.ini) gives every action
// one or two inputs, written as SDL gamepad names separated by " | ":
// buttons (a, b, x, y = south, east, west, north; start, back, guide,
// leftshoulder, leftstick, dpup, ...) or the triggers lefttrigger/righttrigger.
// The Nunchuk stick and the pointer are the two analog sticks.
struct GamepadAction
{
    const char* key;      // INI key
    const char* label;    // launcher label
    const char* note;     // launcher description, or nullptr
    const char* defaults; // default inputs
};

// The first eleven follow the Wii Remote button order of KeyActionIndex.
enum GamepadActionIndex : std::size_t
{
    GamepadActionA, GamepadActionB, GamepadActionOne, GamepadActionTwo, GamepadActionPlus, GamepadActionMinus,
    GamepadActionHome, GamepadActionUp, GamepadActionDown, GamepadActionLeft, GamepadActionRight,
    GamepadActionC, GamepadActionZ, GamepadActionShakeRemote, GamepadActionShakeNunchuk,
    GamepadActionCount
};

inline constexpr std::array<GamepadAction, GamepadActionCount> kGamepadActions{{
    {"a", "A", "Confirm, select", "a"},
    {"b", "B", "Back", "b"},
    {"one", "1", nullptr, "x"},
    {"two", "2", nullptr, "y"},
    {"plus", "+ (Plus)", nullptr, "start"},
    {"minus", "\xE2\x88\x92 (Minus)", nullptr, "back"},
    {"home", "HOME", nullptr, "guide"},
    {"up", "D-pad up", nullptr, "dpup"},
    {"down", "D-pad down", nullptr, "dpdown"},
    {"left", "D-pad left", nullptr, "dpleft"},
    {"right", "D-pad right", nullptr, "dpright"},
    {"c", "C", "Nunchuk button", "leftshoulder"},
    {"z", "Z", "Nunchuk button", "lefttrigger"},
    {"shake_remote", "Shake Remote", "Hit an opponent", "righttrigger"},
    {"shake_nunchuk", "Shake Nunchuk", "Switch items", "rightshoulder"},
}};

inline constexpr std::size_t kGamepadSlots = 4;
// Named gamepad profiles kept in mscharged.ini, beside the built-in "Default"
// (the defaults above), which is never stored, changed or deleted.
inline constexpr std::size_t kMaxGamepadProfiles = 16;
inline constexpr const char* kDefaultGamepadProfile = "Default";
inline constexpr std::size_t kMaxGamepadProfileName = 32;
} // namespace mscharged
