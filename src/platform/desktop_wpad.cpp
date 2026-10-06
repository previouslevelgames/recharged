#include "platform/desktop_wpad.h"
#include <SDL3/SDL.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>

namespace {
using Clock = std::chrono::steady_clock;
constexpr auto ReportPeriod = std::chrono::milliseconds(10);
constexpr std::array<SDL_GamepadButton, 11> DesktopButtons{
    SDL_GAMEPAD_BUTTON_SOUTH, SDL_GAMEPAD_BUTTON_EAST,
    SDL_GAMEPAD_BUTTON_WEST, SDL_GAMEPAD_BUTTON_NORTH,
    SDL_GAMEPAD_BUTTON_START, SDL_GAMEPAD_BUTTON_BACK,
    SDL_GAMEPAD_BUTTON_GUIDE, SDL_GAMEPAD_BUTTON_DPAD_UP,
    SDL_GAMEPAD_BUTTON_DPAD_DOWN, SDL_GAMEPAD_BUTTON_DPAD_LEFT,
    SDL_GAMEPAD_BUTTON_DPAD_RIGHT};
struct Device {
    SDL_JoystickID input_id{}, virtual_id{};
    SDL_Gamepad* input{};
    SDL_Joystick* virtual_joystick{};
    std::thread::id owner;
    bool sensors_enabled{};
    Clock::time_point next_report{};
};
struct State {
    std::mutex mutex;
    std::thread::id owner;
    SDL_WindowID window{};
    mscharged::platform::DesktopWpadSettings settings{};
    std::array<bool, SDL_SCANCODE_COUNT> keys{};
    std::unique_ptr<Device> keyboard;
    std::array<std::unique_ptr<Device>, 4> pads;
    bool ready{}, focused{};
    bool owns_background_hint{}, had_background_hint{};
    std::string background_hint;
};
State& Get() { static State state; return state; }
void Require(bool result, const char* operation);
void RestoreBackgroundHint() {
    auto& state = Get();
    if (!state.owns_background_hint) return;
    // Raw SDL delivery must remain available to publish a zero-button report
    // when this explicit desktop profile loses window focus. Game input and
    // repeat/edge decisions still come from the original WPAD/KPAD/game code.
    if (state.had_background_hint)
        Require(SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS,
            state.background_hint.c_str()), "Restore previous SDL raw-device delivery policy");
    else Require(SDL_ResetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS),
        "Restore default SDL raw-device delivery policy");
    state.owns_background_hint = false;
    state.background_hint.clear();
}
void RequireOwner() {
    auto& state = Get();
    if (!state.ready || state.owner != std::this_thread::get_id())
        throw std::logic_error("Desktop WPAD transport requires its initialized SDL owner");
}
void Require(bool result, const char* operation) {
    if (!result) throw std::runtime_error(std::string(operation) + ": " + SDL_GetError());
}
bool SDLCALL Sensors(void* opaque, bool enabled) {
    auto& device = *static_cast<Device*>(opaque);
    if (device.owner != std::this_thread::get_id()) return false;
    device.sensors_enabled = enabled;
    return true;
}
bool SDLCALL Rumble(void* opaque, Uint16 low, Uint16 high) {
    auto& device = *static_cast<Device*>(opaque);
    if (device.owner != std::this_thread::get_id()) return false;
    // The explicit virtual keyboard has no physical motor. Digital desktop
    // controllers forward motor requests only where that device has a motor;
    // this profile does not claim physical haptics on keyboard/unsupported pads.
    if (device.input && SDL_GetBooleanProperty(SDL_GetGamepadProperties(device.input),
            SDL_PROP_GAMEPAD_CAP_RUMBLE_BOOLEAN, false))
        return SDL_RumbleGamepad(device.input, low, high, (low || high) ? 0xffffffffu : 0);
    return true;
}
std::unique_ptr<Device> MakeDevice(SDL_Gamepad* input = nullptr) {
    auto result = std::make_unique<Device>();
    result->owner = std::this_thread::get_id();
    result->input = input;
    if (input) result->input_id = SDL_GetGamepadID(input);
    const SDL_VirtualJoystickSensorDesc sensor{SDL_SENSOR_ACCEL, 100.0f};
    SDL_VirtualJoystickDesc descriptor;
    SDL_INIT_INTERFACE(&descriptor);
    descriptor.type = SDL_JOYSTICK_TYPE_GAMEPAD;
    descriptor.vendor_id = 0x057e;
    descriptor.product_id = 0x0306;
    descriptor.naxes = SDL_GAMEPAD_AXIS_COUNT;
    descriptor.nbuttons = int(SDL_GAMEPAD_BUTTON_MISC1) + 11;
    descriptor.button_mask = (1u << SDL_GAMEPAD_BUTTON_COUNT) - 1;
    descriptor.axis_mask = (1u << SDL_GAMEPAD_AXIS_COUNT) - 1;
    // Required raw core-Wii report identity at the existing WPAD transport.
    // The native host/settings identify this as desktop emulation, not a
    // discovered physical Nintendo remote or successfully available motion.
    descriptor.name = "Nintendo Wii Remote";
    descriptor.nsensors = 1;
    descriptor.sensors = &sensor;
    descriptor.userdata = result.get();
    descriptor.SetSensorsEnabled = Sensors;
    descriptor.Rumble = Rumble;
    result->virtual_id = SDL_AttachVirtualJoystick(&descriptor);
    Require(result->virtual_id != 0, "Create explicit desktop virtual core-Wii device");
    result->virtual_joystick = SDL_OpenJoystick(result->virtual_id);
    if (!result->virtual_joystick) {
        SDL_DetachVirtualJoystick(result->virtual_id);
        throw std::runtime_error(SDL_GetError());
    }
    return result;
}
void Retire(std::unique_ptr<Device>& device) {
    if (!device) return;
    // Detach the virtual driver before its userdata or borrowed actual pad can
    // retire. Existing WPAD observes a real SDL detach at its next owner service.
    const auto id = device->virtual_id;
    SDL_CloseJoystick(device->virtual_joystick);
    device->virtual_joystick = nullptr;
    Require(SDL_DetachVirtualJoystick(id), "Retire desktop virtual core-Wii device");
    if (device->input) SDL_CloseGamepad(device->input);
    device.reset();
}
bool SDLCALL Watch(void*, SDL_Event* event) {
    auto& state = Get();
    std::lock_guard lock(state.mutex);
    if (!state.ready) return true;
    if (event->type == SDL_EVENT_WINDOW_FOCUS_GAINED && event->window.windowID == state.window)
        state.focused = true;
    else if ((event->type == SDL_EVENT_WINDOW_FOCUS_LOST || event->type == SDL_EVENT_WINDOW_HIDDEN ||
              event->type == SDL_EVENT_WINDOW_MINIMIZED) && event->window.windowID == state.window) {
        state.focused = false;
        state.keys.fill(false);
    } else if ((event->type == SDL_EVENT_KEY_DOWN || event->type == SDL_EVENT_KEY_UP) &&
               event->key.windowID == state.window && unsigned(event->key.scancode) < state.keys.size()) {
        state.keys[event->key.scancode] = event->type == SDL_EVENT_KEY_DOWN && state.focused;
    }
    // Workers latch raw device intent only. No game callbacks/managers run here.
    return true;
}
std::array<bool, 11> KeyboardButtons(const std::array<bool, SDL_SCANCODE_COUNT>& keys) {
    return {keys[SDL_SCANCODE_RETURN] || keys[SDL_SCANCODE_SPACE],
        keys[SDL_SCANCODE_ESCAPE] || keys[SDL_SCANCODE_BACKSPACE],
        keys[SDL_SCANCODE_Z], keys[SDL_SCANCODE_X], keys[SDL_SCANCODE_TAB],
        keys[SDL_SCANCODE_MINUS], keys[SDL_SCANCODE_HOME],
        keys[SDL_SCANCODE_UP], keys[SDL_SCANCODE_DOWN], keys[SDL_SCANCODE_LEFT], keys[SDL_SCANCODE_RIGHT]};
}
void Report(Device& device, const std::array<bool, 11>& buttons, Clock::time_point now) {
    if (now < device.next_report) return;
    device.next_report = now + ReportPeriod;
    for (int n = 0; n < int(buttons.size()); ++n)
        Require(SDL_SetJoystickVirtualButton(device.virtual_joystick,
            int(SDL_GAMEPAD_BUTTON_MISC1) + n, buttons[n]), "Publish actual desktop buttons");
    SDL_UpdateJoysticks();
    if (device.sensors_enabled) {
        // Explicit neutral core-Wii gravity in the existing SDL driver's SI
        // coordinate convention. There is no invented desktop motion or DPD.
        const float gravity[3]{0.0f, 9.80665f, 0.0f};
        Require(SDL_SendJoystickVirtualSensorData(device.virtual_joystick,
            SDL_SENSOR_ACCEL, SDL_GetTicksNS(), gravity, 3), "Publish desktop core-Wii raw report");
    }
}
} // namespace

