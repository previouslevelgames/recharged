#pragma once

#include "bootstrap/key_actions.h"

#include <SDL3/SDL_scancode.h>
#include <SDL3/SDL_stdinc.h>

#include <array>
#include <string>
#include <string_view>

namespace mscharged::platform {
// A keyboard & mouse input: an SDL scancode, or one of the mouse buttons an
// action can take (kMouseKeyBase + SDL button: middle, and the side buttons
// X1 and X2, named "Mouse 4" and "Mouse 5"; issue #62). Left and right click
// stay A and B. kKeyNone (SDL_SCANCODE_UNKNOWN) = unused.
using KeyInput = int;
inline constexpr KeyInput kKeyNone = SDL_SCANCODE_UNKNOWN;
inline constexpr KeyInput kMouseKeyBase = SDL_SCANCODE_COUNT;
// The input of mouse button `button` (SDL_BUTTON_*), or kKeyNone for a button
// no action can take.
KeyInput MouseKeyInput(Uint8 button);
// The mouse button (SDL_BUTTON_*) of `input`, or 0 for a key.
Uint8 MouseButtonOf(KeyInput input);
// "Return", "Mouse 4"; "" for kKeyNone or an unknown input.
std::string KeyInputName(KeyInput input);

// One or two inputs per keyboard & mouse action; kKeyNone = unused.
using KeyBinding = std::array<KeyInput, 2>;
using KeyBindings = std::array<KeyBinding, KeyActionCount>;

// "Return | Mouse 4" (SDL key names, or mouse button names). Unknown names
// are left out.
KeyBinding ParseKeyBinding(std::string_view text);
std::string FormatKeyBinding(const KeyBinding& binding);

// The host's hotkeys ([hotkeys], kHotkeys order): a screenshot, the match HUD
// (No HUD, issue #58), the stretched picture and the 3D resolution up and down
// (issue #91). They work whoever plays, and no keyboard & mouse action takes
// them. SDL_SCANCODE_UNKNOWN (kKeyNone) leaves one unused.
struct Hotkeys {
    std::array<SDL_Scancode, HotkeyCount> keys{SDL_SCANCODE_P, SDL_SCANCODE_H, SDL_SCANCODE_F, SDL_SCANCODE_PAGEUP,
                                               SDL_SCANCODE_PAGEDOWN};
    SDL_Scancode operator[](HotkeyIndex hotkey) const { return keys[hotkey]; }
    SDL_Scancode& operator[](HotkeyIndex hotkey) { return keys[hotkey]; }
    // The hotkey that `input` presses, or HotkeyCount.
    HotkeyIndex Find(KeyInput input) const;
};
// Whether `input` can be a hotkey: a named key, not a mouse button and not a
// modifier (shortcuts such as Alt+Enter need those).
bool HotkeyAllowed(KeyInput input);
// [hotkeys] names: "none" leaves a hotkey unused; a key that cannot be a
// hotkey keeps its default, and two hotkeys naming the same key both keep
// their defaults.
Hotkeys ParseHotkeys(const std::array<std::string, HotkeyCount>& names);
// The [hotkeys] name of a key: its SDL name, or "none".
std::string HotkeyName(SDL_Scancode key);

// The actions' keys without the hotkeys; an action left without a key keeps
// its default keys (also without the hotkeys).
KeyBindings ParseKeyBindings(const std::array<std::string, KeyActionCount>& texts, const Hotkeys& hotkeys);
KeyBindings DefaultKeyBindings();
} // namespace mscharged::platform
