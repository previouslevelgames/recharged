#include "runtime/frontend_input_sdl.h"
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace mscharged
{
namespace
{
float Axis(Sint16 value)
{
    const float axis = value / (value < 0 ? 32768.f : 32767.f);
    return std::abs(axis) <= .18f ? 0 : axis;
}
FrontendPadSample Gate(FrontendPadSample value, bool enabled, bool& blocked, std::uint32_t& previous)
{
    if (!enabled) blocked = true;
    if (blocked)
    {
        if (enabled && !value.buttons && value.left_x == 0 && value.left_y == 0) blocked = false;
        FrontendPadSample suppressed{value.connected, true};
        suppressed.suppressed_buttons = previous; previous = 0;
        return suppressed;
    }
    previous = value.buttons;
    if (value.left_x <= -.5f) previous |= 1;
    if (value.left_x >= .5f) previous |= 2;
    if (value.left_y >= .5f) previous |= 8;
    if (value.left_y <= -.5f) previous |= 4;
    return value;
}
}
std::array<FrontendPadSample, 4> FrontendInputMap::Sample(const FrontendInputDevices& devices, FrontendInputCapture capture)
{
    std::array<FrontendPadSample, 4> result;
    const auto& keys = devices.keys;
    FrontendPadSample keyboard{true};
    if (keys[SDL_SCANCODE_LEFT]) keyboard.buttons |= 1;
    if (keys[SDL_SCANCODE_RIGHT]) keyboard.buttons |= 2;
    if (keys[SDL_SCANCODE_DOWN]) keyboard.buttons |= 4;
    if (keys[SDL_SCANCODE_UP]) keyboard.buttons |= 8;
    if (keys[SDL_SCANCODE_RETURN] || keys[SDL_SCANCODE_SPACE]) keyboard.buttons |= 0x100;
    if (keys[SDL_SCANCODE_ESCAPE] || keys[SDL_SCANCODE_BACKSPACE]) keyboard.buttons |= 0x200;
    if (keys[SDL_SCANCODE_TAB]) keyboard.buttons |= 0x1000;
    keyboard = Gate(keyboard, capture.focused && !capture.keyboard, keyboard_blocked_, keyboard_previous_);
    for (unsigned i = 0; i < 4; ++i)
    {
        const auto& input = devices.pads[i];
        if (input.id != ids_[i]) { ids_[i] = input.id; pad_blocked_[i] = true; }
        FrontendPadSample sample{input.id != 0};
        if (input.buttons[SDL_GAMEPAD_BUTTON_DPAD_LEFT]) sample.buttons |= 1;
        if (input.buttons[SDL_GAMEPAD_BUTTON_DPAD_RIGHT]) sample.buttons |= 2;
        if (input.buttons[SDL_GAMEPAD_BUTTON_DPAD_DOWN]) sample.buttons |= 4;
        if (input.buttons[SDL_GAMEPAD_BUTTON_DPAD_UP]) sample.buttons |= 8;
        if (input.buttons[SDL_GAMEPAD_BUTTON_SOUTH]) sample.buttons |= 0x100;
        if (input.buttons[SDL_GAMEPAD_BUTTON_EAST]) sample.buttons |= 0x200;
        if (input.buttons[SDL_GAMEPAD_BUTTON_START]) sample.buttons |= 0x1000;
        sample.left_x = Axis(input.left_x); sample.left_y = -Axis(input.left_y);
        sample = Gate(sample, capture.focused && !capture.gamepad && input.id != 0, pad_blocked_[i], pad_previous_[i]);
        result[i] = sample;
    }
    // Keyboard shares player one. Suppress all edges only when neither source
    // is usable; capturing one device must not disable the other device.
    result[0].connected = true;
    result[0].buttons |= keyboard.buttons;
    result[0].suppress_edges &= keyboard.suppress_edges;
    result[0].suppressed_buttons |= keyboard.suppressed_buttons;
    return result;
}
void FrontendInputMap::Update(FrontendInput& input, const FrontendInputDevices& devices,
    FrontendInputCapture capture, float delta)
{
    auto next = *this;
    input.Update(next.Sample(devices, capture), delta);
    *this = next;
}
FrontendInputSDL::~FrontendInputSDL()
{
    if (thread_ != std::this_thread::get_id()) std::terminate();
    for (auto* pad : pads_) if (pad) SDL_CloseGamepad(pad);
}
FrontendInputDevices FrontendInputSDL::ReadDevices()
{
    if (thread_ != std::this_thread::get_id()) throw std::logic_error("SDL frontend input requires its owner thread");
    constexpr auto required = SDL_INIT_VIDEO | SDL_INIT_GAMEPAD;
    if ((SDL_WasInit(required) & required) != required)
        throw std::logic_error("SDL frontend input requires initialized video and gamepad services");
    for (auto*& pad : pads_)
        if (pad && !SDL_GamepadConnected(pad)) { SDL_CloseGamepad(pad); pad = nullptr; }
    int count = 0;
    auto* ids = SDL_GetGamepads(&count);
    for (int i = 0; i < count; ++i)
    {
        if (std::any_of(pads_.begin(), pads_.end(), [&](auto* pad) { return pad && SDL_GetGamepadID(pad) == ids[i]; })) continue;
        const auto slot = std::find(pads_.begin(), pads_.end(), nullptr);
        if (slot == pads_.end()) break;
        *slot = SDL_OpenGamepad(ids[i]);
    }
    SDL_free(ids);
    FrontendInputDevices state;
    const bool* keys = SDL_GetKeyboardState(&count);
    if (keys) std::copy_n(keys, std::min<std::size_t>(count, state.keys.size()), state.keys.begin());
    for (unsigned i = 0; i < 4; ++i)
        if (pads_[i])
        {
            auto& sample = state.pads[i]; sample.id = SDL_GetGamepadID(pads_[i]);
            for (unsigned b = 0; b < sample.buttons.size(); ++b) sample.buttons[b] = SDL_GetGamepadButton(pads_[i], SDL_GamepadButton(b));
            sample.left_x = SDL_GetGamepadAxis(pads_[i], SDL_GAMEPAD_AXIS_LEFTX);
            sample.left_y = SDL_GetGamepadAxis(pads_[i], SDL_GAMEPAD_AXIS_LEFTY);
        }
    return state;
}
void FrontendInputSDL::Poll(FrontendInput& input, SDL_Window* window, float delta, bool keyboard, bool gamepad)
{
    if (!window) throw std::invalid_argument("Frontend input needs its window");
    const auto flags = SDL_GetWindowFlags(window);
    const bool focused = (flags & SDL_WINDOW_INPUT_FOCUS) && !(flags & (SDL_WINDOW_HIDDEN | SDL_WINDOW_MINIMIZED));
    mapping_.Update(input, ReadDevices(), {focused, keyboard, gamepad}, delta);
}
}
