#include "platform/desktop_wpad.h"
#include "platform/desktop_dpd.h"
#include "platform/desktop_nunchuk.h"
#include "platform/wpad_sdl.h"
#include <revolution/wpad/WPAD.h>
#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <bitset>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>

namespace {
using Clock = std::chrono::steady_clock;
constexpr auto ReportPeriod = std::chrono::milliseconds(10);
// A keyboard shake is one flick: +2.5 g then -2.5 g along X for six reports
// each (60 ms at the report period), then rest. The original cAIPad smooths the
// accelerometer history and compares five-sample deltas with 1.33 g (Remote)
// and 2.5 g (Nunchuk); the flick clears both, within the sensors' 10-bit
// ranges. The game decides what a shake does.
constexpr int ShakeHalfReports = 6;
constexpr float ShakeG = 2.5f;
constexpr float kGravityMs2 = 9.80665f;
enum Shake { RemoteShake, NunchukShake, ShakeCount };
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
    mscharged::platform::NativeNunchukSource nunchuk_source{};
    std::array<int, ShakeCount> shake_reports{};
    std::array<bool, ShakeCount> shaking{};
    // Gamepad player: right-stick pointer position (0..1 of the picture) and
    // the shake buttons' previous state, for press edges.
    float pointer_x = 0.5f, pointer_y = 0.5f;
    std::array<bool, ShakeCount> shake_held{};
    Clock::time_point last_service{};
};
struct State {
    std::mutex mutex;
    std::thread::id owner;
    SDL_WindowID window{};
    mscharged::platform::DesktopWpadSettings settings{};
    std::array<bool, SDL_SCANCODE_COUNT> keys{};
    // Shake presses latched until the next service, so a quick tap still
    // produces one complete flick.
    std::array<unsigned, ShakeCount> shake_requests{};
    std::map<SDL_KeyboardID, std::bitset<SDL_SCANCODE_COUNT>> keyboard_keys;
    std::map<SDL_MouseID, Uint32> mouse_button_sources;
    SDL_MouseID mouse_position_source{};
    bool event_memory_failed{};
    std::unique_ptr<Device> keyboard;
    std::array<std::unique_ptr<Device>, 4> pads;
    bool ready{}, focused{};
    bool mouse_known{}, mouse_inside{};
    float mouse_x{}, mouse_y{};
    Uint32 mouse_buttons{};
    Clock::time_point mouse_used{};
    bool owns_background_hint{}, had_background_hint{};
    std::string background_hint;
};
State& Get() { static State state; return state; }
void Require(bool result, const char* operation);
void ClearKeyboard(State& state) {
    state.keyboard_keys.clear();
    state.keys.fill(false);
}
void ClearMouse(State& state) {
    state.mouse_button_sources.clear();
    state.mouse_buttons = 0;
    state.mouse_known = state.mouse_inside = false;
    state.mouse_position_source = 0;
}
void RebuildKeys(State& state) {
    state.keys.fill(false);
    for (const auto& source : state.keyboard_keys)
        for (std::size_t n = 0; n < state.keys.size(); ++n)
            state.keys[n] = state.keys[n] || source.second[n];
}
void RebuildMouseButtons(State& state) {
    state.mouse_buttons = 0;
    for (const auto& source : state.mouse_button_sources)
        state.mouse_buttons |= source.second;
}
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
    if (device->nunchuk_source.generation)
        mscharged::platform::DetachNativeWpadNunchukSource(device->nunchuk_source);
    const auto id = device->virtual_id;
    mscharged::platform::SetNativeWpadFixedChannel(id, -1);
    SDL_CloseJoystick(device->virtual_joystick);
    device->virtual_joystick = nullptr;
    Require(SDL_DetachVirtualJoystick(id), "Retire desktop virtual core-Wii device");
    if (device->input) SDL_CloseGamepad(device->input);
    device.reset();
}
// The keyboard & mouse remote and its observation producers.
void AttachKeyboard(State& state) {
    state.keyboard = MakeDevice();
    try {
        if (state.settings.mouse)
            state.keyboard->dpd_source = mscharged::platform::AttachNativeWpadDpdSource(state.keyboard->virtual_id);
        if (state.settings.nunchuk)
            state.keyboard->nunchuk_source =
                mscharged::platform::AttachNativeWpadNunchukSource(state.keyboard->virtual_id);
        if (state.settings.keyboard_channel >= 0)
            mscharged::platform::SetNativeWpadFixedChannel(state.keyboard->virtual_id, state.settings.keyboard_channel);
    } catch (...) {
        Retire(state.keyboard);
        throw;
    }
}
bool SDLCALL Watch(void*, SDL_Event* event) {
    auto& state = Get();
    std::lock_guard lock(state.mutex);
    if (!state.ready) return true;
    try {
        if (event->type == SDL_EVENT_WINDOW_FOCUS_GAINED && event->window.windowID == state.window)
            state.focused = true;
        else if ((event->type == SDL_EVENT_WINDOW_FOCUS_LOST || event->type == SDL_EVENT_WINDOW_HIDDEN ||
                  event->type == SDL_EVENT_WINDOW_MINIMIZED) && event->window.windowID == state.window) {
            state.focused = false;
            ClearKeyboard(state);
            ClearMouse(state);
        } else if (event->type == SDL_EVENT_KEYBOARD_REMOVED) {
            // SDL removal does not guarantee a final key-up or focus loss.
            // Retire only this actual input instance, not the virtual remote.
            state.keyboard_keys.erase(event->kdevice.which);
            RebuildKeys(state);
        } else if ((event->type == SDL_EVENT_KEY_DOWN || event->type == SDL_EVENT_KEY_UP) &&
                   event->key.windowID == state.window && unsigned(event->key.scancode) < state.keys.size()) {
            const auto scancode = std::size_t(event->key.scancode);
            if (event->type == SDL_EVENT_KEY_DOWN && state.focused) {
                state.keyboard_keys.try_emplace(event->key.which).first->second.set(scancode);
                state.keys[scancode] = true;
                const auto bound = [&](std::size_t action) {
                    for (const auto code : state.settings.keys[action])
                        if (code != SDL_SCANCODE_UNKNOWN && code == event->key.scancode) return true;
                    return false;
                };
                if (!event->key.repeat && bound(mscharged::KeyActionShakeRemote)) ++state.shake_requests[RemoteShake];
                if (!event->key.repeat && bound(mscharged::KeyActionShakeNunchuk)) ++state.shake_requests[NunchukShake];
            } else {
                auto source = state.keyboard_keys.find(event->key.which);
                if (source != state.keyboard_keys.end()) {
                    source->second.reset(scancode);
                    if (source->second.none()) state.keyboard_keys.erase(source);
                }
                state.keys[scancode] = false;
                for (const auto& keys : state.keyboard_keys)
                    state.keys[scancode] = state.keys[scancode] || keys.second[scancode];
            }
        }
        if (state.settings.mouse) {
            if (event->type == SDL_EVENT_MOUSE_REMOVED) {
                state.mouse_button_sources.erase(event->mdevice.which);
                RebuildMouseButtons(state);
                if (state.mouse_known && state.mouse_position_source == event->mdevice.which) {
                    state.mouse_known = state.mouse_inside = false;
                    state.mouse_position_source = 0;
                }
            } else if (event->type == SDL_EVENT_WINDOW_MOUSE_ENTER && event->window.windowID == state.window)
                state.mouse_inside = true;
            else if ((event->type == SDL_EVENT_WINDOW_MOUSE_LEAVE || event->type == SDL_EVENT_WINDOW_DESTROYED) &&
                     event->window.windowID == state.window) {
                ClearMouse(state);
            } else if (event->type == SDL_EVENT_MOUSE_MOTION && event->motion.windowID == state.window &&
                       event->motion.which != SDL_TOUCH_MOUSEID && event->motion.which != SDL_PEN_MOUSEID) {
                state.mouse_x = event->motion.x;
                state.mouse_y = event->motion.y;
                state.mouse_used = Clock::now();
                state.mouse_position_source = event->motion.which;
                state.mouse_known = state.focused;
                state.mouse_inside = state.focused;
            } else if ((event->type == SDL_EVENT_MOUSE_BUTTON_DOWN || event->type == SDL_EVENT_MOUSE_BUTTON_UP) &&
                       event->button.windowID == state.window && event->button.which != SDL_TOUCH_MOUSEID &&
                       event->button.which != SDL_PEN_MOUSEID &&
                       (event->button.button == SDL_BUTTON_LEFT || event->button.button == SDL_BUTTON_RIGHT)) {
                const Uint32 bit = SDL_BUTTON_MASK(event->button.button);
                if (event->button.down && state.focused)
                    state.mouse_button_sources.try_emplace(event->button.which, 0).first->second |= bit;
                else {
                    auto source = state.mouse_button_sources.find(event->button.which);
                    if (source != state.mouse_button_sources.end()) {
                        source->second &= ~bit;
                        if (!source->second) state.mouse_button_sources.erase(source);
                    }
                }
                RebuildMouseButtons(state);
                state.mouse_used = Clock::now();
                state.mouse_x = event->button.x;
                state.mouse_y = event->button.y;
                state.mouse_position_source = event->button.which;
                state.mouse_known = state.focused;
                state.mouse_inside = state.focused;
            }
        }
    } catch (const std::bad_alloc&) {
        // Metadata allocation failure cannot escape an SDL worker's watcher.
        // The initialized native owner reports the failure before raw delivery.
        state.event_memory_failed = true;
    }
    // Workers latch raw device intent only. No game callbacks/managers run here.
    return true;
}
// Whether any key of a keyboard action is held.
bool Held(const std::array<bool, SDL_SCANCODE_COUNT>& keys, const mscharged::platform::KeyBindings& bindings,
          std::size_t action) {
    for (const auto code : bindings[action])
        if (code != SDL_SCANCODE_UNKNOWN && keys[code]) return true;
    return false;
}
std::array<bool, 11> KeyboardButtons(const std::array<bool, SDL_SCANCODE_COUNT>& keys,
                                     const mscharged::platform::KeyBindings& bindings) {
    // WPAD order: A, B, 1, 2, +, -, HOME, up, down, left, right.
    std::array<bool, 11> buttons{};
    for (std::size_t n = 0; n < buttons.size(); ++n) buttons[n] = Held(keys, bindings, mscharged::KeyActionA + n);
    return buttons;
}
mscharged::platform::NativeNunchukObservation KeyboardNunchuk(const std::array<bool, SDL_SCANCODE_COUNT>& keys,
                                                              const mscharged::platform::KeyBindings& bindings) {
    // Full deflection of a physical stick after WPAD centre calibration is
    // about 100 counts; diagonals stay on the same circular gate. Original
    // ClampWiiStick and KPAD apply their own dead zones and normalisation.
    const int x = int(Held(keys, bindings, mscharged::KeyActionStickRight)) -
                  int(Held(keys, bindings, mscharged::KeyActionStickLeft));
    const int y = int(Held(keys, bindings, mscharged::KeyActionStickUp)) -
                  int(Held(keys, bindings, mscharged::KeyActionStickDown));
    const int reach = x && y ? 71 : 100;
    mscharged::platform::NativeNunchukObservation result{};
    result.stick_x = static_cast<std::int8_t>(x * reach);
    result.stick_y = static_cast<std::int8_t>(y * reach);
    result.c = Held(keys, bindings, mscharged::KeyActionC);
    result.z = Held(keys, bindings, mscharged::KeyActionZ);
    result.acc_z = mscharged::platform::kNativeNunchukGravity;
    return result;
}
// Flick acceleration (in g) for this report of a shake on the device.
float NextShake(Device& device, Shake shake) {
    if (!device.shaking[shake]) return 0.0f;
    const int report = device.shake_reports[shake]++;
    if (report < ShakeHalfReports) return ShakeG;
    if (report < 2 * ShakeHalfReports) return -ShakeG;
    device.shaking[shake] = false;
    return 0.0f;
}
void Report(Device& device, const std::array<bool, 11>& buttons, Clock::time_point now,
            const mscharged::platform::NativeDpdObservation* observation = nullptr,
            const mscharged::platform::NativeNunchukObservation* nunchuk = nullptr) {
    if (now < device.next_report) return;
    device.next_report = now + ReportPeriod;
    const float remote_shake_g = NextShake(device, RemoteShake);
    const float nunchuk_shake_g = NextShake(device, NunchukShake);
    if (device.dpd_source.generation) {
        if (!observation) throw std::logic_error("Mouse camera report lacks its raw observation");
        mscharged::platform::SubmitNativeWpadDpdObservation(device.dpd_source, *observation);
    }
    if (device.nunchuk_source.generation) {
        if (!nunchuk) throw std::logic_error("Nunchuk report lacks its raw observation");
        auto reported = *nunchuk;
        reported.acc_x = static_cast<std::int16_t>(
            std::lround(nunchuk_shake_g * mscharged::platform::kNativeNunchukGravity));
        mscharged::platform::SubmitNativeWpadNunchukObservation(device.nunchuk_source, reported);
    }
    for (int n = 0; n < int(buttons.size()); ++n)
        Require(SDL_SetJoystickVirtualButton(device.virtual_joystick,
            int(SDL_GAMEPAD_BUTTON_MISC1) + n, buttons[n]), "Publish actual desktop buttons");
    SDL_UpdateJoysticks();
    if (device.sensors_enabled) {
        // Explicit neutral core-Wii gravity in the existing SDL driver's SI
        // coordinate convention. Mouse IR words come from the separately
        // declared raw camera; only a requested shake key adds an X flick.
        const float gravity[3]{remote_shake_g * kGravityMs2, kGravityMs2, 0.0f};
        Require(SDL_SendJoystickVirtualSensorData(device.virtual_joystick,
            SDL_SENSOR_ACCEL, SDL_GetTicksNS(), gravity, 3), "Publish desktop core-Wii raw report");
    }
}
// Gamepad players: the pad is a Wii Remote with Nunchuk (see desktop_wpad.h).
void AttachGamepadPlayer(State& state, Device& pad, int index) {
    pad.nunchuk_source = mscharged::platform::AttachNativeWpadNunchukSource(pad.virtual_id);
    if (state.settings.pointer_projection)
        pad.dpd_source = mscharged::platform::AttachNativeWpadDpdSource(pad.virtual_id);
    mscharged::platform::SetNativeWpadFixedChannel(pad.virtual_id, state.settings.gamepad_channels[index]);
}
// A stick axis in -1..1 outside a small dead zone, rescaled to the full range.
float StickAxis(SDL_Gamepad* pad, SDL_GamepadAxis axis) {
    constexpr float dead = 0.15f;
    const float value = std::clamp(float(SDL_GetGamepadAxis(pad, axis)) / 32767.0f, -1.0f, 1.0f);
    if (std::fabs(value) < dead) return 0.0f;
    return std::copysign((std::fabs(value) - dead) / (1.0f - dead), value);
}
bool TriggerHeld(SDL_Gamepad* pad, SDL_GamepadAxis axis) { return SDL_GetGamepadAxis(pad, axis) > 16384; }
mscharged::platform::NativeNunchukObservation GamepadNunchuk(Device& pad, bool focused) {
    // Left stick on the physical Nunchuk's range (about 100 counts from the
    // calibrated centre); original ClampWiiStick/KPAD apply their own dead
    // zone. Left trigger = Z, left bumper = C.
    mscharged::platform::NativeNunchukObservation result{};
    result.acc_z = mscharged::platform::kNativeNunchukGravity;
    if (!focused) return result;
    float x = StickAxis(pad.input, SDL_GAMEPAD_AXIS_LEFTX), y = -StickAxis(pad.input, SDL_GAMEPAD_AXIS_LEFTY);
    const float length = std::hypot(x, y);
    if (length > 1.0f) { x /= length; y /= length; }
    result.stick_x = static_cast<std::int8_t>(std::lround(x * 100.0f));
    result.stick_y = static_cast<std::int8_t>(std::lround(y * 100.0f));
    result.z = TriggerHeld(pad.input, SDL_GAMEPAD_AXIS_LEFT_TRIGGER);
    result.c = SDL_GetGamepadButton(pad.input, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER);
    return result;
}
void UpdateGamepadShakes(Device& pad, bool focused) {
    // Right trigger shakes the Remote, right bumper the Nunchuk: one flick per press.
    const std::array<bool, ShakeCount> held{
        focused && TriggerHeld(pad.input, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER),
        focused && SDL_GetGamepadButton(pad.input, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER)};
    for (int shake = 0; shake != ShakeCount; ++shake) {
        if (held[shake] && !pad.shake_held[shake]) {
            pad.shake_reports[shake] = 0;
            pad.shaking[shake] = true;
        } else if (!focused) pad.shaking[shake] = false;
        pad.shake_held[shake] = held[shake];
    }
}
mscharged::platform::NativeDpdObservation GamepadPointer(State& state, Device& pad, bool focused, Clock::time_point now) {
    // The right stick moves the pointer across the picture: full deflection
    // crosses its width in about 0.8 s. It stays where it was left.
    const float seconds = pad.last_service == Clock::time_point{} ? 0.0f
        : std::min(0.1f, std::chrono::duration<float>(now - pad.last_service).count());
    pad.last_service = now;
    if (focused) {
        constexpr float speed = 1.25f;
        pad.pointer_x = std::clamp(pad.pointer_x + StickAxis(pad.input, SDL_GAMEPAD_AXIS_RIGHTX) * speed * seconds, 0.0f, 1.0f);
        pad.pointer_y = std::clamp(pad.pointer_y + StickAxis(pad.input, SDL_GAMEPAD_AXIS_RIGHTY) * speed * seconds, 0.0f, 1.0f);
    }
    return mscharged::platform::MakeDesktopDpdObservation(pad.pointer_x, pad.pointer_y, WPADGetSensorBarPosition(),
                                                          focused && state.ready);
}
} // namespace

