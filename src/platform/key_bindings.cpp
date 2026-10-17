#include "platform/key_bindings.h"

#include <SDL3/SDL_keyboard.h>
#include <SDL3/SDL_mouse.h>

#include <cctype>

namespace mscharged::platform {
namespace {
struct MouseKey {
    Uint8 button;
    const char* name;
};
constexpr MouseKey kMouseKeys[]{
    {SDL_BUTTON_MIDDLE, "Mouse Middle"}, {SDL_BUTTON_X1, "Mouse 4"}, {SDL_BUTTON_X2, "Mouse 5"}};

bool SameName(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (std::size_t n = 0; n < a.size(); ++n)
        if (std::tolower(static_cast<unsigned char>(a[n])) != std::tolower(static_cast<unsigned char>(b[n])))
            return false;
    return true;
}

KeyInput ParseKeyInput(std::string_view name) {
    for (const auto& mouse : kMouseKeys)
        if (SameName(name, mouse.name)) return kMouseKeyBase + mouse.button;
    const SDL_Scancode code = SDL_GetScancodeFromName(std::string(name).c_str());
    return code == kScreenshotKey || code == kHudKey ? kKeyNone : code;
}
} // namespace

KeyInput MouseKeyInput(Uint8 button) {
    for (const auto& mouse : kMouseKeys)
        if (mouse.button == button) return kMouseKeyBase + button;
    return kKeyNone;
}

Uint8 MouseButtonOf(KeyInput input) {
    for (const auto& mouse : kMouseKeys)
        if (input == kMouseKeyBase + mouse.button) return mouse.button;
    return 0;
}

std::string KeyInputName(KeyInput input) {
    for (const auto& mouse : kMouseKeys)
        if (input == kMouseKeyBase + mouse.button) return mouse.name;
    if (input <= kKeyNone || input >= SDL_SCANCODE_COUNT) return {};
    return SDL_GetScancodeName(SDL_Scancode(input));
}

KeyBinding ParseKeyBinding(std::string_view text) {
    KeyBinding binding{kKeyNone, kKeyNone};
    std::size_t used = 0;
    while (!text.empty() && used < binding.size()) {
        const auto bar = text.find('|');
        auto name = text.substr(0, bar);
        text = bar == std::string_view::npos ? std::string_view{} : text.substr(bar + 1);
        while (!name.empty() && name.front() == ' ') name.remove_prefix(1);
        while (!name.empty() && name.back() == ' ') name.remove_suffix(1);
        if (name.empty()) continue;
        const KeyInput input = ParseKeyInput(name);
        if (input != kKeyNone && (used == 0 || binding[0] != input)) binding[used++] = input;
    }
    return binding;
}

std::string FormatKeyBinding(const KeyBinding& binding) {
    std::string text;
    for (const KeyInput input : binding) {
        const auto name = KeyInputName(input);
        if (name.empty()) continue;
        if (!text.empty()) text += " | ";
        text += name;
    }
    return text;
}

KeyBindings ParseKeyBindings(const std::array<std::string, KeyActionCount>& texts) {
    KeyBindings bindings{};
    for (std::size_t n = 0; n < KeyActionCount; ++n) {
        bindings[n] = ParseKeyBinding(texts[n]);
        // An action without a usable key keeps its default.
        if (bindings[n][0] == kKeyNone) bindings[n] = ParseKeyBinding(kKeyActions[n].defaults);
    }
    return bindings;
}

KeyBindings DefaultKeyBindings() {
    KeyBindings bindings{};
    for (std::size_t n = 0; n < KeyActionCount; ++n) bindings[n] = ParseKeyBinding(kKeyActions[n].defaults);
    return bindings;
}
} // namespace mscharged::platform
