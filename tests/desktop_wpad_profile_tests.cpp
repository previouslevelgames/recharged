#include "platform/desktop_wpad.h"
#include "platform/wpad_sdl.h"
#include "platform/interrupts.h"
#include <SDL3/SDL.h>
#include <revolution/wpad/WPAD.h>
#include <dolphin/os.h>

#include <chrono>
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
    settings.keys[mscharged::KeyActionA] = mscharged::platform::ParseKeyBinding("F1");
    settings.keys[mscharged::KeyActionB] = mscharged::platform::ParseKeyBinding("Return | P");
    Check(settings.keys[mscharged::KeyActionB][1] == SDL_SCANCODE_UNKNOWN, "The screenshot key was bound to an action");
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
    Until([&] { auto r = report(); return r.fsStickX == 100 && r.fsStickY == 0 && r.button == 0; },
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
    settings.gamepad_profiles[0] = {bindings, true};
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
    Until([&] { auto r = report(); return r.fsStickX == 100 && r.button == 0; },
          "Swapped sticks: the right stick did not move the Nunchuk stick");
    SDL_SetJoystickVirtualAxis(pad.joystick, SDL_GAMEPAD_AXIS_RIGHTX, 0);
    SDL_SetJoystickVirtualAxis(pad.joystick, SDL_GAMEPAD_AXIS_LEFTX, 32767);
    Until([&] { return report().fsStickX == 0; }, "Swapped sticks: the left stick still moved the Nunchuk stick");
    SDL_SetJoystickVirtualAxis(pad.joystick, SDL_GAMEPAD_AXIS_LEFTX, 0);
    SDL_SetJoystickVirtualAxis(pad.joystick, SDL_GAMEPAD_AXIS_LEFT_TRIGGER, 32767);
    Until([&] { return report().button == WPAD_BUTTON_FS_C; }, "Own layout: left trigger did not press C");
    SDL_SetJoystickVirtualAxis(pad.joystick, SDL_GAMEPAD_AXIS_LEFT_TRIGGER, 0);
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
}
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
        CustomGamepadCycle();
        ControllerFamilyCycle();
        MousePlayerCycle();
        SDL_DestroyWindow(window); window = nullptr;
        SDL_Quit();
        std::printf("Raw keyboard-first WPAD profile: %u checks; selected keyboard, keyboard Nunchuk, fixed players, custom keys, gamepad players, custom gamepad layouts, controller-family defaults, mouse beside a controller and retained generic profile pass. No original FE lifecycle/game acceptance.\n", checks);
        return 0;
    } catch (const std::exception& e) {
        try { mscharged::platform::ShutdownDesktopWpad(); } catch (...) {}
        try { WPADShutdown(); } catch (...) {}
        if (window) SDL_DestroyWindow(window);
        SDL_Quit();
        std::fprintf(stderr,"Raw keyboard profile: %s\n",e.what());
        return 1;
    }
}