namespace mscharged::platform {
void InitializeDesktopWpad(SDL_Window* window, DesktopWpadSettings settings) {
    auto& state = Get();
    if (!window || !SDL_GetWindowID(window))
        throw std::invalid_argument("Desktop WPAD transport requires the actual host window");
    {
        std::lock_guard lock(state.mutex);
        if (state.ready) throw std::logic_error("Desktop WPAD transport is already initialized");
        state.owner = std::this_thread::get_id();
        state.window = SDL_GetWindowID(window);
        state.settings = settings;
        state.focused = SDL_GetWindowFlags(window) & SDL_WINDOW_INPUT_FOCUS;
        state.keys.fill(false);
        state.ready = true;
    }
    try {
        if (settings.keyboard || settings.gamepads) {
            const auto* previous = SDL_GetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS);
            state.had_background_hint = previous != nullptr;
            state.background_hint = previous ? previous : "";
            Require(SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1"),
                "Enable explicit desktop raw-report delivery across focus changes");
            state.owns_background_hint = true;
        }
    } catch (...) {
        std::lock_guard lock(state.mutex); state.ready = false; throw;
    }
    if (!SDL_AddEventWatch(Watch, nullptr)) {
        RestoreBackgroundHint();
        std::lock_guard lock(state.mutex);
        state.ready = false;
        throw std::runtime_error(SDL_GetError());
    }
    try { if (settings.keyboard) state.keyboard = MakeDevice(); }
    catch (...) { SDL_RemoveEventWatch(Watch, nullptr); RestoreBackgroundHint(); std::lock_guard lock(state.mutex); state.ready = false; throw; }
}
void ServiceDesktopWpad() {
    RequireOwner();
    auto& state = Get();
    SDL_UpdateGamepads();
    for (auto& pad : state.pads) if (pad && !SDL_GamepadConnected(pad->input)) Retire(pad);
    if (state.settings.gamepads) {
        int count = 0;
        SDL_JoystickID* ids = SDL_GetGamepads(&count);
        if (!ids) throw std::runtime_error(SDL_GetError());
        struct IDs { SDL_JoystickID* p; ~IDs() { SDL_free(p); } } owned{ids};
        for (int n = 0; n < count; ++n) {
            bool known = state.keyboard && ids[n] == state.keyboard->virtual_id;
            for (const auto& pad : state.pads) if (pad)
                known |= ids[n] == pad->input_id || ids[n] == pad->virtual_id;
            if (known) continue;
            // Actual physical Wii/Nunchuk/Classic identities stay on their
            // independent WPAD provider/extension HOLD; never silently remap
            // a physical Wii extension into a button-only desktop controller.
            const auto vendor = SDL_GetGamepadVendorForID(ids[n]), product = SDL_GetGamepadProductForID(ids[n]);
            if (vendor == 0x057e && (product == 0x0306 || product == 0x0330)) continue;
            auto* input = SDL_OpenGamepad(ids[n]);
            if (!input) continue;
            std::unique_ptr<Device>* slot = nullptr;
            for (auto& pad : state.pads) if (!pad) { slot = &pad; break; }
            if (!slot) { SDL_CloseGamepad(input); break; }
            try { *slot = MakeDevice(input); }
            catch (...) { SDL_CloseGamepad(input); throw; }
        }
    }
    bool focused;
    std::array<bool, SDL_SCANCODE_COUNT> keys;
    { std::lock_guard lock(state.mutex); focused = state.focused; keys = state.keys; }
    const auto now = Clock::now();
    if (state.keyboard) Report(*state.keyboard, focused ? KeyboardButtons(keys) : std::array<bool, 11>{}, now);
    for (auto& pad : state.pads) if (pad) {
        std::array<bool, 11> buttons{};
        if (focused) for (int n = 0; n < int(buttons.size()); ++n)
            buttons[n] = SDL_GetGamepadButton(pad->input, DesktopButtons[n]);
        Report(*pad, buttons, now);
    }
}
void ShutdownDesktopWpad() {
    auto& state = Get();
    if (!state.ready) return;
    RequireOwner();
    { std::lock_guard lock(state.mutex); state.ready = false; state.keys.fill(false); }
    SDL_RemoveEventWatch(Watch, nullptr);
    Retire(state.keyboard);
    for (auto& pad : state.pads) Retire(pad);
    RestoreBackgroundHint();
    state.window = 0;
    state.owner = {};
}
} // namespace mscharged::platform
