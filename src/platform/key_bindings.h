#pragma once

#include "bootstrap/key_actions.h"

#include <SDL3/SDL_scancode.h>

#include <array>
#include <string>
#include <string_view>

namespace mscharged::platform {
// One or two keys per keyboard & mouse action; SDL_SCANCODE_UNKNOWN = unused.
using KeyBinding = std::array<SDL_Scancode, 2>;
using KeyBindings = std::array<KeyBinding, KeyActionCount>;

// "Return | Space" (SDL key names). Unknown names are left out.
KeyBinding ParseKeyBinding(std::string_view text);
std::string FormatKeyBinding(const KeyBinding& binding);
KeyBindings ParseKeyBindings(const std::array<std::string, KeyActionCount>& texts);
KeyBindings DefaultKeyBindings();
// The host screenshot key, which no action may take.
inline constexpr SDL_Scancode kScreenshotKey = SDL_SCANCODE_P;
} // namespace mscharged::platform
