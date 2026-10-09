#include "platform/desktop_wpad.h"
#include "platform/wpad_sdl.h"
#include "platform/interrupts.h"
#include <SDL3/SDL.h>
#include <revolution/wpad/WPAD.h>
#include <dolphin/os.h>

#include <chrono>
#include <cstdio>
#include <cstring>
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
template<class F> void Until(F fn, const char* message) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
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
        }
        SDL_DestroyWindow(window); window = nullptr;
        SDL_Quit();
        std::printf("Raw keyboard-first WPAD profile: %u checks; selected keyboard, keyboard Nunchuk and retained generic profile pass. No original FE lifecycle/game acceptance.\n", checks);
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
