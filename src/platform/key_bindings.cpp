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
    return SDL_GetScancodeFromName(std::string(name).c_str());
}

KeyBinding WithoutHotkeys(KeyBinding binding, const Hotkeys& hotkeys) {
    for (auto& input : binding)
        if (hotkeys.Find(input) != HotkeyCount) input = kKeyNone;
    if (binding[0] == kKeyNone) binding = {binding[1], kKeyNone};
    return binding;
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

SDL_Keymod HotkeyModifiers(SDL_Keymod mod) {
    SDL_Keymod modifiers = SDL_KMOD_NONE;
    if (mod & SDL_KMOD_CTRL) modifiers |= SDL_KMOD_CTRL;
    if (mod & SDL_KMOD_SHIFT) modifiers |= SDL_KMOD_SHIFT;
    if (mod & SDL_KMOD_ALT) modifiers |= SDL_KMOD_ALT;
    return modifiers;
}

bool HotkeyAllowed(const Hotkey& hotkey) {
    if (hotkey.key <= SDL_SCANCODE_UNKNOWN || hotkey.key >= SDL_SCANCODE_COUNT || KeyInputName(hotkey.key).empty())
        return false;
    return (hotkey.key < SDL_SCANCODE_LCTRL || hotkey.key > SDL_SCANCODE_RGUI)
           && hotkey.modifiers == HotkeyModifiers(hotkey.modifiers);
}

HotkeyIndex Hotkeys::Find(KeyInput input) const {
    if (input == kKeyNone) return HotkeyCount;
    for (std::size_t n = 0; n < HotkeyCount; ++n)
        if (keys[n].key == input && keys[n].modifiers == SDL_KMOD_NONE) return HotkeyIndex(n);
    return HotkeyCount;
}

HotkeyIndex Hotkeys::Match(SDL_Scancode key, SDL_Keymod mod) const {
    if (key == SDL_SCANCODE_UNKNOWN || (mod & SDL_KMOD_GUI)) return HotkeyCount;
    const SDL_Keymod modifiers = HotkeyModifiers(mod);
    for (std::size_t n = 0; n < HotkeyCount; ++n)
        if (keys[n].key != SDL_SCANCODE_UNKNOWN && keys[n].modifiers == modifiers
            && (keys[n].key == key || (keys[n].key == SDL_SCANCODE_RETURN && key == SDL_SCANCODE_KP_ENTER)))
            return HotkeyIndex(n);
    return HotkeyCount;
}

Hotkey ParseHotkey(std::string_view text) {
    struct Modifier {
        const char* name;
        SDL_Keymod modifier;
    };
    static constexpr Modifier kModifiers[]{{"Ctrl", SDL_KMOD_CTRL}, {"Control", SDL_KMOD_CTRL},
                                           {"Shift", SDL_KMOD_SHIFT}, {"Alt", SDL_KMOD_ALT}, {"Option", SDL_KMOD_ALT}};
    const auto trim = [](std::string_view& part) {
        while (!part.empty() && part.front() == ' ') part.remove_prefix(1);
        while (!part.empty() && part.back() == ' ') part.remove_suffix(1);
    };
    Hotkey hotkey;
    // Modifiers come first, each followed by '+' ("Keypad +" is a key name).
    for (bool found = true; found;) {
        found = false;
        trim(text);
        const auto plus = text.find('+');
        if (plus == std::string_view::npos || plus + 1 >= text.size()) break;
        auto name = text.substr(0, plus);
        trim(name);
        for (const auto& modifier : kModifiers)
            if (SameName(name, modifier.name)) {
                hotkey.modifiers = SDL_Keymod(hotkey.modifiers | modifier.modifier);
                text.remove_prefix(plus + 1);
                found = true;
                break;
            }
    }
    trim(text);
    const KeyInput input = SameName(text, "Enter") ? KeyInput(SDL_SCANCODE_RETURN) : ParseKeyInput(text);
    if (input > kKeyNone && input < SDL_SCANCODE_COUNT) hotkey.key = SDL_Scancode(input);
    return hotkey;
}

Hotkeys ParseHotkeys(const std::array<std::string, HotkeyCount>& names) {
    const Hotkeys defaults;
    Hotkeys hotkeys;
    for (std::size_t n = 0; n < HotkeyCount; ++n) {
        if (SameName(names[n], kNoHotkey)) { hotkeys.keys[n] = {}; continue; }
        const Hotkey hotkey = ParseHotkey(names[n]);
        hotkeys.keys[n] = HotkeyAllowed(hotkey) ? hotkey : defaults.keys[n];
    }
    // Two hotkeys on the same keys both take their defaults, until none share
    // them (the defaults differ; each round puts at least one more at its default).
    for (bool shared = true; shared;) {
        shared = false;
        for (std::size_t a = 0; a < HotkeyCount; ++a)
            for (std::size_t b = a + 1; b < HotkeyCount; ++b)
                if (hotkeys.keys[a].key != SDL_SCANCODE_UNKNOWN && hotkeys.keys[a] == hotkeys.keys[b]
                    && (hotkeys.keys[a] != defaults.keys[a] || hotkeys.keys[b] != defaults.keys[b])) {
                    hotkeys.keys[a] = defaults.keys[a];
                    hotkeys.keys[b] = defaults.keys[b];
                    shared = true;
                }
    }
    return hotkeys;
}

std::string HotkeyName(const Hotkey& hotkey) {
    const auto name = KeyInputName(hotkey.key);
    if (name.empty()) return kNoHotkey;
    return std::string(hotkey.modifiers & SDL_KMOD_CTRL ? "Ctrl+" : "") + (hotkey.modifiers & SDL_KMOD_SHIFT ? "Shift+" : "")
           + (hotkey.modifiers & SDL_KMOD_ALT ? "Alt+" : "") + name;
}

std::vector<std::string> HotkeyKeys(const Hotkey& hotkey) {
    std::vector<std::string> keys;
    const auto name = KeyInputName(hotkey.key);
    if (name.empty()) return keys;
#if defined(__APPLE__)
    if (hotkey.modifiers & SDL_KMOD_CTRL) keys.emplace_back("Control");
    if (hotkey.modifiers & SDL_KMOD_SHIFT) keys.emplace_back("Shift");
    if (hotkey.modifiers & SDL_KMOD_ALT) keys.emplace_back("Option");
    keys.push_back(name);
#else
    if (hotkey.modifiers & SDL_KMOD_CTRL) keys.emplace_back("Ctrl");
    if (hotkey.modifiers & SDL_KMOD_SHIFT) keys.emplace_back("Shift");
    if (hotkey.modifiers & SDL_KMOD_ALT) keys.emplace_back("Alt");
    // The key SDL names Return reads Enter on PC keyboards.
    keys.push_back(hotkey.key == SDL_SCANCODE_RETURN ? std::string("Enter") : name);
#endif
    return keys;
}

std::string HotkeyLabel(const Hotkey& hotkey) {
    std::string label;
    for (const auto& key : HotkeyKeys(hotkey)) label += (label.empty() ? "" : "+") + key;
    return label;
}

KeyBindings ParseKeyBindings(const std::array<std::string, KeyActionCount>& texts, const Hotkeys& hotkeys) {
    KeyBindings bindings{};
    for (std::size_t n = 0; n < KeyActionCount; ++n) {
        bindings[n] = WithoutHotkeys(ParseKeyBinding(texts[n]), hotkeys);
        // An action without a usable key keeps its default.
        if (bindings[n][0] == kKeyNone)
            bindings[n] = WithoutHotkeys(ParseKeyBinding(kKeyActions[n].defaults), hotkeys);
    }
    return bindings;
}

KeyBindings DefaultKeyBindings() {
    KeyBindings bindings{};
    for (std::size_t n = 0; n < KeyActionCount; ++n) bindings[n] = ParseKeyBinding(kKeyActions[n].defaults);
    return bindings;
}
} // namespace mscharged::platform
