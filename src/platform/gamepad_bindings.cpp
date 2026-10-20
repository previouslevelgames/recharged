#include "platform/gamepad_bindings.h"

#include <algorithm>
#include <cmath>

namespace mscharged::platform {
namespace {
constexpr const char* kStickDirectionNames[GamepadStickDirectionCount] = {
    "leftstick_up", "leftstick_down", "leftstick_left", "leftstick_right",
    "rightstick_up", "rightstick_down", "rightstick_left", "rightstick_right"};
} // namespace

GamepadInput ParseGamepadInput(std::string_view name) {
    for (int n = 0; n < GamepadStickDirectionCount; ++n)
        if (name == kStickDirectionNames[n]) return kGamepadStickBase + n;
    const std::string text(name);
    const SDL_GamepadButton button = SDL_GetGamepadButtonFromString(text.c_str());
    if (button != SDL_GAMEPAD_BUTTON_INVALID) return GamepadInput(button);
    const SDL_GamepadAxis axis = SDL_GetGamepadAxisFromString(text.c_str());
    if (axis == SDL_GAMEPAD_AXIS_LEFT_TRIGGER || axis == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER)
        return kGamepadTriggerBase + GamepadInput(axis);
    return kGamepadNone;
}

std::string GamepadInputName(GamepadInput input) {
    const char* name = nullptr;
    if (input >= 0 && input < SDL_GAMEPAD_BUTTON_COUNT)
        name = SDL_GetGamepadStringForButton(SDL_GamepadButton(input));
    else if (input == kGamepadTriggerBase + SDL_GAMEPAD_AXIS_LEFT_TRIGGER ||
             input == kGamepadTriggerBase + SDL_GAMEPAD_AXIS_RIGHT_TRIGGER)
        name = SDL_GetGamepadStringForAxis(SDL_GamepadAxis(input - kGamepadTriggerBase));
    else if (IsGamepadStickDirection(input))
        name = kStickDirectionNames[input - kGamepadStickBase];
    return name ? name : "";
}

GamepadBinding ParseGamepadBinding(std::string_view text) {
    GamepadBinding binding{kGamepadNone, kGamepadNone};
    std::size_t used = 0;
    while (!text.empty() && used < binding.size()) {
        const auto bar = text.find('|');
        auto name = text.substr(0, bar);
        text = bar == std::string_view::npos ? std::string_view{} : text.substr(bar + 1);
        while (!name.empty() && name.front() == ' ') name.remove_prefix(1);
        while (!name.empty() && name.back() == ' ') name.remove_suffix(1);
        if (name.empty()) continue;
        const GamepadInput input = ParseGamepadInput(name);
        if (input != kGamepadNone && (used == 0 || binding[0] != input)) binding[used++] = input;
    }
    return binding;
}

std::string FormatGamepadBinding(const GamepadBinding& binding) {
    std::string text;
    for (const GamepadInput input : binding) {
        const auto name = GamepadInputName(input);
        if (name.empty()) continue;
        if (!text.empty()) text += " | ";
        text += name;
    }
    return text.empty() ? "none" : text;
}

namespace {
bool IsNone(std::string_view text) {
    while (!text.empty() && text.front() == ' ') text.remove_prefix(1);
    while (!text.empty() && text.back() == ' ') text.remove_suffix(1);
    return text == "none";
}
} // namespace

GamepadBindings ParseGamepadBindings(const std::array<std::string, GamepadActionCount>& texts) {
    GamepadBindings bindings{};
    for (std::size_t n = 0; n < GamepadActionCount; ++n) {
        bindings[n] = ParseGamepadBinding(texts[n]);
        if (bindings[n][0] == kGamepadNone && !IsNone(texts[n]))
            bindings[n] = ParseGamepadBinding(kGamepadActions[n].defaults);
    }
    return bindings;
}

GamepadBindings DefaultGamepadBindings() { return DefaultGamepadBindings(GamepadFamily::Xbox); }

GamepadBindings DefaultGamepadBindings(GamepadFamily family) {
    return ParseGamepadBindings(DefaultGamepadInputs(family));
}

GamepadFamily GamepadFamilyOf(SDL_GamepadType type) {
    switch (type) {
    case SDL_GAMEPAD_TYPE_PS3:
    case SDL_GAMEPAD_TYPE_PS4:
    case SDL_GAMEPAD_TYPE_PS5: return GamepadFamily::PlayStation;
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO:
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_LEFT:
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_RIGHT:
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_PAIR: return GamepadFamily::Nintendo;
    case SDL_GAMEPAD_TYPE_GAMECUBE: return GamepadFamily::GameCube;
    default: return GamepadFamily::Xbox;
    }
}

SDL_GamepadType GamepadFamilyType(GamepadFamily family) {
    switch (family) {
    case GamepadFamily::PlayStation: return SDL_GAMEPAD_TYPE_PS5;
    case GamepadFamily::Nintendo: return SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO;
    case GamepadFamily::GameCube: return SDL_GAMEPAD_TYPE_GAMECUBE;
    default: return SDL_GAMEPAD_TYPE_XBOXONE;
    }
}

const char* GamepadFamilyName(GamepadFamily family) {
    switch (family) {
    case GamepadFamily::PlayStation: return "PlayStation";
    case GamepadFamily::Nintendo: return "Nintendo";
    case GamepadFamily::GameCube: return "GameCube";
    default: return "Xbox";
    }
}

const char* GamepadProfileKind(GamepadFamily family) {
    switch (family) {
    case GamepadFamily::Nintendo: return "nintendo";
    case GamepadFamily::GameCube: return "gamecube";
    default: return "xbox";
    }
}

GamepadFamily GamepadFamilyOfKind(std::string_view kind) {
    if (kind == "nintendo") return GamepadFamily::Nintendo;
    if (kind == "gamecube") return GamepadFamily::GameCube;
    return GamepadFamily::Xbox;
}

const char* GamepadKindName(std::string_view kind) {
    if (kind == "nintendo") return "Nintendo";
    if (kind == "gamecube") return "GameCube";
    return "Xbox and PlayStation";
}

std::array<std::string, GamepadActionCount> DefaultGamepadInputs(GamepadFamily family) {
    std::array<std::string, GamepadActionCount> inputs;
    for (std::size_t n = 0; n < GamepadActionCount; ++n) inputs[n] = kGamepadActions[n].defaults;
    // SDL names are positions: a = bottom, b = right, x = left, y = top face button.
    if (family == GamepadFamily::Nintendo) {
        // Nintendo prints A on the right and B at the bottom, X on top and Y on the left.
        inputs[GamepadActionA] = "b";
        inputs[GamepadActionB] = "a";
        inputs[GamepadActionOne] = "x";
        inputs[GamepadActionTwo] = "y";
    } else if (family == GamepadFamily::GameCube) {
        // GameCube (USB adapter): A bottom, B left, X right, Y top; L/R are
        // the analog triggers and Z the right shoulder button. It has no
        // buttons left for 1, - and HOME, and no stick buttons (nor a gyro).
        inputs[GamepadActionA] = "a";
        inputs[GamepadActionB] = "x";
        inputs[GamepadActionOne] = "none";
        inputs[GamepadActionTwo] = "y";
        inputs[GamepadActionMinus] = "none";
        inputs[GamepadActionHome] = "none";
        inputs[GamepadActionC] = "lefttrigger";
        inputs[GamepadActionZ] = "rightshoulder";
        inputs[GamepadActionShakeRemote] = "righttrigger";
        inputs[GamepadActionShakeNunchuk] = "b";
        inputs[GamepadActionRecenterPointer] = "none";
    }
    return inputs;
}

Sint16 GamepadTriggerThreshold(int press) {
    // 50 % is kGamepadTriggerPressed, the threshold before press points.
    return Sint16(std::min(std::clamp(press, 0, 100) * 32768 / 100, 32767));
}

bool IsGamepadStickDirection(GamepadInput input) {
    return input >= kGamepadStickBase && input < kGamepadStickBase + GamepadStickDirectionCount;
}

bool GamepadStickDirectionHeld(SDL_Gamepad* pad, GamepadInput input, bool held) {
    if (!IsGamepadStickDirection(input)) return false;
    const int direction = input - kGamepadStickBase;
    const bool right_stick = direction >= RightStickUp;
    const auto axis = [&](SDL_GamepadAxis which) {
        return std::clamp(float(SDL_GetGamepadAxis(pad, which)) / 32767.0f, -1.0f, 1.0f);
    };
    // SDL's y axes point down.
    float lean = 0;
    switch (direction % 4) {
    case 0: lean = -axis(right_stick ? SDL_GAMEPAD_AXIS_RIGHTY : SDL_GAMEPAD_AXIS_LEFTY); break;
    case 1: lean = axis(right_stick ? SDL_GAMEPAD_AXIS_RIGHTY : SDL_GAMEPAD_AXIS_LEFTY); break;
    case 2: lean = -axis(right_stick ? SDL_GAMEPAD_AXIS_RIGHTX : SDL_GAMEPAD_AXIS_LEFTX); break;
    default: lean = axis(right_stick ? SDL_GAMEPAD_AXIS_RIGHTX : SDL_GAMEPAD_AXIS_LEFTX); break;
    }
    return lean >= (held ? kStickDirectionRelease : kStickDirectionPress);
}

bool GamepadInputHeld(SDL_Gamepad* pad, GamepadInput input, int press) {
    if (input >= 0 && input < SDL_GAMEPAD_BUTTON_COUNT) return SDL_GetGamepadButton(pad, SDL_GamepadButton(input));
    if (input >= kGamepadTriggerBase && input < kGamepadTriggerBase + SDL_GAMEPAD_AXIS_COUNT)
        return SDL_GetGamepadAxis(pad, SDL_GamepadAxis(input - kGamepadTriggerBase)) > GamepadTriggerThreshold(press);
    return GamepadStickDirectionHeld(pad, input);
}

bool GamepadBindingHeld(SDL_Gamepad* pad, const GamepadBinding& binding, int press) {
    return GamepadInputHeld(pad, binding[0], press) || GamepadInputHeld(pad, binding[1], press);
}

StickTilt ApplyStickDeadzones(float x, float y, int inner, int outer) {
    const float length = std::hypot(x, y);
    const float start = std::clamp(inner, 0, 100) / 100.0f;
    const float full = std::max(std::clamp(outer, 0, 100) / 100.0f, start + 0.01f);
    if (!(length > start)) return {};
    const float scale = std::min(1.0f, (length - start) / (full - start)) / length;
    return {x * scale, y * scale};
}

StickTilt GamepadStickTilt(SDL_Gamepad* pad, SDL_GamepadAxis x, SDL_GamepadAxis y, int inner, int outer) {
    const auto axis = [&](SDL_GamepadAxis which) {
        return std::clamp(float(SDL_GetGamepadAxis(pad, which)) / 32767.0f, -1.0f, 1.0f);
    };
    return ApplyStickDeadzones(axis(x), axis(y), inner, outer);
}

std::array<std::int8_t, 2> NunchukStickCounts(StickTilt tilt) {
    const auto counts = [](float value) {
        const long moved = std::lround(std::clamp(value, -1.0f, 1.0f) * kNunchukStickReachCounts);
        if (moved == 0) return std::int8_t(0);
        return std::int8_t(moved > 0 ? moved + kNunchukStickDeadCounts : moved - kNunchukStickDeadCounts);
    };
    return {counts(tilt.x), counts(tilt.y)};
}
} // namespace mscharged::platform
