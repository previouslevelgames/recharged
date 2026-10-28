#include "platform/desktop_dpd.h"
#include "platform/desktop_wpad.h"
#include "platform/hotkey_filter.h"
#include "platform/key_bindings.h"
#include "platform/gyro_pointer.h"
#include "platform/wpad_sdl.h"
#include "platform/interrupts.h"
#include <SDL3/SDL.h>
#include <revolution/wpad/WPAD.h>
#include <dolphin/os.h>

#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <thread>

namespace {
unsigned checks{}, connects{}, samples{}, extension_count{};
s32 extensions[4]{};
SDL_Window* window{};
std::thread::id owner;
void Check(bool value, const char* message) {
    ++checks;
    if (!value) throw std::runtime_error(message);
}
void Connect(s32, WPADResult result) {
    Check(owner == std::this_thread::get_id() && !mscharged::platform::NativeInterruptsEnabled(),
          "Connection delivery left the owner interrupt scope");
    connects += result == WPAD_ERR_OK;
}
void Sample(s32) {
    Check(owner == std::this_thread::get_id() && !mscharged::platform::NativeInterruptsEnabled(),
          "Sampling delivery left the owner interrupt scope");
    ++samples;
}
void Extension(s32 channel, s32 type) {
    Check(owner == std::this_thread::get_id() && !mscharged::platform::NativeInterruptsEnabled(),
          "Extension delivery left the owner interrupt scope");
    Check(channel == 0, "Extension change reached an unused player port");
    if (extension_count < 4) extensions[extension_count] = type;
    ++extension_count;
}
void Event(Uint32 type, SDL_Scancode key = SDL_SCANCODE_UNKNOWN) {
    SDL_Event e{}; e.type = type;
    if (type == SDL_EVENT_KEY_DOWN || type == SDL_EVENT_KEY_UP) {
        e.key.windowID = SDL_GetWindowID(window);
        e.key.scancode = key;
        e.key.down = type == SDL_EVENT_KEY_DOWN;
    } else e.window.windowID = SDL_GetWindowID(window);
    Check(SDL_PushEvent(&e), "Actual SDL event queue rejected input");
}
void Service() {
    SDL_PumpEvents();
    mscharged::platform::ServiceDesktopWpad();
    mscharged::platform::ServiceWpadSDL();
}
WPADStatus Report(int port = 0) {
    WPADStatus s{}; WPADRead(port, &s); return s;
}
WPADFSStatus FreestyleReport() {
    WPADFSStatus s{}; WPADRead(0, reinterpret_cast<WPADStatus*>(&s)); return s;
}
template<class F> void Until(F fn, const char* message, int seconds = 2) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
    while (!fn()) {
        Service();
        Check(std::chrono::steady_clock::now() < deadline, message);
        SDL_Delay(2);
    }
    ++checks;
}
struct GenericPad {
    SDL_JoystickID id{};
    SDL_Joystick* joystick{};
    GenericPad() {
        SDL_VirtualJoystickDesc d; SDL_INIT_INTERFACE(&d);
        d.type = SDL_JOYSTICK_TYPE_GAMEPAD;
        d.vendor_id = 0x1234; d.product_id = 0x4321;
        d.name = "Generated desktop controller for raw profile gate";
        d.naxes = SDL_GAMEPAD_AXIS_COUNT; d.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
        d.button_mask = (1u << SDL_GAMEPAD_BUTTON_COUNT) - 1;
        d.axis_mask = (1u << SDL_GAMEPAD_AXIS_COUNT) - 1;
        id = SDL_AttachVirtualJoystick(&d);
        Check(id != 0, "Real SDL generic fixture attach failed");
        joystick = SDL_OpenJoystick(id);
        Check(joystick, "Real SDL generic fixture handle failed");
    }
    ~GenericPad() {
        if (joystick) SDL_CloseJoystick(joystick);
        if (id) SDL_DetachVirtualJoystick(id);
    }
};
// A connected Wii Remote as WPAD sees one: core-Wii buttons and an accelerometer.
struct WiiRemote {
    SDL_JoystickID id{};
    SDL_Joystick* joystick{};
    WiiRemote() {
        static const SDL_VirtualJoystickSensorDesc sensor{SDL_SENSOR_ACCEL, 100.0f};
        SDL_VirtualJoystickDesc d; SDL_INIT_INTERFACE(&d);
        d.type = SDL_JOYSTICK_TYPE_GAMEPAD;
        d.vendor_id = 0x057e; d.product_id = 0x0306;
        d.name = "Nintendo Wii Remote";
        d.naxes = SDL_GAMEPAD_AXIS_COUNT; d.nbuttons = int(SDL_GAMEPAD_BUTTON_MISC1) + 11;
        d.button_mask = (1u << SDL_GAMEPAD_BUTTON_COUNT) - 1;
        d.axis_mask = (1u << SDL_GAMEPAD_AXIS_COUNT) - 1;
        d.nsensors = 1; d.sensors = &sensor;
        id = SDL_AttachVirtualJoystick(&d);
        Check(id != 0, "Real SDL Wii Remote fixture attach failed");
        joystick = SDL_OpenJoystick(id);
        Check(joystick, "Real SDL Wii Remote fixture handle failed");
    }
    ~WiiRemote() {
        if (joystick) SDL_CloseJoystick(joystick);
        if (id) SDL_DetachVirtualJoystick(id);
    }
};
// controls.player1-4: devices connect only on their own player channel, and
// devices without one skip the channels reserved for configured players.
void FixedPlayersCycle() {
    using mscharged::platform::GetNativeWpadChannel;
    using mscharged::platform::SetNativeWpadFixedChannel;
    using mscharged::platform::WpadSDLConnectedChannels;
    mscharged::platform::ConfigureWpadSDL({0,3,false,false});
    mscharged::platform::DesktopWpadSettings settings{};
    settings.keyboard = true;
    settings.keyboard_channel = 4;
    bool rejected = false;
    try { mscharged::platform::InitializeDesktopWpad(window, settings); }
    catch (const std::invalid_argument&) { rejected = true; }
    Check(rejected, "Keyboard & mouse was given a player outside the Wii ports");

    // Players: 1 = a Wii Remote that is still off, 2 = keyboard & mouse.
    mscharged::platform::SetNativeWpadReservedChannels(0b11);
    settings.keyboard_channel = 1;
    mscharged::platform::InitializeDesktopWpad(window, settings);
    WPADInit();
    Event(SDL_EVENT_WINDOW_FOCUS_GAINED);
    WPADDeviceType type{};
    Until([&] { return WPADProbe(1, &type) == WPAD_ERR_OK; }, "Keyboard & mouse did not become player 2");
    Check(WPADProbe(0, &type) == WPAD_ERR_NO_CONTROLLER, "Keyboard & mouse took player 1's reserved port");
    Event(SDL_EVENT_KEY_DOWN, SDL_SCANCODE_RETURN);
    Until([&] { return Report(1).button == WPAD_BUTTON_A; }, "Keyboard input did not reach player 2");
    Event(SDL_EVENT_KEY_UP, SDL_SCANCODE_RETURN);
    Until([&] { return Report(1).button == 0; }, "Keyboard kept a press");

    // A remote without a player skips the reserved port 1; player 1's remote
    // then connects on its own port; a remote whose port is busy waits.
    WiiRemote spare;
    Until([&] { return GetNativeWpadChannel(spare.id) == 2; }, "Unassigned remote did not skip reserved ports");
    WiiRemote first;
    SetNativeWpadFixedChannel(first.id, 0);
    Until([&] { return GetNativeWpadChannel(first.id) == 0; }, "Player 1's remote did not get port 1");
    WiiRemote waiting;
    SetNativeWpadFixedChannel(waiting.id, 1);
    for (int n = 0; n != 5; ++n) { Service(); SDL_Delay(2); }
    Check(GetNativeWpadChannel(waiting.id) == -1, "A remote took a busy player's port");
    Check(WpadSDLConnectedChannels() == 3, "Fixed players changed the connected ports");
    SetNativeWpadFixedChannel(waiting.id, -1);
    SetNativeWpadFixedChannel(first.id, -1);
    mscharged::platform::ShutdownDesktopWpad();
    WPADShutdown();
    mscharged::platform::SetNativeWpadReservedChannels(0);
}
// [keyboard] bindings replace the default keys of an action.
void CustomKeysCycle() {
    mscharged::platform::ConfigureWpadSDL({0,3,false,false});
    mscharged::platform::DesktopWpadSettings settings{};
    settings.keyboard = true;
    // The hotkeys ([hotkeys]) are no action's keys: P and H unless rebound.
    {
        using mscharged::platform::Hotkeys;
        using mscharged::platform::KeyBinding;
        using mscharged::platform::ParseHotkeys;
        std::array<std::string, mscharged::KeyActionCount> texts{};
        texts[mscharged::KeyActionB] = "Return | P";
        texts[mscharged::KeyActionOne] = "H";
        auto bindings = mscharged::platform::ParseKeyBindings(texts, Hotkeys{});
        Check(bindings[mscharged::KeyActionB] == KeyBinding{SDL_SCANCODE_RETURN, SDL_SCANCODE_UNKNOWN},
              "The screenshot key was bound to an action");
        Check(bindings[mscharged::KeyActionOne] == KeyBinding{SDL_SCANCODE_Z, SDL_SCANCODE_UNKNOWN},
              "An action left with only the HUD key did not keep its default");
        Hotkeys rebound;
        rebound[mscharged::HotkeyScreenshot] = {SDL_SCANCODE_F5};
        rebound[mscharged::HotkeyHud] = {SDL_SCANCODE_Z};
        bindings = mscharged::platform::ParseKeyBindings(texts, rebound);
        Check(bindings[mscharged::KeyActionB] == KeyBinding{SDL_SCANCODE_RETURN, SDL_SCANCODE_P}
                  && bindings[mscharged::KeyActionOne] == KeyBinding{SDL_SCANCODE_H, SDL_SCANCODE_UNKNOWN},
              "Rebound hotkeys did not free P and H");
        texts[mscharged::KeyActionOne] = "";
        Check(mscharged::platform::ParseKeyBindings(texts, rebound)[mscharged::KeyActionOne][0] == SDL_SCANCODE_UNKNOWN,
              "A default key taken by a hotkey stayed bound");
        // The display hotkeys (issue #91) are no action's keys either, unless unused.
        texts[mscharged::KeyActionC] = "F | PageUp";
        Check(mscharged::platform::ParseKeyBindings(texts, Hotkeys{})[mscharged::KeyActionC]
                  == KeyBinding{SDL_SCANCODE_C, SDL_SCANCODE_UNKNOWN},
              "The stretch or 3D resolution key was bound to an action");
        Hotkeys unused;
        unused[mscharged::HotkeyStretch] = {};
        Check(mscharged::platform::ParseKeyBindings(texts, unused)[mscharged::KeyActionC]
                  == KeyBinding{SDL_SCANCODE_F, SDL_SCANCODE_UNKNOWN},
              "An unused hotkey still took its key from an action");
        texts[mscharged::KeyActionC] = "";
        // A hotkey with Ctrl, Shift or Alt leaves its key to the actions.
        rebound[mscharged::HotkeyHud] = {SDL_SCANCODE_H, SDL_KMOD_CTRL};
        texts[mscharged::KeyActionOne] = "H";
        Check(mscharged::platform::ParseKeyBindings(texts, rebound)[mscharged::KeyActionOne]
                  == KeyBinding{SDL_SCANCODE_H, SDL_SCANCODE_UNKNOWN},
              "A Ctrl hotkey took its key from an action");
        texts[mscharged::KeyActionOne] = "";
        using Names = std::array<std::string, mscharged::HotkeyCount>;
        using mscharged::platform::Hotkey;
        const auto keys = [](const Hotkeys& hotkeys, std::array<Hotkey, mscharged::HotkeyCount> expected) {
            return hotkeys.keys == expected;
        };
        const std::array<Hotkey, mscharged::HotkeyCount> defaults = Hotkeys{}.keys;
        Check(defaults[mscharged::HotkeyFullscreen] == Hotkey{SDL_SCANCODE_RETURN, SDL_KMOD_ALT},
              "Fullscreen is not Alt+Return by default");
        Check(keys(ParseHotkeys(Names{"F12", "G", "T", "F6", "F7", "F11"}),
                   {{{SDL_SCANCODE_F12}, {SDL_SCANCODE_G}, {SDL_SCANCODE_T}, {SDL_SCANCODE_F6}, {SDL_SCANCODE_F7},
                     {SDL_SCANCODE_F11}}}),
              "Hotkeys did not parse");
        Check(keys(ParseHotkeys(Names{"Ctrl+Shift+F12", "alt + h", "Control+Keypad +", "Option+Return", "Shift+PageDown",
                                      "Ctrl+Alt+Enter"}),
                   {{{SDL_SCANCODE_F12, SDL_KMOD_CTRL | SDL_KMOD_SHIFT}, {SDL_SCANCODE_H, SDL_KMOD_ALT},
                     {SDL_SCANCODE_KP_PLUS, SDL_KMOD_CTRL}, {SDL_SCANCODE_RETURN, SDL_KMOD_ALT},
                     {SDL_SCANCODE_PAGEDOWN, SDL_KMOD_SHIFT}, {SDL_SCANCODE_RETURN, SDL_KMOD_CTRL | SDL_KMOD_ALT}}}),
              "Hotkeys with Ctrl, Shift and Alt did not parse");
        Check(keys(ParseHotkeys(Names{"Left Alt", "Mouse 4", "", "Nonsense", "Ctrl+", "Cmd+Q"}), defaults),
              "A modifier, mouse button, unknown name or system key became a hotkey");
        Check(keys(ParseHotkeys(Names{"G", "G", "F", "PageUp", "PageDown", "Alt+Return"}), defaults),
              "Two hotkeys took the same key");
        Check(keys(ParseHotkeys(Names{"P", "H", "Alt+Return", "PageUp", "PageDown", "Alt+Return"}), defaults),
              "Two hotkeys took the same keys");
        Check(keys(ParseHotkeys(Names{"P", "F", "F", "PageUp", "PageDown", "Alt+Return"}), defaults),
              "A hotkey took another one's default key");
        Check(keys(ParseHotkeys(Names{"P", "H", "None", "none", "PageDown", "none"}),
                   {{{SDL_SCANCODE_P}, {SDL_SCANCODE_H}, {}, {}, {SDL_SCANCODE_PAGEDOWN}, {}}}),
              "none did not leave a hotkey unused");
        using mscharged::platform::HotkeyName;
        Check(HotkeyName({}) == "none" && HotkeyName({SDL_SCANCODE_PAGEUP}) == "PageUp"
                  && HotkeyName({SDL_SCANCODE_F12, SDL_KMOD_CTRL | SDL_KMOD_SHIFT | SDL_KMOD_ALT}) == "Ctrl+Shift+Alt+F12"
                  && ParseHotkeys(Names{"P", "H", "F", "PageUp", "PageDown", "Alt+Return"}).Find(SDL_SCANCODE_PAGEUP)
                         == mscharged::HotkeyResolutionUp
                  && Hotkeys{}.Find(SDL_SCANCODE_RETURN) == mscharged::HotkeyCount
                  && Hotkeys{}.Find(SDL_SCANCODE_UNKNOWN) == mscharged::HotkeyCount,
              "Hotkey names or lookup");
        // A press is a hotkey with exactly its Ctrl, Shift and Alt; never with Windows or Command.
        const Hotkeys standard;
        Check(standard.Match(SDL_SCANCODE_RETURN, SDL_KMOD_LALT) == mscharged::HotkeyFullscreen
                  && standard.Match(SDL_SCANCODE_KP_ENTER, SDL_KMOD_RALT) == mscharged::HotkeyFullscreen
                  && standard.Match(SDL_SCANCODE_P, SDL_KMOD_NUM | SDL_KMOD_CAPS) == mscharged::HotkeyScreenshot,
              "A hotkey press was not its hotkey");
        Check(standard.Match(SDL_SCANCODE_RETURN, SDL_KMOD_NONE) == mscharged::HotkeyCount
                  && standard.Match(SDL_SCANCODE_RETURN, SDL_KMOD_LALT | SDL_KMOD_LSHIFT) == mscharged::HotkeyCount
                  && standard.Match(SDL_SCANCODE_RETURN, SDL_KMOD_LALT | SDL_KMOD_LGUI) == mscharged::HotkeyCount
                  && standard.Match(SDL_SCANCODE_P, SDL_KMOD_LCTRL) == mscharged::HotkeyCount
                  && standard.Match(SDL_SCANCODE_SPACE, SDL_KMOD_LALT) == mscharged::HotkeyCount,
              "Other keys or modifiers made a hotkey");
#if defined(__APPLE__)
        const std::vector<std::string> fullscreen{"Option", "Return"};
#else
        const std::vector<std::string> fullscreen{"Alt", "Enter"};
#endif
        Check(mscharged::platform::HotkeyKeys(standard[mscharged::HotkeyFullscreen]) == fullscreen
                  && mscharged::platform::HotkeyKeys({}).empty() && mscharged::platform::HotkeyLabel({}).empty(),
              "Hotkey labels");
    }
    settings.keys[mscharged::KeyActionA] = mscharged::platform::ParseKeyBinding("F1");
    settings.keys[mscharged::KeyActionB] = mscharged::platform::ParseKeyBinding("Return");
    mscharged::platform::InitializeDesktopWpad(window, settings);
    WPADInit();
    Event(SDL_EVENT_WINDOW_FOCUS_GAINED);
    WPADDeviceType type{};
    Until([&] { return WPADProbe(0, &type) == WPAD_ERR_OK; }, "Keyboard remote did not connect");
    Event(SDL_EVENT_KEY_DOWN, SDL_SCANCODE_F1);
    Until([&] { return Report().button == WPAD_BUTTON_A; }, "F1 bound to A did not press A");
    Event(SDL_EVENT_KEY_UP, SDL_SCANCODE_F1);
    Until([&] { return Report().button == 0; }, "F1 release kept A");
    Event(SDL_EVENT_KEY_DOWN, SDL_SCANCODE_RETURN);
    Until([&] { return Report().button == WPAD_BUTTON_B; }, "Return rebound to B did not press B");
    Check(!(Report().button & WPAD_BUTTON_A), "Return still pressed A after rebinding");
    Event(SDL_EVENT_KEY_UP, SDL_SCANCODE_RETURN);
    Until([&] { return Report().button == 0; }, "Return release kept B");
    mscharged::platform::ShutdownDesktopWpad();
    WPADShutdown();
}
void Cycle(bool gamepads, bool physical) {
    connects = samples = 0;
    mscharged::platform::ConfigureWpadSDL({0,3,physical,gamepads});
    mscharged::platform::InitializeDesktopWpad(window, {true,gamepads});
    WPADInit();
    Check(bool(WPADIsMotorEnabled()) == gamepads,
          "WPAD initialization lost the staged motor preference across profiles");
    for (int n = 0; n != 4; ++n) {
        WPADSetConnectCallback(n, Connect);
        WPADSetSamplingCallback(n, Sample);
    }
    Event(SDL_EVENT_WINDOW_FOCUS_GAINED);
    auto expected = gamepads ? 2u : 1u;
    Until([&] { return connects == expected && Report().err == WPAD_ERR_OK; },
          "Selected raw device profile did not discover actual SDL backing");
    Check(mscharged::platform::WpadSDLConnectedChannels() == expected,
          "Selected raw profile admitted an unexpected controller");
    if (!gamepads) {
        for (int n = 1; n != 4; ++n) {
            WPADDeviceType type{};
            Check(WPADProbe(n,&type) == WPAD_ERR_NO_CONTROLLER,
                  "Keyboard-only profile occupied another player port");
        }
    }
    Check(Report().button == 0, "Initial source report manufactured a press");
    WPADEnableMotor(FALSE);
    WPADControlMotor(0, WPAD_MOTOR_RUMBLE);
    Check(!WPADIsMotorEnabled() && Report().button == 0,
          "Disabled motor request changed the source preference or raw input");
    WPADEnableMotor(gamepads ? TRUE : FALSE);
    Event(SDL_EVENT_KEY_DOWN, SDL_SCANCODE_RETURN);
    auto before = samples;
    auto old = OSDisableInterrupts();
    SDL_Delay(12); Service();
    Check(samples == before && Report().button == 0, "Masked input delivered a report");
    OSRestoreInterrupts(old);
    Until([&] { return Report().button == WPAD_BUTTON_A; },
          "Enter did not reach actual first-port raw Wii A");
    Event(SDL_EVENT_KEY_UP, SDL_SCANCODE_RETURN);
    Until([&] { return Report().button == 0; }, "Enter release retained a raw press");
    Event(SDL_EVENT_KEY_DOWN, SDL_SCANCODE_SPACE);
    Until([&] { return Report().button == WPAD_BUTTON_A; }, "Space raw A mapping failed");
    Event(SDL_EVENT_WINDOW_FOCUS_LOST);
    Until([&] { return Report().button == 0; }, "Focus loss retained a raw press");
    Event(SDL_EVENT_WINDOW_FOCUS_GAINED);
    Event(SDL_EVENT_KEY_DOWN, SDL_SCANCODE_ESCAPE);
    Until([&] { return Report().button == WPAD_BUTTON_B; }, "Escape raw B mapping failed");
    Event(SDL_EVENT_KEY_UP, SDL_SCANCODE_ESCAPE);
    Until([&] { return Report().button == 0; }, "Escape release retained a raw press");
    Event(SDL_EVENT_KEY_DOWN, SDL_SCANCODE_LEFT);
    Until([&] { return Report().button == WPAD_BUTTON_LEFT; }, "Left raw D-pad mapping failed");
    Event(SDL_EVENT_WINDOW_FOCUS_LOST);
    Until([&] { return Report().button == 0; }, "Raw D-pad did not clear with focus");
    mscharged::platform::ShutdownDesktopWpad();
    WPADShutdown();
}
// A standard gamepad as a player: a Wii Remote with Nunchuk on its own channel.
void GamepadPlayerCycle() {
    connects = samples = extension_count = 0;
    mscharged::platform::ConfigureWpadSDL({0,3,false,false});
    mscharged::platform::DesktopWpadSettings settings{};
    settings.gamepads = settings.gamepad_players = true;
    settings.gamepad_channels = {1, -1, -1, -1};
    GenericPad pad;
    mscharged::platform::InitializeDesktopWpad(window, settings);
    WPADInit();
    // Extension callbacks are checked on player 1 elsewhere; this pad is player 2.
    for (int n = 0; n != 4; ++n) {
        WPADSetConnectCallback(n, Connect);
        WPADSetSamplingCallback(n, Sample);
    }
    Event(SDL_EVENT_WINDOW_FOCUS_GAINED);
    WPADDeviceType type{};
    Until([&] { return WPADProbe(1, &type) == WPAD_ERR_OK && type == WPAD_DEV_FREESTYLE; },
          "Gamepad did not connect as player 2 with a Nunchuk");
    Check(WPADProbe(0, &type) != WPAD_ERR_OK, "Gamepad also took player 1");
    Check(WPADSetDataFormat(1, WPAD_FMT_FS_BTN_ACC_DPD) == WPAD_ERR_OK, "Freestyle format was rejected");
    const auto report = [] {
        WPADFSStatus status{};
        WPADRead(1, reinterpret_cast<WPADStatus*>(&status));
        return status;
    };
    SDL_SetJoystickVirtualButton(pad.joystick, SDL_GAMEPAD_BUTTON_SOUTH, true);
    Until([&] { return report().button == WPAD_BUTTON_A; }, "Gamepad south did not press A");
    SDL_SetJoystickVirtualButton(pad.joystick, SDL_GAMEPAD_BUTTON_SOUTH, false);
    SDL_SetJoystickVirtualAxis(pad.joystick, SDL_GAMEPAD_AXIS_LEFTX, 32767);
    Until([&] { auto r = report(); return r.fsStickX == 71 && r.fsStickY == 0 && r.button == 0; },
          "Left stick did not move the Nunchuk stick right");
    SDL_SetJoystickVirtualAxis(pad.joystick, SDL_GAMEPAD_AXIS_LEFTX, 0);
    SDL_SetJoystickVirtualAxis(pad.joystick, SDL_GAMEPAD_AXIS_LEFT_TRIGGER, 32767);
    SDL_SetJoystickVirtualButton(pad.joystick, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, true);
    Until([&] { auto r = report(); return r.fsStickX == 0 && r.button == (WPAD_BUTTON_FS_Z | WPAD_BUTTON_FS_C); },
          "Left trigger and bumper did not press Z and C");
    SDL_SetJoystickVirtualAxis(pad.joystick, SDL_GAMEPAD_AXIS_LEFT_TRIGGER, 0);
    SDL_SetJoystickVirtualButton(pad.joystick, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, false);
    Until([&] { return report().button == 0; }, "Gamepad release kept a button");
    mscharged::platform::ShutdownDesktopWpad();
    WPADShutdown();
}
// The game's own processing of the Nunchuk stick (original ClampWiiStick),
// written out again: 15 counts dropped per axis, full push 56 counts further.
std::array<int, 2> GameStick(int x, int y) {
    const auto axis = [](int v) { return v < 15 && v > -15 ? 0 : v > 0 ? v - 15 : v + 15; };
    x = axis(x);
    y = axis(y);
    if (x * x + y * y > 56 * 56) {
        const float scale = 56.0f / std::sqrt(float(x * x + y * y));
        x = int(x * scale);
        y = int(y * scale);
    }
    return {x, y};
}
// Issue #62: round dead zones, and Nunchuk counts the game turns back into
// exactly the tilt the dead zones give.
void StickDeadzoneChecks() {
    using namespace mscharged::platform;
    const auto near = [](float a, float b) { return std::fabs(a - b) < 1e-4f; };
    Check(ApplyStickDeadzones(0.15f, 0, 15, 71).x == 0 && ApplyStickDeadzones(0.1f, -0.1f, 15, 71).y == 0,
          "The stick moved inside the inner dead zone");
    Check(near(ApplyStickDeadzones(0.43f, 0, 15, 71).x, 0.5f), "Tilt between the dead zones is not linear");
    Check(near(ApplyStickDeadzones(0, -0.71f, 15, 71).y, -1) && near(ApplyStickDeadzones(1, 0, 15, 71).x, 1),
          "The stick is not fully pushed from the outer dead zone on");
    const auto diagonal = ApplyStickDeadzones(1, 1, 15, 71);
    Check(near(diagonal.x, std::sqrt(0.5f)) && near(diagonal.y, std::sqrt(0.5f)),
          "A full diagonal left the round gate");
    Check(near(ApplyStickDeadzones(0.05f, 0, 0, 100).x, 0.05f), "No dead zone changed the tilt");
    float previous = 0;
    for (int n = 0; n <= 1000; ++n) {
        const auto tilt = ApplyStickDeadzones(n / 1000.0f * 0.6f, n / 1000.0f * 0.8f, 20, 90);
        const float length = std::hypot(tilt.x, tilt.y);
        Check(length + 1e-6f >= previous && length <= 1.0f + 1e-6f, "Dead zones are not monotonic");
        previous = length;
    }
    Check(NunchukStickCounts({}) == std::array<std::int8_t, 2>{0, 0}
              && NunchukStickCounts({1, 0}) == std::array<std::int8_t, 2>{71, 0}
              && NunchukStickCounts({0, -1}) == std::array<std::int8_t, 2>{0, -71},
          "Nunchuk counts at rest or fully pushed");
    for (int a = 0; a < 360; a += 3)
        for (int r = 0; r <= 100; ++r) {
            const float x = std::cos(a * 3.14159265f / 180) * r / 100, y = std::sin(a * 3.14159265f / 180) * r / 100;
            const auto counts = NunchukStickCounts({x, y});
            const auto game = GameStick(counts[0], counts[1]);
            Check(std::abs(game[0] - int(std::lround(x * 56))) <= 1 && std::abs(game[1] - int(std::lround(y * 56))) <= 1,
                  "The game does not read the tilt the dead zones gave");
        }
    // A real Nunchuk: corners at the gate's reach in all eight directions, the
    // edges between them closer in, and still fully pushed all around the rim.
    Check(OctagonNunchukStickCounts({}) == std::array<std::int8_t, 2>{0, 0}
              && OctagonNunchukStickCounts({1, 0}) == std::array<std::int8_t, 2>{96, 0}
              && OctagonNunchukStickCounts({0, -2}) == std::array<std::int8_t, 2>{0, -96}
              && OctagonNunchukStickCounts({std::sqrt(0.5f), std::sqrt(0.5f)}) == std::array<std::int8_t, 2>{68, 68},
          "Real Nunchuk corners");
    const float tilted = 22.5f * 3.14159265f / 180;
    const auto edge = OctagonNunchukStickCounts({std::cos(tilted), std::sin(tilted)});
    Check(std::abs(std::hypot(float(edge[0]), float(edge[1])) - 96 * std::cos(tilted)) < 1,
          "The real Nunchuk's gate is not octagonal");
    for (int a = 0; a < 360; ++a) {
        const float x = std::cos(a * 3.14159265f / 180), y = std::sin(a * 3.14159265f / 180);
        Check(NunchukStickPushed(OctagonNunchukStickCounts({x, y})) == 1.0f, "A real Nunchuk's rim is not fully pushed");
    }
    Check(NunchukStickPushed(OctagonNunchukStickCounts({0.15f, 0})) == 0
              && OctagonNunchukStickCounts({1, 0.15f})[1] < kNunchukStickDeadCounts,
          "The game's own dead zone along each axis");
    Check(GamepadTriggerThreshold(50) == kGamepadTriggerPressed && GamepadTriggerThreshold(10) == 3276
              && GamepadTriggerThreshold(100) == 32767,
          "Trigger press points");
}
// [gamepadN]: a gamepad's own layout reaches WPAD as configured.
void CustomGamepadCycle() {
    using namespace mscharged::platform;
    Check(ParseGamepadBinding("b | righttrigger") ==
              GamepadBinding{SDL_GAMEPAD_BUTTON_EAST, kGamepadTriggerBase + SDL_GAMEPAD_AXIS_RIGHT_TRIGGER},
          "Gamepad input names did not parse");
    Check(ParseGamepadBinding("leftx | nonsense")[0] == kGamepadNone, "A stick axis was accepted as a button");
    Check(FormatGamepadBinding(ParseGamepadBinding(" a |lefttrigger ")) == "a | lefttrigger",
          "Gamepad binding did not format back");
    std::array<std::string, mscharged::GamepadActionCount> texts{};
    texts[mscharged::GamepadActionA] = "b";
    texts[mscharged::GamepadActionB] = "a";
    texts[mscharged::GamepadActionC] = "lefttrigger";
    texts[mscharged::GamepadActionZ] = "leftshoulder";
    const auto bindings = ParseGamepadBindings(texts);
    Check(bindings[mscharged::GamepadActionPlus] == DefaultGamepadBindings()[mscharged::GamepadActionPlus],
          "An unset gamepad action lost its default");
    connects = samples = extension_count = 0;
    ConfigureWpadSDL({0,3,false,false});
    DesktopWpadSettings settings{};
    settings.gamepads = settings.gamepad_players = true;
    settings.gamepad_channels = {1, -1, -1, -1};
    settings.gamepad_profiles[0] = {bindings, true, false, 20, 80, 30};
    GenericPad pad;
    InitializeDesktopWpad(window, settings);
    WPADInit();
    for (int n = 0; n != 4; ++n) WPADSetConnectCallback(n, Connect);
    Event(SDL_EVENT_WINDOW_FOCUS_GAINED);
    WPADDeviceType type{};
    Until([&] { return WPADProbe(1, &type) == WPAD_ERR_OK && type == WPAD_DEV_FREESTYLE; },
          "Custom gamepad did not connect as player 2");
    Check(WPADSetDataFormat(1, WPAD_FMT_FS_BTN_ACC_DPD) == WPAD_ERR_OK, "Freestyle format was rejected");
    const auto report = [] {
        WPADFSStatus status{};
        WPADRead(1, reinterpret_cast<WPADStatus*>(&status));
        return status;
    };
    SDL_SetJoystickVirtualButton(pad.joystick, SDL_GAMEPAD_BUTTON_EAST, true);
    Until([&] { return report().button == WPAD_BUTTON_A; }, "Own layout: east did not press A");
    SDL_SetJoystickVirtualButton(pad.joystick, SDL_GAMEPAD_BUTTON_EAST, false);
    SDL_SetJoystickVirtualButton(pad.joystick, SDL_GAMEPAD_BUTTON_SOUTH, true);
    Until([&] { return report().button == WPAD_BUTTON_B; }, "Own layout: south did not press B");
    SDL_SetJoystickVirtualButton(pad.joystick, SDL_GAMEPAD_BUTTON_SOUTH, false);
    SDL_SetJoystickVirtualAxis(pad.joystick, SDL_GAMEPAD_AXIS_RIGHTX, 32767);
    Until([&] { auto r = report(); return r.fsStickX == 71 && r.button == 0; },
          "Swapped sticks: the right stick did not move the Nunchuk stick");
    // Dead zones 20-80 %: half the way between them is half pushed.
    SDL_SetJoystickVirtualAxis(pad.joystick, SDL_GAMEPAD_AXIS_RIGHTX, Sint16(0.5f * 32767));
    Until([&] { auto r = report(); return GameStick(r.fsStickX, r.fsStickY)[0] == 28; },
          "Tilt between the dead zones did not reach the game half pushed");
    SDL_SetJoystickVirtualAxis(pad.joystick, SDL_GAMEPAD_AXIS_RIGHTX, Sint16(-0.19f * 32767));
    Until([&] { return report().fsStickX == 0; }, "The stick moved inside the inner dead zone");
    SDL_SetJoystickVirtualAxis(pad.joystick, SDL_GAMEPAD_AXIS_RIGHTX, Sint16(-0.85f * 32767));
    Until([&] { return report().fsStickX == -71; }, "The stick was not fully pushed past the outer dead zone");
    SDL_SetJoystickVirtualAxis(pad.joystick, SDL_GAMEPAD_AXIS_RIGHTX, 0);
    SDL_SetJoystickVirtualAxis(pad.joystick, SDL_GAMEPAD_AXIS_LEFTX, 32767);
    Until([&] { return report().fsStickX == 0; }, "Swapped sticks: the left stick still moved the Nunchuk stick");
    SDL_SetJoystickVirtualAxis(pad.joystick, SDL_GAMEPAD_AXIS_LEFTX, 0);
    // Press point 30 %: a trigger 40 % in presses its button. A virtual pad's
    // trigger travels over the whole raw axis, resting at its minimum.
    const auto trigger = [](float travel) { return Sint16(std::lround(travel * 65535 - 32768)); };
    SDL_SetJoystickVirtualAxis(pad.joystick, SDL_GAMEPAD_AXIS_LEFT_TRIGGER, trigger(0.4f));
    Until([&] { return report().button == WPAD_BUTTON_FS_C; }, "Own layout: left trigger did not press C");
    SDL_SetJoystickVirtualAxis(pad.joystick, SDL_GAMEPAD_AXIS_LEFT_TRIGGER, trigger(0.25f));
    Until([&] { return report().button == 0; }, "The trigger pressed C before its press point");
    SDL_SetJoystickVirtualAxis(pad.joystick, SDL_GAMEPAD_AXIS_LEFT_TRIGGER, trigger(0));
    SDL_SetJoystickVirtualButton(pad.joystick, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, true);
    Until([&] { return report().button == WPAD_BUTTON_FS_Z; }, "Own layout: left bumper did not press Z");
    SDL_SetJoystickVirtualButton(pad.joystick, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, false);
    Until([&] { return report().button == 0; }, "Custom gamepad release kept a button");
    ShutdownDesktopWpad();
    WPADShutdown();
}
// The built-in Default follows the connected controller: a Switch Pro
// controller presses Wii A with its own A, the right face button.
void ControllerFamilyCycle() {
    using namespace mscharged::platform;
    using mscharged::GamepadActionA, mscharged::GamepadActionB, mscharged::GamepadActionOne;
    Check(DefaultGamepadBindings(GamepadFamily::Nintendo)[GamepadActionA][0] == SDL_GAMEPAD_BUTTON_EAST
              && DefaultGamepadBindings(GamepadFamily::Nintendo)[GamepadActionB][0] == SDL_GAMEPAD_BUTTON_SOUTH,
          "Nintendo default does not follow its labels");
    Check(DefaultGamepadBindings(GamepadFamily::GameCube)[GamepadActionB][0] == SDL_GAMEPAD_BUTTON_WEST
              && DefaultGamepadBindings(GamepadFamily::GameCube)[GamepadActionOne][0] == kGamepadNone,
          "GameCube default layout");
    Check(DefaultGamepadBindings(GamepadFamily::PlayStation) == DefaultGamepadBindings(),
          "PlayStation default differs from the positional default");
    Check(GamepadFamilyOf(SDL_GAMEPAD_TYPE_PS5) == GamepadFamily::PlayStation
              && GamepadFamilyOf(SDL_GAMEPAD_TYPE_GAMECUBE) == GamepadFamily::GameCube
              && GamepadFamilyOf(SDL_GAMEPAD_TYPE_UNKNOWN) == GamepadFamily::Xbox,
          "Controller families");
    std::array<std::string, mscharged::GamepadActionCount> texts{};
    texts[GamepadActionOne] = "none";
    Check(ParseGamepadBindings(texts)[GamepadActionOne][0] == kGamepadNone
              && ParseGamepadBindings(texts)[GamepadActionB][0] == SDL_GAMEPAD_BUTTON_EAST
              && FormatGamepadBinding({kGamepadNone, kGamepadNone}) == "none",
          "\"none\" did not leave an action without a button");

    connects = samples = extension_count = 0;
    ConfigureWpadSDL({0,3,false,false});
    DesktopWpadSettings settings{};
    settings.gamepads = settings.gamepad_players = true;
    settings.gamepad_channels = {1, -1, -1, -1};
    settings.gamepad_profiles[0].follow_controller = true;
    SDL_VirtualJoystickDesc d; SDL_INIT_INTERFACE(&d);
    d.type = SDL_JOYSTICK_TYPE_GAMEPAD;
    d.vendor_id = 0x057e; d.product_id = 0x2009; // Switch Pro controller
    d.name = "Generated Nintendo Switch Pro Controller";
    d.naxes = SDL_GAMEPAD_AXIS_COUNT; d.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
    d.button_mask = (1u << SDL_GAMEPAD_BUTTON_COUNT) - 1;
    d.axis_mask = (1u << SDL_GAMEPAD_AXIS_COUNT) - 1;
    const SDL_JoystickID id = SDL_AttachVirtualJoystick(&d);
    Check(id != 0, "Switch Pro fixture attach failed");
    SDL_Joystick* joystick = SDL_OpenJoystick(id);
    Check(joystick, "Switch Pro fixture handle failed");
    Check(SDL_GetGamepadTypeForID(id) == SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO, "Fixture is not a Switch Pro type");
    InitializeDesktopWpad(window, settings);
    WPADInit();
    for (int n = 0; n != 4; ++n) WPADSetConnectCallback(n, Connect);
    Event(SDL_EVENT_WINDOW_FOCUS_GAINED);
    WPADDeviceType type{};
    Until([&] { return WPADProbe(1, &type) == WPAD_ERR_OK && type == WPAD_DEV_FREESTYLE; },
          "Switch Pro controller did not connect as player 2");
    Check(WPADSetDataFormat(1, WPAD_FMT_FS_BTN_ACC_DPD) == WPAD_ERR_OK, "Freestyle format was rejected");
    const auto report = [] {
        WPADFSStatus status{};
        WPADRead(1, reinterpret_cast<WPADStatus*>(&status));
        return status;
    };
    SDL_SetJoystickVirtualButton(joystick, SDL_GAMEPAD_BUTTON_EAST, true);
    Until([&] { return report().button == WPAD_BUTTON_A; }, "Switch Pro A (right) did not press Wii A");
    SDL_SetJoystickVirtualButton(joystick, SDL_GAMEPAD_BUTTON_EAST, false);
    SDL_SetJoystickVirtualButton(joystick, SDL_GAMEPAD_BUTTON_SOUTH, true);
    Until([&] { return report().button == WPAD_BUTTON_B; }, "Switch Pro B (bottom) did not press Wii B");
    SDL_SetJoystickVirtualButton(joystick, SDL_GAMEPAD_BUTTON_SOUTH, false);
    Until([&] { return report().button == 0; }, "Switch Pro release kept a button");
    ShutdownDesktopWpad();
    WPADShutdown();
    SDL_CloseJoystick(joystick);
    SDL_DetachVirtualJoystick(id);
}
// controls.mouse_pointer: the mouse adds A/B (and its pointer) to a gamepad player 1.
bool WholeWindow(void*, SDL_Window* target, mscharged::platform::DesktopDpdProjection* projection) {
    int w = 0, h = 0;
    SDL_GetWindowSize(target, &w, &h);
    *projection = {1, 0.0f, 0.0f, float(w), float(h)};
    return true;
}
void MouseButton(Uint8 button, bool down) {
    SDL_Event e{};
    e.type = down ? SDL_EVENT_MOUSE_BUTTON_DOWN : SDL_EVENT_MOUSE_BUTTON_UP;
    e.button.windowID = SDL_GetWindowID(window);
    e.button.which = 1;
    e.button.button = button;
    e.button.down = down;
    e.button.x = 320; e.button.y = 224;
    Check(SDL_PushEvent(&e), "Actual SDL event queue rejected mouse input");
}
void MousePlayerCycle() {
    mscharged::platform::ConfigureWpadSDL({0,3,false,false});
    mscharged::platform::DesktopWpadSettings settings{};
    settings.gamepads = settings.gamepad_players = true;
    settings.gamepad_channels = {0, -1, -1, -1};
    settings.mouse = true;
    settings.pointer_projection = WholeWindow;
    settings.mouse_player_channel = 0;
    GenericPad pad;
    mscharged::platform::InitializeDesktopWpad(window, settings);
    WPADInit();
    for (int n = 0; n != 4; ++n) WPADSetConnectCallback(n, Connect);
    Event(SDL_EVENT_WINDOW_FOCUS_GAINED);
    WPADDeviceType type{};
    Until([&] { return WPADProbe(0, &type) == WPAD_ERR_OK; }, "Gamepad did not connect as player 1");
    Check(WPADProbe(1, &type) != WPAD_ERR_OK, "The mouse brought its own remote");
    MouseButton(SDL_BUTTON_LEFT, true);
    Until([&] { return Report().button == WPAD_BUTTON_A; }, "Left click did not press player 1's A");
    MouseButton(SDL_BUTTON_LEFT, false);
    MouseButton(SDL_BUTTON_RIGHT, true);
    Until([&] { return Report().button == WPAD_BUTTON_B; }, "Right click did not press player 1's B");
    MouseButton(SDL_BUTTON_RIGHT, false);
    SDL_SetJoystickVirtualButton(pad.joystick, SDL_GAMEPAD_BUTTON_EAST, true);
    Until([&] { return Report().button == WPAD_BUTTON_B; }, "The gamepad stopped working beside the mouse");
    SDL_SetJoystickVirtualButton(pad.joystick, SDL_GAMEPAD_BUTTON_EAST, false);
    Until([&] { return Report().button == 0; }, "Mouse or gamepad release kept a button");
    mscharged::platform::ShutdownDesktopWpad();
    WPADShutdown();
}
// Middle and side mouse buttons press the keyboard actions bound to them (#62);
// left and right click stay A and B.
void MouseKeysCycle() {
    using namespace mscharged::platform;
    Check(ParseKeyBinding("mouse 4 | Return") == KeyBinding{kMouseKeyBase + SDL_BUTTON_X1, SDL_SCANCODE_RETURN}
              && FormatKeyBinding(ParseKeyBinding("Mouse Middle|mouse 5")) == "Mouse Middle | Mouse 5"
              && MouseKeyInput(SDL_BUTTON_LEFT) == kKeyNone && MouseButtonOf(SDL_SCANCODE_A) == 0,
          "Mouse button names");
    ConfigureWpadSDL({0,3,false,false});
    DesktopWpadSettings settings{};
    settings.keyboard = settings.mouse = true;
    settings.pointer_projection = WholeWindow;
    settings.keys[mscharged::KeyActionOne] = ParseKeyBinding("Mouse 4");
    settings.keys[mscharged::KeyActionB] = ParseKeyBinding("Escape | Mouse 5");
    settings.keys[mscharged::KeyActionUp] = ParseKeyBinding("Mouse Middle");
    InitializeDesktopWpad(window, settings);
    WPADInit();
    Event(SDL_EVENT_WINDOW_FOCUS_GAINED);
    WPADDeviceType type{};
    Until([&] { return WPADProbe(0, &type) == WPAD_ERR_OK; }, "Keyboard & mouse did not connect");
    MouseButton(SDL_BUTTON_X1, true);
    Until([&] { return Report().button == WPAD_BUTTON_1; }, "Mouse 4 bound to 1 did not press 1");
    MouseButton(SDL_BUTTON_X1, false);
    MouseButton(SDL_BUTTON_X2, true);
    Until([&] { return Report().button == WPAD_BUTTON_B; }, "Mouse 5 as B's second input did not press B");
    MouseButton(SDL_BUTTON_X2, false);
    MouseButton(SDL_BUTTON_MIDDLE, true);
    Until([&] { return Report().button == WPAD_BUTTON_UP; }, "Middle click bound to up did not press up");
    MouseButton(SDL_BUTTON_MIDDLE, false);
    MouseButton(SDL_BUTTON_LEFT, true);
    Until([&] { return Report().button == WPAD_BUTTON_A; }, "Left click stopped pressing A");
    MouseButton(SDL_BUTTON_LEFT, false);
    Until([&] { return Report().button == 0; }, "A mouse button release kept a press");
    ShutdownDesktopWpad();
    WPADShutdown();
}
void NunchukCycle() {
    connects = samples = extension_count = 0;
    mscharged::platform::ConfigureWpadSDL({0,3,false,false});
    mscharged::platform::DesktopWpadSettings settings{};
    settings.nunchuk = true;
    bool rejected = false;
    try { mscharged::platform::InitializeDesktopWpad(window, settings); }
    catch (const std::invalid_argument&) { rejected = true; }
    Check(rejected, "A Nunchuk was attached without the keyboard remote");
    settings.keyboard = true;
    mscharged::platform::InitializeDesktopWpad(window, settings);
    WPADInit();
    for (int n = 0; n != 4; ++n) {
        WPADSetConnectCallback(n, Connect);
        WPADSetExtensionCallback(n, Extension);
        WPADSetSamplingCallback(n, Sample);
    }
    Event(SDL_EVENT_WINDOW_FOCUS_GAINED);
    WPADDeviceType type{};
    Until([&] { return connects == 1 && WPADProbe(0, &type) == WPAD_ERR_OK && type == WPAD_DEV_FREESTYLE; },
          "Virtual Nunchuk did not complete the original extension sequence");
    Check(extension_count == 2 && extensions[0] == WPAD_DEV_INITIALIZING && extensions[1] == WPAD_DEV_FREESTYLE,
          "Extension callbacks skipped INITIALIZING or FREESTYLE");
    WPADAccGravityUnit unit{};
    WPADGetAccGravityUnit(0, WPAD_ACC_GRAVITY_UNIT_FS, &unit);
    Check(unit.x == 200 && unit.y == 200 && unit.z == 200, "Identified Nunchuk lacks its nominal calibration");

    // Core formats carry no extension bytes and copy only the core layout.
    // A, pressed with C, proves the report was latched after both presses.
    Event(SDL_EVENT_KEY_DOWN, SDL_SCANCODE_C);
    Event(SDL_EVENT_KEY_DOWN, SDL_SCANCODE_RETURN);
    unsigned char raw[sizeof(WPADFSStatus)];
    Until([&] {
        std::memset(raw, 0xA5, sizeof raw);
        WPADRead(0, reinterpret_cast<WPADStatus*>(raw));
        const auto* core = reinterpret_cast<WPADStatus*>(raw);
        return core->err == WPAD_ERR_OK && core->dev == WPAD_DEV_FREESTYLE && (core->button & WPAD_BUTTON_A);
    }, "Core-format report lost the identified device or remote A");
    for (std::size_t n = sizeof(WPADStatus); n != sizeof raw; ++n)
        Check(raw[n] == 0xA5, "Core-format read copied the freestyle layout");
    Check(reinterpret_cast<WPADStatus*>(raw)->button == WPAD_BUTTON_A, "Core format reported a Nunchuk button");
    Event(SDL_EVENT_KEY_UP, SDL_SCANCODE_C);
    Event(SDL_EVENT_KEY_UP, SDL_SCANCODE_RETURN);

    Check(WPADSetDataFormat(0, WPAD_FMT_CLASSIC_BTN_ACC_DPD) == WPAD_ERR_INVALID,
          "Classic format admitted without a classic device");
    Check(WPADSetDataFormat(0, WPAD_FMT_FS_BTN_ACC_DPD) == WPAD_ERR_OK, "Freestyle format was rejected");
    Until([&] { auto r = FreestyleReport(); return r.fsAccZ == 200; }, "Freestyle report lacks level gravity");
    auto idle = FreestyleReport();
    Check(idle.fsStickX == 0 && idle.fsStickY == 0 && idle.fsAccX == 0 && idle.fsAccY == 0 && idle.button == 0,
          "Idle Nunchuk manufactured input");
    auto stick = [&](int x, int y, const char* message) {
        Until([&] { auto r = FreestyleReport(); return r.fsStickX == x && r.fsStickY == y; }, message);
    };
    Event(SDL_EVENT_KEY_DOWN, SDL_SCANCODE_W);
    stick(0, 100, "W did not push the stick fully up");
    Event(SDL_EVENT_KEY_DOWN, SDL_SCANCODE_D);
    stick(71, 71, "W+D left the circular gate");
    Event(SDL_EVENT_KEY_DOWN, SDL_SCANCODE_A);
    stick(0, 100, "Opposite A+D did not cancel");
    Event(SDL_EVENT_KEY_UP, SDL_SCANCODE_A);
    Event(SDL_EVENT_KEY_UP, SDL_SCANCODE_D);
    Event(SDL_EVENT_KEY_UP, SDL_SCANCODE_W);
    Event(SDL_EVENT_KEY_DOWN, SDL_SCANCODE_S);
    stick(0, -100, "S did not push the stick fully down");
    Event(SDL_EVENT_KEY_UP, SDL_SCANCODE_S);
    stick(0, 0, "Released stick kept a deflection");
    Event(SDL_EVENT_KEY_DOWN, SDL_SCANCODE_C);
    Until([&] { return FreestyleReport().button == WPAD_BUTTON_FS_C; }, "C did not press Nunchuk C");
    Event(SDL_EVENT_KEY_DOWN, SDL_SCANCODE_V);
    Until([&] { return FreestyleReport().button == (WPAD_BUTTON_FS_C | WPAD_BUTTON_FS_Z); },
          "V did not press Nunchuk Z");
    Event(SDL_EVENT_KEY_UP, SDL_SCANCODE_C);
    Event(SDL_EVENT_KEY_UP, SDL_SCANCODE_V);
    // Q flicks the Nunchuk and E the Remote along X: +2.5 g, then -2.5 g, then
    // rest. A tap released before the next service still flicks once.
    Until([&] { auto r = FreestyleReport(); return r.accX == 0 && r.accZ == 100; }, "Remote is not level at rest");
    Event(SDL_EVENT_KEY_DOWN, SDL_SCANCODE_Q);
    Event(SDL_EVENT_KEY_UP, SDL_SCANCODE_Q);
    Until([&] { return FreestyleReport().fsAccX == 500; }, "Q did not flick the Nunchuk");
    Check(FreestyleReport().accX == 0, "A Nunchuk flick moved the Remote");
    Until([&] { return FreestyleReport().fsAccX == -500; }, "The Nunchuk flick did not swing back");
    Until([&] { auto r = FreestyleReport(); return r.fsAccX == 0 && r.fsAccZ == 200; },
          "The Nunchuk flick did not come to rest");
    Event(SDL_EVENT_KEY_DOWN, SDL_SCANCODE_E);
    Event(SDL_EVENT_KEY_UP, SDL_SCANCODE_E);
    Until([&] { return FreestyleReport().accX == -250; }, "E did not flick the Remote");
    Check(FreestyleReport().fsAccX == 0, "A Remote flick moved the Nunchuk");
    Until([&] { return FreestyleReport().accX == 250; }, "The Remote flick did not swing back");
    Until([&] { auto r = FreestyleReport(); return r.accX == 0 && r.accZ == 100; },
          "The Remote flick did not come to rest");
    Event(SDL_EVENT_KEY_DOWN, SDL_SCANCODE_RETURN);
    Until([&] { return FreestyleReport().button == WPAD_BUTTON_A; }, "Remote A changed with the Nunchuk");
    Event(SDL_EVENT_KEY_UP, SDL_SCANCODE_RETURN);
    Event(SDL_EVENT_KEY_DOWN, SDL_SCANCODE_W);
    Event(SDL_EVENT_KEY_DOWN, SDL_SCANCODE_C);
    stick(0, 100, "Stick did not move before focus loss");
    Event(SDL_EVENT_WINDOW_FOCUS_LOST);
    Until([&] { auto r = FreestyleReport(); return r.fsStickY == 0 && r.button == 0 && r.fsAccZ == 200; },
          "Focus loss retained a Nunchuk deflection or press");
    mscharged::platform::ShutdownDesktopWpad();
    WPADShutdown();
}
// Stick directions as buttons (issue #14): the right stick presses the D-pad.
void StickDirectionCycle() {
    using namespace mscharged::platform;
    Check(ParseGamepadBinding("dpup | rightstick_up") ==
              GamepadBinding{SDL_GAMEPAD_BUTTON_DPAD_UP, kGamepadStickBase + RightStickUp}
              && ParseGamepadInput("leftstick_left") == kGamepadStickBase + LeftStickLeft
              && ParseGamepadInput("rightstick") == SDL_GAMEPAD_BUTTON_RIGHT_STICK
              && FormatGamepadBinding(ParseGamepadBinding("dpright|rightstick_right")) == "dpright | rightstick_right",
          "Stick direction names did not parse or format");
    std::array<std::string, mscharged::GamepadActionCount> texts{};
    texts[mscharged::GamepadActionUp] = "dpup | rightstick_up";
    texts[mscharged::GamepadActionDown] = "dpdown | rightstick_down";
    texts[mscharged::GamepadActionLeft] = "dpleft | rightstick_left";
    texts[mscharged::GamepadActionRight] = "dpright | rightstick_right";
    connects = samples = extension_count = 0;
    ConfigureWpadSDL({0,3,false,false});
    DesktopWpadSettings settings{};
    settings.gamepads = settings.gamepad_players = true;
    settings.gamepad_channels = {1, -1, -1, -1};
    settings.gamepad_profiles[0] = GamepadProfile{ParseGamepadBindings(texts)};
    GenericPad pad;
    InitializeDesktopWpad(window, settings);
    WPADInit();
    for (int n = 0; n != 4; ++n) WPADSetConnectCallback(n, Connect);
    Event(SDL_EVENT_WINDOW_FOCUS_GAINED);
    WPADDeviceType type{};
    Until([&] { return WPADProbe(1, &type) == WPAD_ERR_OK && type == WPAD_DEV_FREESTYLE; },
          "Stick-direction gamepad did not connect as player 2");
    Check(WPADSetDataFormat(1, WPAD_FMT_FS_BTN_ACC_DPD) == WPAD_ERR_OK, "Freestyle format was rejected");
    const auto report = [] {
        WPADFSStatus status{};
        WPADRead(1, reinterpret_cast<WPADStatus*>(&status));
        return status;
    };
    const auto stick = [&](float x, float y) {
        SDL_SetJoystickVirtualAxis(pad.joystick, SDL_GAMEPAD_AXIS_RIGHTX, Sint16(std::lround(x * 32767)));
        SDL_SetJoystickVirtualAxis(pad.joystick, SDL_GAMEPAD_AXIS_RIGHTY, Sint16(std::lround(y * 32767)));
    };
    const auto settle = [] { for (int n = 0; n < 10; ++n) { Service(); SDL_Delay(2); } };
    stick(0, -1);
    Until([&] { return report().button == WPAD_BUTTON_UP; }, "Right stick up did not press D-pad up");
    stick(0, -0.45f);
    settle();
    Check(report().button == WPAD_BUTTON_UP, "Up was let go above 40 % of the stick's travel");
    stick(0, -0.35f);
    Until([&] { return report().button == 0; }, "Up stayed pressed below 40 % of the stick's travel");
    stick(0, -0.45f);
    settle();
    Check(report().button == 0, "Up was pressed below half the stick's travel");
    stick(0.8f, -0.8f);
    Until([&] { return report().button == (WPAD_BUTTON_UP | WPAD_BUTTON_RIGHT); },
          "Right stick up-right did not press up and right");
    stick(-1, 0);
    Until([&] { return report().button == WPAD_BUTTON_LEFT; }, "Right stick left did not press D-pad left");
    stick(0, 1);
    Until([&] { return report().button == WPAD_BUTTON_DOWN; }, "Right stick down did not press D-pad down");
    stick(0, 0);
    Until([&] { return report().button == 0; }, "The resting stick kept a D-pad direction");
    SDL_SetJoystickVirtualButton(pad.joystick, SDL_GAMEPAD_BUTTON_DPAD_UP, true);
    Until([&] { return report().button == WPAD_BUTTON_UP; }, "The D-pad stopped pressing up beside the stick");
    SDL_SetJoystickVirtualButton(pad.joystick, SDL_GAMEPAD_BUTTON_DPAD_UP, false);
    Until([&] { return report().button == 0; }, "D-pad release kept a button");
    ShutdownDesktopWpad();
    WPADShutdown();
}
// The gyro pointer's own sums (issue #14): readings 10 ms apart, SDL's axes.
void GyroPointerChecks() {
    using namespace mscharged::platform;
    constexpr float degree = 3.14159265f / 180.0f;
    const std::array<float, 3> flat{0.0f, 9.80665f, 0.0f};
    std::uint64_t ns = 1'000'000'000;
    const auto feed = [&](GyroMotion& motion, std::array<float, 3> rate, float seconds,
                          const std::array<float, 3>* accel) {
        for (int n = 0; n < int(std::lround(seconds * 100)); ++n) {
            ns += 10'000'000;
            if (accel) AddAccelReading(motion, accel->data(), ns);
            AddGyroReading(motion, rate.data(), ns);
        }
    };
    // Everything a motion has left to show, taken 10 ms at a time.
    const auto shown = [](GyroMotion& motion) {
        GyroTurn sum;
        for (int n = 0; n < 300; ++n) {
            const auto turn = TakeGyroTurn(motion, 0.01f);
            sum.yaw += turn.yaw;
            sum.pitch += turn.pitch;
        }
        return sum;
    };
    const auto start = [&](const std::array<float, 3>* accel = nullptr) {
        GyroMotion motion;
        feed(motion, {}, 0.01f, accel);
        (void)shown(motion);
        return motion;
    };
    // Turning right 5 degrees moves the pointer a fifth of the way across (25 per picture).
    auto motion = start(&flat);
    feed(motion, {0, -5 * degree, 0}, 1.0f, &flat);
    float x = 0.5f, y = 0.5f;
    MoveGyroPointer(x, y, shown(motion), 100, 0.5625f);
    Check(std::fabs(x - 0.7f) < 0.002f && std::fabs(y - 0.5f) < 1e-5f,
          "A 5 degree turn right did not move the pointer a fifth of the picture right");
    // Up and down take the same angle per distance: 2.5 degrees up at 200 %
    // on a 16:9 picture is 2.5 / (12.5 * 0.5625) of its height.
    feed(motion, {5 * degree, 0, 0}, 0.5f, &flat);
    x = y = 0.5f;
    MoveGyroPointer(x, y, shown(motion), 200, 0.5625f);
    Check(std::fabs(y - (0.5f - 2.5f / (12.5f * 0.5625f))) < 0.002f && std::fabs(x - 0.5f) < 1e-5f,
          "Tilting up did not move the pointer up by the same angle per distance");
    // Rolling a flat controller does not move the pointer.
    feed(motion, {0, 0, 10 * degree}, 1.0f, &flat);
    const auto rolled = shown(motion);
    Check(std::fabs(rolled.yaw) < 1e-5f && std::fabs(rolled.pitch) < 1e-5f, "Rolling a flat controller pointed");
    // Held with its front 45 degrees up, a turn about the room's vertical
    // still moves the pointer the whole amount.
    const float c = std::sqrt(0.5f);
    const std::array<float, 3> tilted{0.0f, 9.80665f * c, -9.80665f * c};
    auto held = start(&tilted);
    feed(held, {0, -5 * degree * c, 5 * degree * c}, 1.0f, &tilted);
    x = y = 0.5f;
    MoveGyroPointer(x, y, shown(held), 100, 0.5625f);
    Check(std::fabs(x - 0.7f) < 0.002f && std::fabs(y - 0.5f) < 1e-5f,
          "A tilted controller turning about the vertical did not point the whole amount");
    // A gyro that reads 1 degree a second at rest: after a while still, its
    // offset is taken off and the pointer stops creeping.
    auto resting = start(&flat);
    feed(resting, {0, 1 * degree, 0}, 5.0f, &flat);
    (void)shown(resting);
    feed(resting, {0, 1 * degree, 0}, 5.0f, &flat);
    x = y = 0.5f;
    MoveGyroPointer(x, y, shown(resting), 100, 0.5625f);
    Check(std::fabs(x - 0.5f) < 0.01f && std::fabs(resting.bias[1] - 1 * degree) < 0.2f * degree,
          "The gyro's offset at rest was not taken off");
    // Moving faster than 2 degrees a second is never taken for an offset.
    auto moving = start(&flat);
    feed(moving, {0, -3 * degree, 0}, 5.0f, &flat);
    Check(moving.bias[1] == 0.0f && std::fabs(shown(moving).yaw + 15 * degree) < 0.1f * degree,
          "A slow steady turn was taken for the gyro's offset");
    // Readings after a gap (a pause, a reconnect) add no turn.
    ns += 500'000'000;
    const float fast[3]{0, 90 * degree, 0};
    AddGyroReading(moving, fast, ns);
    Check(std::fabs(shown(moving).yaw) < 1e-6f, "A reading after a gap added a turn");
    // The pointer stops at the edges; speeds stay within 25-300 %.
    x = y = 0.5f;
    MoveGyroPointer(x, y, {-90 * degree, -90 * degree}, 100, 0.5625f);
    Check(x == 1.0f && y == 1.0f, "The pointer left the picture");
    float a = 0.5f, b = 0.5f, ay = 0.5f, by = 0.5f;
    MoveGyroPointer(a, ay, {1 * degree, 0}, 1000, 0.75f);
    MoveGyroPointer(b, by, {1 * degree, 0}, 300, 0.75f);
    Check(a == b, "Gyro speed was not kept within its range");
    // Readings 4 ms apart handed over in bunches of 20 ms every other 10 ms
    // still move the pointer evenly, at the turning speed, and in the end
    // exactly as far as the controller turned. It stops with the controller.
    auto even = start(&flat);
    const float right[3]{0, -30 * degree, 0}, still[3]{};
    float total = 0, least = 1, most = 0;
    for (int take = 0; take < 100; ++take) {
        if (take % 2 == 0)
            for (int n = 0; n < 5; ++n) {
                ns += 4'000'000;
                AddAccelReading(even, flat.data(), ns);
                AddGyroReading(even, right, ns);
            }
        const float yaw = -TakeGyroTurn(even, 0.01f).yaw;
        total += yaw;
        if (take >= 10) least = std::min(least, yaw), most = std::max(most, yaw);
    }
    Check(least > 0.27f * degree && most < 0.33f * degree, "Readings in bunches moved the gyro pointer unevenly");
    float after = 0;
    for (int take = 0; take < 30; ++take) {
        for (int n = 0; n < 2; ++n) {
            ns += 5'000'000;
            AddAccelReading(even, flat.data(), ns);
            AddGyroReading(even, still, ns);
        }
        const float yaw = -TakeGyroTurn(even, 0.01f).yaw;
        if (take >= 2) after = std::max(after, std::fabs(yaw));
        total += yaw;
    }
    Check(after < 0.03f * degree, "The gyro pointer kept moving after the controller stopped");
    Check(std::fabs(total - 30 * degree) < 0.01f * degree, "Turns shown evenly did not add up to the turn");
    // Going back to the middle forgets the turn not shown yet.
    auto cleared = start(&flat);
    feed(cleared, {0, -30 * degree, 0}, 0.5f, &flat);
    (void)TakeGyroTurn(cleared, 0.01f);
    ClearGyroTurn(cleared);
    feed(cleared, {}, 0.1f, &flat);
    Check(std::fabs(shown(cleared).yaw) < 0.05f * degree, "Centring the pointer kept the turn not shown yet");
}
// A gamepad with a gyro and an accelerometer, like a PlayStation controller.
struct GyroPad {
    SDL_JoystickID id{};
    SDL_Joystick* joystick{};
    std::uint64_t ns = 1'000'000'000;
    GyroPad() {
        static const SDL_VirtualJoystickSensorDesc sensors[2]{{SDL_SENSOR_GYRO, 100.0f}, {SDL_SENSOR_ACCEL, 100.0f}};
        SDL_VirtualJoystickDesc d; SDL_INIT_INTERFACE(&d);
        d.type = SDL_JOYSTICK_TYPE_GAMEPAD;
        d.vendor_id = 0x1234; d.product_id = 0x4322;
        d.name = "Generated gyro controller for raw profile gate";
        d.naxes = SDL_GAMEPAD_AXIS_COUNT; d.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
        d.button_mask = (1u << SDL_GAMEPAD_BUTTON_COUNT) - 1;
        d.axis_mask = (1u << SDL_GAMEPAD_AXIS_COUNT) - 1;
        d.nsensors = 2; d.sensors = sensors;
        id = SDL_AttachVirtualJoystick(&d);
        Check(id != 0, "Real SDL gyro fixture attach failed");
        joystick = SDL_OpenJoystick(id);
        Check(joystick, "Real SDL gyro fixture handle failed");
    }
    ~GyroPad() {
        if (joystick) SDL_CloseJoystick(joystick);
        if (id) SDL_DetachVirtualJoystick(id);
    }
    // One reading 10 ms after the last: turning at `yaw` rad/s, lying flat.
    void Turn(float yaw) {
        ns += 10'000'000;
        const float rate[3]{0, yaw, 0}, accel[3]{0, 9.80665f, 0};
        SDL_SendJoystickVirtualSensorData(joystick, SDL_SENSOR_ACCEL, ns, accel, 3);
        SDL_SendJoystickVirtualSensorData(joystick, SDL_SENSOR_GYRO, ns, rate, 3);
    }
};
// Gamepad player 2 pointing with the gyro: the pointer camera's raw objects
// follow the turns, the recentre button and the game turning the camera on
// centre it, and the right stick no longer points. Without a gyro the
// stick points as before.
void GyroPadCycle(bool has_gyro) {
    using namespace mscharged::platform;
    ConfigureWpadSDL({0,3,false,false});
    DesktopWpadSettings settings{};
    settings.gamepads = settings.gamepad_players = true;
    settings.gamepad_channels = {1, -1, -1, -1};
    settings.pointer_projection = WholeWindow;
    settings.gamepad_profiles[0] = GamepadProfile{DefaultGamepadBindings()};
    settings.gamepad_profiles[0].gyro_pointer = true;
    std::unique_ptr<GyroPad> gyro_pad;
    std::unique_ptr<GenericPad> plain_pad;
    if (has_gyro) gyro_pad = std::make_unique<GyroPad>();
    else plain_pad = std::make_unique<GenericPad>();
    SDL_Joystick* joystick = has_gyro ? gyro_pad->joystick : plain_pad->joystick;
    InitializeDesktopWpad(window, settings);
    WPADInit();
    for (int n = 0; n != 4; ++n) WPADSetConnectCallback(n, Connect);
    Event(SDL_EVENT_WINDOW_FOCUS_GAINED);
    WPADDeviceType type{};
    Until([&] { return WPADProbe(1, &type) == WPAD_ERR_OK && type == WPAD_DEV_FREESTYLE; },
          "Gyro gamepad did not connect as player 2");
    Check(WPADSetDataFormat(1, WPAD_FMT_FS_BTN_ACC_DPD) == WPAD_ERR_OK, "Freestyle format was rejected");
    const auto camera = [] {
        WPADFSStatus status{};
        WPADRead(1, reinterpret_cast<WPADStatus*>(&status));
        return status.obj[0];
    };
    const auto centre = MakeDesktopDpdObservation(0.5f, 0.5f, 0, true)[0];
    const auto right = MakeDesktopDpdObservation(0.6f, 0.5f, 0, true)[0];
    const int toward = right.x > centre.x ? 1 : -1;
    const auto moved_right = [&] { return (camera().x - centre.x) * toward >= (right.x - centre.x) * toward; };
    const auto centred = [&] { auto o = camera(); return o.x == centre.x && o.y == centre.y; };
    const auto camera_on = [&](bool on) {
        Check(WPADControlDpd(1, on ? WPAD_DPD_STANDARD : WPAD_DPD_DISABLE, nullptr) == WPAD_ERR_OK,
              "Pointer camera request was rejected");
        Until([&] { return bool(WPADIsDpdEnabled(1)) == on; }, "Pointer camera did not change");
    };
    camera_on(true);
    Until(centred, "The pointer did not start in the middle");
    if (has_gyro) {
        // 20 degrees a second to the right: a tenth of the picture in 0.125 s of readings.
        Until([&] { gyro_pad->Turn(-20.0f * 3.14159265f / 180.0f); return moved_right(); },
              "Turning the controller right did not move the pointer right");
        SDL_SetJoystickVirtualButton(joystick, SDL_GAMEPAD_BUTTON_RIGHT_STICK, true);
        Until(centred, "The recentre button did not centre the gyro pointer");
        SDL_SetJoystickVirtualButton(joystick, SDL_GAMEPAD_BUTTON_RIGHT_STICK, false);
        SDL_SetJoystickVirtualAxis(joystick, SDL_GAMEPAD_AXIS_RIGHTX, 32767);
        for (int n = 0; n < 20; ++n) { Service(); SDL_Delay(2); }
        Check(centred(), "The right stick still moved the gyro pointer");
        SDL_SetJoystickVirtualAxis(joystick, SDL_GAMEPAD_AXIS_RIGHTX, 0);
        Until([&] { gyro_pad->Turn(-20.0f * 3.14159265f / 180.0f); return moved_right(); },
              "The gyro pointer stopped moving");
        // The game turning the camera off and on (a goalie's Mega Strike defence) centres it.
        camera_on(false);
        camera_on(true);
        Until(centred, "Turning the pointer camera on did not centre the gyro pointer");
    } else {
        SDL_SetJoystickVirtualAxis(joystick, SDL_GAMEPAD_AXIS_RIGHTX, 32767);
        Until(moved_right, "Without a gyro the right stick did not move the pointer");
        SDL_SetJoystickVirtualAxis(joystick, SDL_GAMEPAD_AXIS_RIGHTX, 0);
    }
    ShutdownDesktopWpad();
    WPADShutdown();
}
// A key event with modifiers; false when an event filter took it out.
bool Key(Uint32 type, SDL_Scancode key, SDL_Keymod mod, bool repeat = false, SDL_WindowID target = 0) {
    SDL_Event e{}; e.type = type;
    e.key.windowID = target ? target : SDL_GetWindowID(window);
    e.key.scancode = key;
    e.key.mod = mod;
    e.key.down = type == SDL_EVENT_KEY_DOWN;
    e.key.repeat = repeat;
    return SDL_PushEvent(&e);
}
// Hotkeys pressed with Ctrl, Shift or Alt (issue #91): Alt+Enter (Option+Return
// on a Mac) switches the window between fullscreen and a window (issue #77)
// and its Enter never presses the keyboard's A; a hotkey set to another
// combination does its action and keeps its key from the game too.
int gHotkeyActions = 0;
void FullscreenShortcutCycle() {
    using mscharged::platform::FullscreenSwitch;
    using mscharged::platform::ServiceFullscreenHotkey;
    mscharged::platform::ConfigureWpadSDL({0,3,false,false});
    mscharged::platform::DesktopWpadSettings settings{};
    settings.keyboard = true;
    mscharged::platform::InitializeDesktopWpad(window, settings);
    mscharged::platform::SetHotkeys({});
    mscharged::platform::InitializeHotkeys(window);
    WPADInit();
    Check(SDL_ShowWindow(window), "Actual SDL fixture window did not show");
    Event(SDL_EVENT_WINDOW_FOCUS_GAINED);
    WPADDeviceType type{};
    Until([&] { return WPADProbe(0, &type) == WPAD_ERR_OK; }, "Keyboard remote did not connect");
    const auto fullscreen = [] { return (SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN) != 0; };
    Check(!fullscreen() && ServiceFullscreenHotkey() == FullscreenSwitch::None, "The window switched without a press");

    // The window goes fullscreen; Enter's press, repeats and release stay out of the game.
    Check(Key(SDL_EVENT_KEY_DOWN, SDL_SCANCODE_LALT, SDL_KMOD_LALT), "Alt was taken out");
    Check(!Key(SDL_EVENT_KEY_DOWN, SDL_SCANCODE_RETURN, SDL_KMOD_LALT), "Alt+Enter reached the game");
    Check(!Key(SDL_EVENT_KEY_DOWN, SDL_SCANCODE_RETURN, SDL_KMOD_LALT, true), "Alt+Enter's repeat reached the game");
    Check(Key(SDL_EVENT_KEY_UP, SDL_SCANCODE_LALT, SDL_KMOD_NONE), "Alt's release was taken out");
    Check(!Key(SDL_EVENT_KEY_DOWN, SDL_SCANCODE_RETURN, SDL_KMOD_NONE, true),
          "The shortcut's Enter repeated into the game after Alt's release");
    Check(!Key(SDL_EVENT_KEY_UP, SDL_SCANCODE_RETURN, SDL_KMOD_NONE), "The shortcut's Enter release reached the game");
    for (int n = 0; n != 10; ++n) { Service(); SDL_Delay(2); }
    Check(Report().button == 0, "Alt+Enter pressed A");
    Check(ServiceFullscreenHotkey() == FullscreenSwitch::Fullscreen && fullscreen(), "Alt+Enter did not go fullscreen");
    Check(ServiceFullscreenHotkey() == FullscreenSwitch::None, "One press switched twice");

    // Enter alone, and Enter with Ctrl+Alt, stay A.
    for (const SDL_Keymod mod : {SDL_Keymod(SDL_KMOD_NONE), SDL_Keymod(SDL_KMOD_LALT | SDL_KMOD_LCTRL)}) {
        Check(Key(SDL_EVENT_KEY_DOWN, SDL_SCANCODE_RETURN, mod), "Enter without the shortcut was taken out");
        Until([&] { return Report().button == WPAD_BUTTON_A; }, "Enter without the shortcut did not press A");
        Check(Key(SDL_EVENT_KEY_UP, SDL_SCANCODE_RETURN, mod), "Enter's release was taken out");
        Until([&] { return Report().button == 0; }, "Enter's release kept A");
    }
    Check(ServiceFullscreenHotkey() == FullscreenSwitch::None && fullscreen(), "Enter without the shortcut switched");

    // The keypad's Enter goes back to a window. A release lost with the focus
    // does not take the next press, and another window's shortcut is not ours.
    Check(!Key(SDL_EVENT_KEY_DOWN, SDL_SCANCODE_KP_ENTER, SDL_KMOD_RALT), "Alt+keypad Enter reached the game");
    Check(ServiceFullscreenHotkey() == FullscreenSwitch::Windowed && !fullscreen(), "Alt+keypad Enter did not leave fullscreen");
    Check(Key(SDL_EVENT_KEY_DOWN, SDL_SCANCODE_KP_ENTER, SDL_KMOD_NONE), "A new keypad Enter press was taken out");
    Check(Key(SDL_EVENT_KEY_UP, SDL_SCANCODE_KP_ENTER, SDL_KMOD_NONE), "A new keypad Enter release was taken out");
    Check(Key(SDL_EVENT_KEY_DOWN, SDL_SCANCODE_RETURN, SDL_KMOD_LALT, false, SDL_GetWindowID(window) + 1000),
          "Another window's Alt+Enter was taken out");
    Check(ServiceFullscreenHotkey() == FullscreenSwitch::None && !fullscreen(), "Another window's Alt+Enter switched");

    // A hotkey set to Ctrl+Return does its action without pressing A; Return
    // alone stays A, and a fullscreen hotkey set to F11 alone switches.
    mscharged::platform::Hotkeys custom;
    custom[mscharged::HotkeyScreenshot] = {SDL_SCANCODE_RETURN, SDL_KMOD_CTRL};
    custom[mscharged::HotkeyFullscreen] = {SDL_SCANCODE_F11};
    mscharged::platform::SetHotkeys(custom);
    mscharged::platform::SetHotkeyAction(mscharged::HotkeyScreenshot, [] { ++gHotkeyActions; });
    Check(!Key(SDL_EVENT_KEY_DOWN, SDL_SCANCODE_RETURN, SDL_KMOD_RCTRL), "Ctrl+Return reached the game");
    Check(!Key(SDL_EVENT_KEY_UP, SDL_SCANCODE_RETURN, SDL_KMOD_NONE), "Ctrl+Return's release reached the game");
    for (int n = 0; n != 10; ++n) { Service(); SDL_Delay(2); }
    Check(gHotkeyActions == 1 && Report().button == 0, "Ctrl+Return did not do its action alone");
    Check(Key(SDL_EVENT_KEY_DOWN, SDL_SCANCODE_RETURN, SDL_KMOD_LALT), "The former Alt+Enter was taken out");
    Until([&] { return Report().button == WPAD_BUTTON_A; }, "Alt+Enter no longer pressed A");
    Check(Key(SDL_EVENT_KEY_UP, SDL_SCANCODE_RETURN, SDL_KMOD_NONE), "Enter's release was taken out");
    Until([&] { return Report().button == 0; }, "Enter's release kept A");
    Check(!Key(SDL_EVENT_KEY_DOWN, SDL_SCANCODE_F11, SDL_KMOD_NONE), "F11 reached the game");
    Check(!Key(SDL_EVENT_KEY_UP, SDL_SCANCODE_F11, SDL_KMOD_NONE), "F11's release reached the game");
    Check(ServiceFullscreenHotkey() == FullscreenSwitch::Fullscreen && fullscreen(), "F11 did not go fullscreen");
    Check(!Key(SDL_EVENT_KEY_DOWN, SDL_SCANCODE_F11, SDL_KMOD_NONE), "F11 reached the game");
    Check(ServiceFullscreenHotkey() == FullscreenSwitch::Windowed && !fullscreen(), "F11 did not leave fullscreen");
    Check(!Key(SDL_EVENT_KEY_UP, SDL_SCANCODE_F11, SDL_KMOD_NONE), "F11's release reached the game");
    mscharged::platform::SetHotkeyAction(mscharged::HotkeyScreenshot, nullptr);
    mscharged::platform::SetHotkeys({});

    // After shutdown Alt+Enter is A again.
    mscharged::platform::ShutdownHotkeys();
    Check(Key(SDL_EVENT_KEY_DOWN, SDL_SCANCODE_RETURN, SDL_KMOD_LALT), "Alt+Enter was taken out after shutdown");
    Until([&] { return Report().button == WPAD_BUTTON_A; }, "Alt+Enter after shutdown did not press A");
    Check(Key(SDL_EVENT_KEY_UP, SDL_SCANCODE_RETURN, SDL_KMOD_NONE), "Enter's release was taken out after shutdown");
    Until([&] { return Report().button == 0; }, "Enter's release kept A after shutdown");
    Check(ServiceFullscreenHotkey() == FullscreenSwitch::None, "The shortcut switched after shutdown");
    Check(SDL_HideWindow(window), "Actual SDL fixture window did not hide");
    mscharged::platform::ShutdownDesktopWpad();
    WPADShutdown();
}
}
#if defined(MSCHARGED_AUTOPILOT)
// The autopilot's chord (test builds): C, P and U held
// together act once, in any order, and none of them reaches the game or takes
// a screenshot. Alone, C still presses Nunchuk C (60 ms late, a short tap too)
// and P still takes its screenshot (at its release); a C already in the game
// keeps its release. Without the chord the keys are as ever.
int gChords = 0, gScreenshots = 0;
void AutopilotChordCycle() {
    mscharged::platform::ConfigureWpadSDL({0,3,false,false});
    mscharged::platform::DesktopWpadSettings settings{};
    settings.keyboard = true;
    settings.nunchuk = true;
    mscharged::platform::InitializeDesktopWpad(window, settings);
    mscharged::platform::SetHotkeys({});
    mscharged::platform::InitializeHotkeys(window);
    mscharged::platform::SetHotkeyAction(mscharged::HotkeyScreenshot, [] { ++gScreenshots; });
    WPADInit();
    Check(SDL_ShowWindow(window), "Actual SDL fixture window did not show");
    Event(SDL_EVENT_WINDOW_FOCUS_GAINED);
    WPADDeviceType type{};
    Until([&] { return WPADProbe(0, &type) == WPAD_ERR_OK && type == WPAD_DEV_FREESTYLE; },
          "Keyboard remote and Nunchuk did not connect");
    Check(WPADSetDataFormat(0, WPAD_FMT_FS_BTN_ACC_DPD) == WPAD_ERR_OK, "Freestyle format was rejected");
    const auto c = [] { return (FreestyleReport().button & WPAD_BUTTON_FS_C) != 0; };
    const auto none = [&] { return !c(); };
    const auto settle = [] { for (int n = 0; n != 50; ++n) { Service(); SDL_Delay(2); } };
    constexpr auto down = SDL_EVENT_KEY_DOWN, up = SDL_EVENT_KEY_UP;
    constexpr auto C = SDL_SCANCODE_C, P = SDL_SCANCODE_P, U = SDL_SCANCODE_U;

    // Without the chord: C at once, the screenshot at P's press.
    Check(Key(down, C, SDL_KMOD_NONE), "C was taken out without the chord");
    Until(c, "C did not press Nunchuk C");
    Check(Key(up, C, SDL_KMOD_NONE), "C's release was taken out without the chord");
    Until(none, "C's release kept Nunchuk C");
    Check(!Key(down, P, SDL_KMOD_NONE) && gScreenshots == 1, "P did not take its screenshot at its press");
    Check(!Key(up, P, SDL_KMOD_NONE), "P's release reached the game");

    mscharged::platform::SetAutopilotChord([] { ++gChords; });
    // C alone: held back for the chord, then in the game; its release follows at once.
    Check(!Key(down, C, SDL_KMOD_NONE), "C reached the game before the chord's wait");
    Service();
    Check(!c(), "C pressed Nunchuk C during the chord's wait");
    Until(c, "C never reached the game");
    Check(Key(up, C, SDL_KMOD_NONE), "C's release was taken out");
    Until(none, "C's release kept Nunchuk C");
    // A tap shorter than the wait still presses C once.
    Check(!Key(down, C, SDL_KMOD_NONE) && !Key(up, C, SDL_KMOD_NONE), "A short C tap reached the game at once");
    Until(c, "A short C tap was lost");
    Until(none, "A short C tap's release was lost");
    // P alone: the screenshot at its release.
    Check(!Key(down, P, SDL_KMOD_NONE) && gScreenshots == 1, "P took its screenshot at its press");
    Check(!Key(up, P, SDL_KMOD_NONE) && gScreenshots == 2, "P did not take its screenshot at its release");

    // The chord, in two orders: once each, no C, no screenshot.
    for (const auto& keys : {std::array{C, P, U}, std::array{P, U, C}}) {
        const int before = gChords;
        for (const auto key : keys) Check(!Key(down, key, SDL_KMOD_NONE), "A chord key reached the game");
        Check(!Key(down, keys[2], SDL_KMOD_NONE, true), "A chord key's repeat reached the game");
        Check(gChords == before + 1, "The chord did not act once");
        settle();
        Check(!c(), "The chord pressed Nunchuk C");
        // Its last key again, before all three were up: no second act.
        Check(!Key(up, keys[2], SDL_KMOD_NONE) && !Key(down, keys[2], SDL_KMOD_NONE) && gChords == before + 1,
              "The chord acted twice in one hold");
        for (const auto key : keys) Check(!Key(up, key, SDL_KMOD_NONE), "A chord key's release reached the game");
        settle();
        Check(!c() && gScreenshots == 2, "The chord pressed C or took a screenshot");
    }
    // A slow chord: C was already in the game and keeps its release.
    Check(!Key(down, C, SDL_KMOD_NONE), "C reached the game before the chord's wait");
    Until(c, "C never reached the game");
    const int before = gChords;
    Check(!Key(down, U, SDL_KMOD_NONE) && !Key(down, P, SDL_KMOD_NONE) && gChords == before + 1,
          "A slow chord did not act");
    Check(Key(up, C, SDL_KMOD_NONE), "C's release stayed out of the game after a slow chord");
    Until(none, "C stayed pressed after a slow chord");
    Check(!Key(up, U, SDL_KMOD_NONE) && !Key(up, P, SDL_KMOD_NONE) && gScreenshots == 2,
          "A slow chord's keys reached the game or took a screenshot");

    // Without the chord again, C and P are as ever.
    mscharged::platform::SetAutopilotChord(nullptr);
    Check(Key(down, C, SDL_KMOD_NONE), "C was taken out after the chord went");
    Until(c, "C did not press Nunchuk C after the chord went");
    Check(Key(up, C, SDL_KMOD_NONE), "C's release was taken out after the chord went");
    Until(none, "C's release kept Nunchuk C after the chord went");
    Check(!Key(down, P, SDL_KMOD_NONE) && gScreenshots == 3, "P's screenshot did not come back to its press");
    Check(!Key(up, P, SDL_KMOD_NONE), "P's release reached the game");
    mscharged::platform::SetHotkeyAction(mscharged::HotkeyScreenshot, nullptr);
    mscharged::platform::ShutdownHotkeys();
    Check(SDL_HideWindow(window), "Actual SDL fixture window did not hide");
    mscharged::platform::ShutdownDesktopWpad();
    WPADShutdown();
}
#endif
int main() {
    try {
        owner = std::this_thread::get_id();
        Check(SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD), "Actual SDL dummy setup failed");
        window = SDL_CreateWindow("Selected desktop raw profile",640,448,SDL_WINDOW_HIDDEN);
        Check(window, "Actual SDL fixture window creation failed");
        {
            GenericPad generated;
            Cycle(false,false);
            Cycle(true,true);
            NunchukCycle();
            FixedPlayersCycle();
            CustomKeysCycle();
        }
        // With the generic pad above gone, this test's pad is gamepad 1.
        GamepadPlayerCycle();
        StickDeadzoneChecks();
        CustomGamepadCycle();
        ControllerFamilyCycle();
        MousePlayerCycle();
        MouseKeysCycle();
        StickDirectionCycle();
        GyroPointerChecks();
        GyroPadCycle(true);
        GyroPadCycle(false);
        FullscreenShortcutCycle();
#if defined(MSCHARGED_AUTOPILOT)
        AutopilotChordCycle();
#endif
        SDL_DestroyWindow(window); window = nullptr;
        SDL_Quit();
        std::printf("Raw keyboard-first WPAD profile: %u checks; selected keyboard, keyboard Nunchuk, fixed players, custom keys, gamepad players, custom gamepad layouts, stick dead zones and trigger press points, controller-family defaults, mouse beside a controller, mouse buttons as keys, stick directions as buttons, the gyro pointer, the fullscreen shortcut and retained generic profile pass. No original FE lifecycle/game acceptance.\n", checks);
        return 0;
    } catch (const std::exception& e) {
        mscharged::platform::ShutdownHotkeys();
        try { mscharged::platform::ShutdownDesktopWpad(); } catch (...) {}
        try { WPADShutdown(); } catch (...) {}
        if (window) SDL_DestroyWindow(window);
        SDL_Quit();
        std::fprintf(stderr,"Raw keyboard profile: %s\n",e.what());
        return 1;
    }
}
