#pragma once

#include "bootstrap/key_actions.h"

#include <SDL3/SDL_keycode.h>
#include <SDL3/SDL_scancode.h>
#include <SDL3/SDL_stdinc.h>

#include <array>
#include <string>
#include <string_view>
#include <vector>

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

// A hotkey of the game window: a key, pressed with exactly the Ctrl, Shift and
// Alt (Option on a Mac) in `modifiers` (SDL_KMOD_CTRL, SDL_KMOD_SHIFT,
// SDL_KMOD_ALT; either side counts). The Windows and Command keys stay the
// system's. SDL_SCANCODE_UNKNOWN leaves it unused.
struct Hotkey {
    SDL_Scancode key = SDL_SCANCODE_UNKNOWN;
    SDL_Keymod modifiers = SDL_KMOD_NONE;
    bool operator==(const Hotkey& other) const { return key == other.key && modifiers == other.modifiers; }
    bool operator!=(const Hotkey& other) const { return !(*this == other); }
};
// The Ctrl, Shift and Alt held in `mod`, as a Hotkey names them.
SDL_Keymod HotkeyModifiers(SDL_Keymod mod);

// The host's hotkeys ([hotkeys], kHotkeys order): a screenshot, the match HUD
// (No HUD, issue #58), the stretched picture and the 3D resolution up and down
// (issue #91), and fullscreen or a window (issue #77). They work whoever
// plays; no keyboard & mouse action takes a hotkey's key pressed alone.
struct Hotkeys {
    std::array<Hotkey, HotkeyCount> keys{{{SDL_SCANCODE_P}, {SDL_SCANCODE_H}, {SDL_SCANCODE_F}, {SDL_SCANCODE_PAGEUP},
                                          {SDL_SCANCODE_PAGEDOWN}, {SDL_SCANCODE_RETURN, SDL_KMOD_ALT}}};
    const Hotkey& operator[](HotkeyIndex hotkey) const { return keys[hotkey]; }
    Hotkey& operator[](HotkeyIndex hotkey) { return keys[hotkey]; }
    // The hotkey that `input` pressed alone is, or HotkeyCount.
    HotkeyIndex Find(KeyInput input) const;
    // The hotkey that pressing `key` with `mod` held presses, or HotkeyCount;
    // the keypad's Enter presses one on Return. Never with Windows or Command.
    HotkeyIndex Match(SDL_Scancode key, SDL_Keymod mod) const;
};
// Whether `hotkey` can be one: a named key, not a mouse button or a modifier.
bool HotkeyAllowed(const Hotkey& hotkey);
// A [hotkeys] text: "Ctrl+Shift+F12", "Alt+Return", "P" (Control, Option and
// Enter also work); anything else is an unused hotkey.
Hotkey ParseHotkey(std::string_view text);
// [hotkeys] texts: "none" leaves a hotkey unused; one that cannot be a hotkey
// keeps its default, and two hotkeys on the same keys both keep their defaults.
Hotkeys ParseHotkeys(const std::array<std::string, HotkeyCount>& names);
// The [hotkeys] text of a hotkey ("Alt+Return"), or "none".
std::string HotkeyName(const Hotkey& hotkey);
// The keys of a hotkey as a keyboard shows them, in the order they are held:
// "Alt", "Enter" ("Option", "Return" on a Mac); none when it is unused.
std::vector<std::string> HotkeyKeys(const Hotkey& hotkey);
// The same joined for text ("Alt+Enter"), or "" when it is unused.
std::string HotkeyLabel(const Hotkey& hotkey);

// The actions' keys without the hotkeys; an action left without a key keeps
// its default keys (also without the hotkeys).
KeyBindings ParseKeyBindings(const std::array<std::string, KeyActionCount>& texts, const Hotkeys& hotkeys);
KeyBindings DefaultKeyBindings();
} // namespace mscharged::platform
