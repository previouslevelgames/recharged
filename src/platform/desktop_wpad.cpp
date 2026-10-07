#include "platform/desktop_wpad.h"
#include "platform/desktop_dpd.h"
#include <revolution/wpad/WPAD.h>
#include <SDL3/SDL.h>

#include <array>
#include <chrono>
#include <cmath>
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
    mscharged::platform::NativeDpdSource dpd_source{};
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
    bool mouse_known{}, mouse_inside{};
    float mouse_x{}, mouse_y{};
    Uint32 mouse_buttons{};
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
    if (device->dpd_source.generation)
        mscharged::platform::DetachNativeWpadDpdSource(device->dpd_source);
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
        state.mouse_known = state.mouse_inside = false;
        state.mouse_buttons = 0;
    } else if ((event->type == SDL_EVENT_KEY_DOWN || event->type == SDL_EVENT_KEY_UP) &&
               event->key.windowID == state.window && unsigned(event->key.scancode) < state.keys.size()) {
        state.keys[event->key.scancode] = event->type == SDL_EVENT_KEY_DOWN && state.focused;
    }
    if (state.settings.mouse) {
        if (event->type == SDL_EVENT_WINDOW_MOUSE_ENTER && event->window.windowID == state.window)
            state.mouse_inside = true;
        else if ((event->type == SDL_EVENT_WINDOW_MOUSE_LEAVE || event->type == SDL_EVENT_WINDOW_DESTROYED) &&
                 event->window.windowID == state.window) {
            state.mouse_known = state.mouse_inside = false;
            state.mouse_buttons = 0;
        } else if (event->type == SDL_EVENT_MOUSE_MOTION && event->motion.windowID == state.window &&
                   event->motion.which != SDL_TOUCH_MOUSEID && event->motion.which != SDL_PEN_MOUSEID) {
            state.mouse_x = event->motion.x;
            state.mouse_y = event->motion.y;
            state.mouse_known = state.focused;
            state.mouse_inside = state.focused;
        } else if ((event->type == SDL_EVENT_MOUSE_BUTTON_DOWN || event->type == SDL_EVENT_MOUSE_BUTTON_UP) &&
                   event->button.windowID == state.window && event->button.which != SDL_TOUCH_MOUSEID &&
                   event->button.which != SDL_PEN_MOUSEID &&
                   (event->button.button == SDL_BUTTON_LEFT || event->button.button == SDL_BUTTON_RIGHT)) {
            state.mouse_x = event->button.x;
            state.mouse_y = event->button.y;
            state.mouse_known = state.focused;
            state.mouse_inside = state.focused;
            const Uint32 bit = SDL_BUTTON_MASK(event->button.button);
            if (event->button.down && state.focused) state.mouse_buttons |= bit;
            else state.mouse_buttons &= ~bit;
        }
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
void Report(Device& device, const std::array<bool, 11>& buttons, Clock::time_point now,
            const mscharged::platform::NativeDpdObservation* observation = nullptr) {
    if (now < device.next_report) return;
    device.next_report = now + ReportPeriod;
    if (device.dpd_source.generation) {
        if (!observation) throw std::logic_error("Mouse camera report lacks its raw observation");
        mscharged::platform::SubmitNativeWpadDpdObservation(device.dpd_source, *observation);
    }
    for (int n = 0; n < int(buttons.size()); ++n)
        Require(SDL_SetJoystickVirtualButton(device.virtual_joystick,
            int(SDL_GAMEPAD_BUTTON_MISC1) + n, buttons[n]), "Publish actual desktop buttons");
    SDL_UpdateJoysticks();
    if (device.sensors_enabled) {
        // Explicit neutral core-Wii gravity in the existing SDL driver's SI
        // coordinate convention. Mouse IR words come from the separately
        // declared raw camera; this gravity does not invent physical motion.
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
    if (settings.mouse && !settings.pointer_projection)
        throw std::invalid_argument("Mouse camera requires a successful-Present content projection");
    {
        std::lock_guard lock(state.mutex);
        if (state.ready) throw std::logic_error("Desktop WPAD transport is already initialized");
        state.owner = std::this_thread::get_id();
        state.window = SDL_GetWindowID(window);
        state.settings = settings;
        state.focused = SDL_GetWindowFlags(window) & SDL_WINDOW_INPUT_FOCUS;
        state.keys.fill(false);
        state.mouse_known = state.mouse_inside = false;
        state.mouse_buttons = 0;
        state.ready = true;
    }
    try {
        if (settings.keyboard || settings.gamepads || settings.mouse) {
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
    try {
        if (settings.keyboard || settings.mouse) state.keyboard = MakeDevice();
        if (settings.mouse)
            state.keyboard->dpd_source = mscharged::platform::AttachNativeWpadDpdSource(state.keyboard->virtual_id);
    }
    catch (...) {
        Retire(state.keyboard);
        SDL_RemoveEventWatch(Watch, nullptr);
        RestoreBackgroundHint();
        std::lock_guard lock(state.mutex);
        state.ready = false;
        throw;
    }
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
    bool focused, mouse_known, mouse_inside;
    float mouse_x, mouse_y;
    Uint32 mouse_buttons;
    std::array<bool, SDL_SCANCODE_COUNT> keys;
    {
        std::lock_guard lock(state.mutex);
        focused = state.focused;
        keys = state.keys;
        mouse_known = state.mouse_known;
        mouse_inside = state.mouse_inside;
        mouse_x = state.mouse_x;
        mouse_y = state.mouse_y;
        mouse_buttons = state.mouse_buttons;
    }
    NativeDpdObservation observation{};
    if (state.settings.mouse) {
        DesktopDpdProjection projection{};
        SDL_Window* window = SDL_GetWindowFromID(state.window);
        const bool available = window && state.settings.pointer_projection(
            state.settings.pointer_projection_context, window, &projection);
        if (available && (!projection.presented_revision || !std::isfinite(projection.left) ||
                          !std::isfinite(projection.top) || !std::isfinite(projection.width) ||
                          !std::isfinite(projection.height) || projection.width <= 0 || projection.height <= 0))
            throw std::invalid_argument("Native mouse projection has no valid presented content domain");
        const bool visible = available && focused && mouse_known && mouse_inside &&
            mouse_x >= projection.left && mouse_x <= projection.left + projection.width &&
            mouse_y >= projection.top && mouse_y <= projection.top + projection.height;
        const float x = visible ? (mouse_x - projection.left) / projection.width : 0;
        const float y = visible ? (mouse_y - projection.top) / projection.height : 0;
        observation = MakeDesktopDpdObservation(x, y, WPADGetSensorBarPosition(), visible);
    }
    const auto now = Clock::now();
    if (state.keyboard) {
        auto buttons = focused && state.settings.keyboard ? KeyboardButtons(keys) : std::array<bool, 11>{};
        // Mouse buttons are independent raw Wii A/B signals; source KPAD and
        // game listeners retain all edge/lock/invalid-pointer decisions.
        if (focused && state.settings.mouse) {
            buttons[0] = buttons[0] || (mouse_buttons & SDL_BUTTON_LMASK);
            buttons[1] = buttons[1] || (mouse_buttons & SDL_BUTTON_RMASK);
        }
        Report(*state.keyboard, buttons, now, state.settings.mouse ? &observation : nullptr);
    }
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
    {
        std::lock_guard lock(state.mutex);
        state.ready = false;
        state.keys.fill(false);
        state.mouse_known = state.mouse_inside = false;
        state.mouse_buttons = 0;
    }
    SDL_RemoveEventWatch(Watch, nullptr);
    Retire(state.keyboard);
    for (auto& pad : state.pads) Retire(pad);
    RestoreBackgroundHint();
    state.window = 0;
    state.owner = {};
}
} // namespace mscharged::platform
