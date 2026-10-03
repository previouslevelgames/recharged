#include "runtime/debug_camera_input.h"
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace mscharged
{
namespace
{
constexpr float Deadzone = .18f;
float Stick(Sint16 value)
{
    const float axis = value / (value < 0 ? 32768.f : 32767.f);
    const float magnitude = std::abs(axis);
    return magnitude <= Deadzone ? 0 : std::copysign((magnitude-Deadzone)/(1-Deadzone), axis);
}
float Trigger(Sint16 value)
{
    const float axis = std::max(0, int(value)) / 32767.f;
    return axis <= .05f ? 0 : (axis-.05f)/.95f;
}
struct Channel
{
    DebugCameraInputs input;
    bool reset = false;
    bool Neutral() const
    {
        return !reset && input.left_x == 0 && input.left_y == 0 && input.right_x == 0 && input.right_y == 0
            && !input.increase && !input.decrease && !input.height_modifier
            && input.height_up_pressure == 0 && input.height_down_pressure == 0;
    }
};
Channel Gate(Channel value, bool enabled, bool& blocked)
{
    if (!enabled) blocked = true;
    if (blocked)
    {
        if (enabled && value.Neutral()) blocked = false;
        return {};
    }
    return value;
}
}
DebugCameraCommand DebugCameraInputMap::Sample(const DebugCameraDevices& devices, DebugCameraCapture capture)
{
    const auto& key = devices.keys;
    Channel keyboard;
    keyboard.input.left_x = float(key[SDL_SCANCODE_D])-float(key[SDL_SCANCODE_A]);
    keyboard.input.left_y = float(key[SDL_SCANCODE_W])-float(key[SDL_SCANCODE_S]);
    keyboard.input.right_x = float(key[SDL_SCANCODE_RIGHT])-float(key[SDL_SCANCODE_LEFT]);
    keyboard.input.right_y = float(key[SDL_SCANCODE_UP])-float(key[SDL_SCANCODE_DOWN]);
    keyboard.input.increase = key[SDL_SCANCODE_E]; keyboard.input.decrease = key[SDL_SCANCODE_Q];
    keyboard.input.height_modifier = key[SDL_SCANCODE_LSHIFT] || key[SDL_SCANCODE_RSHIFT];
    keyboard.reset = key[SDL_SCANCODE_R];
    keyboard = Gate(keyboard, capture.focused && !capture.keyboard, keyboard_blocked_);
    if (devices.gamepad != gamepad_) { gamepad_ = devices.gamepad; gamepad_blocked_ = true; }
    Channel pad;
    pad.input.left_x = Stick(devices.axes[SDL_GAMEPAD_AXIS_LEFTX]);
    pad.input.left_y = -Stick(devices.axes[SDL_GAMEPAD_AXIS_LEFTY]);
    pad.input.right_x = Stick(devices.axes[SDL_GAMEPAD_AXIS_RIGHTX]);
    pad.input.right_y = -Stick(devices.axes[SDL_GAMEPAD_AXIS_RIGHTY]);
    pad.input.increase = devices.buttons[SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER];
    pad.input.decrease = devices.buttons[SDL_GAMEPAD_BUTTON_LEFT_SHOULDER];
    pad.input.height_modifier = devices.buttons[SDL_GAMEPAD_BUTTON_WEST];
    pad.input.height_up_pressure = Trigger(devices.axes[SDL_GAMEPAD_AXIS_RIGHT_TRIGGER]);
    pad.input.height_down_pressure = Trigger(devices.axes[SDL_GAMEPAD_AXIS_LEFT_TRIGGER]);
    pad.reset = devices.buttons[SDL_GAMEPAD_BUTTON_BACK];
    pad = Gate(pad, capture.focused && !capture.gamepad && devices.gamepad != 0, gamepad_blocked_);
    DebugCameraCommand command;
    auto& out = command.inputs;
    out.left_x = std::clamp(keyboard.input.left_x+pad.input.left_x, -1.f, 1.f);
    out.left_y = std::clamp(keyboard.input.left_y+pad.input.left_y, -1.f, 1.f);
    out.right_x = std::clamp(keyboard.input.right_x+pad.input.right_x, -1.f, 1.f);
    out.right_y = std::clamp(keyboard.input.right_y+pad.input.right_y, -1.f, 1.f);
    out.increase = keyboard.input.increase || pad.input.increase;
    out.decrease = keyboard.input.decrease || pad.input.decrease;
    out.increase_pressure = out.increase ? 1.f : 0.f;
    out.decrease_pressure = out.decrease ? 1.f : 0.f;
    out.increase_edge = out.increase && !increase_; out.decrease_edge = out.decrease && !decrease_;
    out.height_modifier = keyboard.input.height_modifier || pad.input.height_modifier;
    out.height_up_pressure = pad.input.height_up_pressure;
    out.height_down_pressure = pad.input.height_down_pressure;
    const bool reset = keyboard.reset || pad.reset;
    command.reset = reset && !reset_;
    increase_ = out.increase; decrease_ = out.decrease; reset_ = reset;
    return command;
}
DebugCameraInput::~DebugCameraInput() { if (gamepad_) SDL_CloseGamepad(gamepad_); }
DebugCameraDevices DebugCameraInput::ReadDevices()
{
    if (gamepad_ && !SDL_GamepadConnected(gamepad_)) { SDL_CloseGamepad(gamepad_); gamepad_ = nullptr; }
    if (!gamepad_)
    {
        int count = 0;
        auto* ids = SDL_GetGamepads(&count);
        for (int i = 0; i < count && !gamepad_; ++i)
            if (!preferred_ || preferred_ == ids[i]) gamepad_ = SDL_OpenGamepad(ids[i]);
        SDL_free(ids);
    }
    DebugCameraDevices state;
    int count = 0;
    const bool* keys = SDL_GetKeyboardState(&count);
    if (keys) std::copy_n(keys, std::min<std::size_t>(count, state.keys.size()), state.keys.begin());
    if (gamepad_)
    {
        state.gamepad = SDL_GetGamepadID(gamepad_);
        for (unsigned i = 0; i < state.axes.size(); ++i) state.axes[i] = SDL_GetGamepadAxis(gamepad_, SDL_GamepadAxis(i));
        for (unsigned i = 0; i < state.buttons.size(); ++i) state.buttons[i] = SDL_GetGamepadButton(gamepad_, SDL_GamepadButton(i));
    }
    return state;
}
DebugCameraCommand DebugCameraInput::Poll(SDL_Window* window, bool keyboard_capture, bool gamepad_capture)
{
    if (!window) throw std::invalid_argument("Debug camera input needs its preview window");
    const auto flags = SDL_GetWindowFlags(window);
    return mapping_.Sample(ReadDevices(), {bool(flags & SDL_WINDOW_INPUT_FOCUS) && !(flags & SDL_WINDOW_MINIMIZED),
                                           keyboard_capture, gamepad_capture});
}
}
