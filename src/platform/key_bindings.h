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
KeyBindings ParseKeyBindings(const std::array<std::string, KeyActionCount>& texts);
KeyBindings DefaultKeyBindings();
// The host screenshot key and the key that shows or hides the match HUD
// (No HUD, issue #58), which no action may take.
inline constexpr SDL_Scancode kScreenshotKey = SDL_SCANCODE_P;
inline constexpr SDL_Scancode kHudKey = SDL_SCANCODE_H;
} // namespace mscharged::platform
