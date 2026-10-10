#include "platform/key_bindings.h"

#include <SDL3/SDL_keyboard.h>

namespace mscharged::platform {
KeyBinding ParseKeyBinding(std::string_view text) {
    KeyBinding binding{SDL_SCANCODE_UNKNOWN, SDL_SCANCODE_UNKNOWN};
    std::size_t used = 0;
    while (!text.empty() && used < binding.size()) {
        const auto bar = text.find('|');
        auto name = text.substr(0, bar);
        text = bar == std::string_view::npos ? std::string_view{} : text.substr(bar + 1);
        while (!name.empty() && name.front() == ' ') name.remove_prefix(1);
        while (!name.empty() && name.back() == ' ') name.remove_suffix(1);
        if (name.empty()) continue;
        const SDL_Scancode code = SDL_GetScancodeFromName(std::string(name).c_str());
        if (code != SDL_SCANCODE_UNKNOWN && code != kScreenshotKey) binding[used++] = code;
    }
    return binding;
}

std::string FormatKeyBinding(const KeyBinding& binding) {
    std::string text;
    for (const SDL_Scancode code : binding) {
        if (code == SDL_SCANCODE_UNKNOWN) continue;
        if (!text.empty()) text += " | ";
        text += SDL_GetScancodeName(code);
    }
    return text;
}

KeyBindings ParseKeyBindings(const std::array<std::string, KeyActionCount>& texts) {
    KeyBindings bindings{};
    for (std::size_t n = 0; n < KeyActionCount; ++n) {
        bindings[n] = ParseKeyBinding(texts[n]);
        // An action without a usable key keeps its default.
        if (bindings[n][0] == SDL_SCANCODE_UNKNOWN && bindings[n][1] == SDL_SCANCODE_UNKNOWN)
            bindings[n] = ParseKeyBinding(kKeyActions[n].defaults);
        else if (bindings[n][0] == SDL_SCANCODE_UNKNOWN)
            bindings[n] = {bindings[n][1], SDL_SCANCODE_UNKNOWN};
    }
    return bindings;
}

KeyBindings DefaultKeyBindings() {
    KeyBindings bindings{};
    for (std::size_t n = 0; n < KeyActionCount; ++n) bindings[n] = ParseKeyBinding(kKeyActions[n].defaults);
    return bindings;
}
} // namespace mscharged::platform
