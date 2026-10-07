#include "platform/desktop_wpad.h"
#include "platform/wpad_sdl.h"
#include "platform/interrupts.h"
#include <SDL3/SDL.h>
#include <revolution/wpad/WPAD.h>
#include <dolphin/os.h>

#include <chrono>
#include <cstdio>
#include <stdexcept>
#include <thread>

namespace {
unsigned checks{}, connects{}, samples{};
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
    mscharged::platform::ConfigureWpadSDL({0,3,physical});
    mscharged::platform::InitializeDesktopWpad(window, {true,gamepads});
    WPADInit();
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
        }
        SDL_DestroyWindow(window); window = nullptr;
        SDL_Quit();
        std::printf("Raw keyboard-first WPAD profile: %u checks; selected keyboard and retained generic profile pass. No original FE lifecycle/game acceptance.\n", checks);
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