namespace mscharged::platform {
void InitializeDesktopWpad(SDL_Window* window, DesktopWpadSettings settings) {
    auto& state = Get();
    if (!window || !SDL_GetWindowID(window))
        throw std::invalid_argument("Desktop WPAD transport requires the actual host window");
    if (settings.mouse && !settings.pointer_projection)
        throw std::invalid_argument("Mouse camera requires a successful-Present content projection");
    if (settings.nunchuk && !settings.keyboard)
        throw std::invalid_argument("The desktop Nunchuk requires the keyboard profile");
    if (settings.share_mouse_with_remotes && !settings.mouse)
        throw std::invalid_argument("Sharing the mouse camera requires the mouse profile");
    if (settings.mouse_player_channel < -1 || settings.mouse_player_channel >= WPAD_MAX_CONTROLLERS ||
        (settings.mouse_player_channel >= 0 && (!settings.mouse || settings.keyboard)))
        throw std::invalid_argument("The mouse joins a controller player only without keyboard & mouse playing");
    if (settings.keyboard_channel < -1 || settings.keyboard_channel >= WPAD_MAX_CONTROLLERS)
        throw std::invalid_argument("Keyboard & mouse player outside Wii hardware ports");
    for (const int channel : settings.gamepad_channels)
        if (channel < -1 || channel >= WPAD_MAX_CONTROLLERS)
            throw std::invalid_argument("Gamepad player outside Wii hardware ports");
    {
        std::lock_guard lock(state.mutex);
        if (state.ready) throw std::logic_error("Desktop WPAD transport is already initialized");
        state.owner = std::this_thread::get_id();
        state.window = SDL_GetWindowID(window);
        state.settings = settings;
        state.focused = SDL_GetWindowFlags(window) & SDL_WINDOW_INPUT_FOCUS;
        ClearKeyboard(state);
        ClearMouse(state);
        state.event_memory_failed = false;
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
        if (settings.keyboard || (settings.mouse && settings.mouse_player_channel < 0)) AttachKeyboard(state);
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
            int index = 0;
            for (; index < int(state.pads.size()); ++index)
                if (!state.pads[index] && (!state.settings.gamepad_players || state.settings.gamepad_channels[index] >= 0)) {
                    slot = &state.pads[index];
                    break;
                }
            if (!slot) { SDL_CloseGamepad(input); break; }
            try {
                *slot = MakeDevice(input);
                if (state.settings.gamepad_players) AttachGamepadPlayer(state, **slot, index);
            }
            catch (...) {
                if (*slot) Retire(*slot);
                else SDL_CloseGamepad(input);
                throw;
            }
        }
    }
    bool focused, mouse_known, mouse_inside;
    float mouse_x, mouse_y;
    Uint32 mouse_buttons;
    Clock::time_point mouse_used;
    std::array<bool, SDL_SCANCODE_COUNT> keys;
    std::array<unsigned, ShakeCount> shake_requests;
    {
        std::lock_guard lock(state.mutex);
        if (state.event_memory_failed)
            throw std::runtime_error("Desktop input instance metadata allocation failed");
        focused = state.focused;
        keys = state.keys;
        shake_requests = state.shake_requests;
        state.shake_requests = {};
        mouse_known = state.mouse_known;
        mouse_inside = state.mouse_inside;
        mouse_x = state.mouse_x;
        mouse_y = state.mouse_y;
        mouse_buttons = state.mouse_buttons;
        mouse_used = state.mouse_used;
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
        if (state.settings.share_mouse_with_remotes) SetNativeWpadSharedPointer(&observation);
    }
    const auto now = Clock::now();
    if (state.settings.mouse_player_channel >= 0) {
        // The mouse points while it is in use; otherwise the controller does.
        const bool held = focused && (mouse_buttons & (SDL_BUTTON_LMASK | SDL_BUTTON_RMASK));
        const bool active = held || (focused && mouse_used != Clock::time_point{} &&
                                     now - mouse_used < std::chrono::seconds(3));
        SetNativeWpadMousePlayer(state.settings.mouse_player_channel, active ? &observation : nullptr,
                                 focused && (mouse_buttons & SDL_BUTTON_LMASK),
                                 focused && (mouse_buttons & SDL_BUTTON_RMASK));
    }
    if (state.keyboard) {
        auto buttons = focused && state.settings.keyboard ? KeyboardButtons(keys, state.settings.keys) : std::array<bool, 11>{};
        // Mouse buttons are independent raw Wii A/B signals; source KPAD and
        // game listeners retain all edge/lock/invalid-pointer decisions.
        if (focused && state.settings.mouse) {
            buttons[0] = buttons[0] || (mouse_buttons & SDL_BUTTON_LMASK);
            buttons[1] = buttons[1] || (mouse_buttons & SDL_BUTTON_RMASK);
        }
        // An unfocused Nunchuk is released and level, like the buttons above.
        mscharged::platform::NativeNunchukObservation nunchuk{};
        nunchuk.acc_z = mscharged::platform::kNativeNunchukGravity;
        auto& keyboard = *state.keyboard;
        const bool keyboard_input = focused && state.settings.keyboard;
        for (int shake = 0; shake != ShakeCount; ++shake) {
            // E shakes the Remote, Q the Nunchuk (keyboard profile only).
            if (keyboard_input && shake_requests[shake]) {
                keyboard.shake_reports[shake] = 0;
                keyboard.shaking[shake] = true;
            } else if (!keyboard_input) keyboard.shaking[shake] = false;
        }
        if (keyboard_input) nunchuk = KeyboardNunchuk(keys, state.settings.keys);
        Report(keyboard, buttons, now, state.settings.mouse ? &observation : nullptr, &nunchuk);
    }
    for (auto& pad : state.pads) if (pad) {
        std::array<bool, 11> buttons{};
        if (focused) for (int n = 0; n < int(buttons.size()); ++n)
            buttons[n] = SDL_GetGamepadButton(pad->input, DesktopButtons[n]);
        if (!state.settings.gamepad_players) { Report(*pad, buttons, now); continue; }
        auto nunchuk = GamepadNunchuk(*pad, focused);
        UpdateGamepadShakes(*pad, focused);
        const auto pointer = GamepadPointer(state, *pad, focused, now);
        Report(*pad, buttons, now, pad->dpd_source.generation ? &pointer : nullptr, &nunchuk);
    }
}
void ShutdownDesktopWpad() {
    auto& state = Get();
    if (!state.ready) return;
    RequireOwner();
    {
        std::lock_guard lock(state.mutex);
        state.ready = false;
        ClearKeyboard(state);
        ClearMouse(state);
    }
    SDL_RemoveEventWatch(Watch, nullptr);
    if (state.settings.share_mouse_with_remotes) SetNativeWpadSharedPointer(nullptr);
    if (state.settings.mouse_player_channel >= 0) SetNativeWpadMousePlayer(-1, nullptr, false, false);
    Retire(state.keyboard);
    for (auto& pad : state.pads) Retire(pad);
    RestoreBackgroundHint();
    state.window = 0;
    state.owner = {};
}
} // namespace mscharged::platform
