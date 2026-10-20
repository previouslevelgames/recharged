#pragma once

#include <array>
#include <cstddef>

namespace mscharged
{
// Actions of the keyboard & mouse Wii Remote ([keyboard] in recharged.ini).
// Each takes one or two keys, written as SDL key names separated by " | ".
struct KeyAction
{
    const char* key;      // INI key
    const char* label;    // launcher label
    const char* note;     // launcher description, or nullptr
    const char* defaults; // default keys
};

enum KeyActionIndex : std::size_t
{
    KeyActionA, KeyActionB, KeyActionOne, KeyActionTwo, KeyActionPlus, KeyActionMinus, KeyActionHome,
    KeyActionUp, KeyActionDown, KeyActionLeft, KeyActionRight,
    KeyActionStickUp, KeyActionStickLeft, KeyActionStickDown, KeyActionStickRight,
    KeyActionC, KeyActionZ, KeyActionShakeRemote, KeyActionShakeNunchuk,
    KeyActionCount
};

inline constexpr std::array<KeyAction, KeyActionCount> kKeyActions{{
    {"a", "A", "Confirm, select", "Return | Space"},
    {"b", "B", "Back", "Escape | Backspace"},
    {"one", "1", nullptr, "Z"},
    {"two", "2", nullptr, "X"},
    {"plus", "+ (Plus)", nullptr, "Tab"},
    {"minus", "\xE2\x88\x92 (Minus)", nullptr, "-"},
    {"home", "HOME", nullptr, "Home"},
    {"up", "D-pad up", nullptr, "Up"},
    {"down", "D-pad down", nullptr, "Down"},
    {"left", "D-pad left", nullptr, "Left"},
    {"right", "D-pad right", nullptr, "Right"},
    {"stick_up", "Nunchuk stick up", nullptr, "W"},
    {"stick_left", "Nunchuk stick left", nullptr, "A"},
    {"stick_down", "Nunchuk stick down", nullptr, "S"},
    {"stick_right", "Nunchuk stick right", nullptr, "D"},
    {"c", "C", "Nunchuk button", "C"},
    {"z", "Z", "Nunchuk button", "V"},
    {"shake_remote", "Shake Remote", "Hit an opponent", "E"},
    {"shake_nunchuk", "Shake Nunchuk", "Switch items", "Q"},
}};

// Hotkeys of the game window, whoever plays ([hotkeys]): one SDL key name
// each, or "none". No keyboard & mouse action takes them.
enum HotkeyIndex : std::size_t
{
    HotkeyScreenshot, HotkeyHud, HotkeyStretch, HotkeyResolutionUp, HotkeyResolutionDown,
    HotkeyCount
};

inline constexpr std::array<KeyAction, HotkeyCount> kHotkeys{{
    {"screenshot", "Screenshot", "Saved in the screenshots folder.", "P"},
    {"hud", "Show or hide the HUD", "In a match.", "H"},
    {"stretch", "Stretch the picture", "Fills the window, or keeps the 16:9 or 4:3 shape. For this run.", "F"},
    {"resolution_up", "Higher 3D resolution", "Sharper, for this run.", "PageUp"},
    {"resolution_down", "Lower 3D resolution", "Faster, for this run.", "PageDown"},
}};
// The value that leaves a hotkey without a key.
inline constexpr const char* kNoHotkey = "none";
} // namespace mscharged
