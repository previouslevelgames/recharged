// Physical Wii Remote + Nunchuk transport through the existing native WPAD.
// SDL's HIDAPI Wii driver renames a remote whose extension is a Nunchuk to
// "Nintendo Wii Remote with Nunchuk" and reports it as the left stick, the C
// button (left shoulder), the Z trigger axis and an ACCEL_L sensor, in
// addition to the core remote's MISC buttons and ACCEL sensor. This test
// emulates exactly that SDL device identity/shape with a virtual joystick and
// checks the raw WPAD freestyle reports and extension handshake. It also checks
// the desktop mouse camera lent to remotes without their own camera source.
#include "platform/desktop_dpd.h"
#include "platform/desktop_nunchuk.h"
#include "platform/desktop_wpad.h"
#include "platform/hardware_owner.h"
#include "platform/interrupts.h"
#include "platform/stm_device.h"
#include "platform/wpad_sdl.h"
#include <aurora/aurora.h>
#include <dolphin/os.h>
#include <revolution/wpad/WPAD.h>
#include <SDL3/SDL.h>

#include <chrono>
#include <cstdio>
#include <stdexcept>
#include <thread>

namespace aurora { extern AuroraConfig g_config; }
void AuroraOSShutdown();

namespace {
using namespace mscharged;
unsigned checks{}, connected{}, disconnected{}, extensions{}, dpd_done{};
s32 last_extension = -1;
WPADResult dpd_result = WPAD_ERR_INVALID;
void Check(bool value, const char* message) { ++checks; if (!value) throw std::runtime_error(message); }
void Connect(s32, WPADResult result) { (result == WPAD_ERR_OK ? connected : disconnected) += 1; }
void Extension(s32, s32 device) { ++extensions; last_extension = device; }
void DpdDone(s32, WPADResult result) { ++dpd_done; dpd_result = result; }
void Sample(s32) {}
bool SDLCALL Sensors(void*, bool) { return true; }

struct Remote {
    SDL_JoystickID id{}; SDL_Joystick* joystick{};
    explicit Remote(bool nunchuk) {
        static const SDL_VirtualJoystickSensorDesc sensors[2]{{SDL_SENSOR_ACCEL, 100.0f}, {SDL_SENSOR_ACCEL_L, 100.0f}};
        SDL_VirtualJoystickDesc desc; SDL_INIT_INTERFACE(&desc);
        desc.type = SDL_JOYSTICK_TYPE_GAMEPAD; desc.vendor_id = 0x057e; desc.product_id = 0x0306;
        desc.naxes = SDL_GAMEPAD_AXIS_COUNT; desc.nbuttons = int(SDL_GAMEPAD_BUTTON_MISC1) + 11;
        desc.button_mask = (1u << SDL_GAMEPAD_BUTTON_COUNT) - 1; desc.axis_mask = (1u << SDL_GAMEPAD_AXIS_COUNT) - 1;
        desc.name = nunchuk ? "Nintendo Wii Remote with Nunchuk" : "Nintendo Wii Remote";
        desc.nsensors = nunchuk ? 2 : 1; desc.sensors = sensors; desc.SetSensorsEnabled = Sensors;
        id = SDL_AttachVirtualJoystick(&desc); Check(id != 0, "Cannot attach emulated SDL Wii remote");
        joystick = SDL_OpenJoystick(id); Check(joystick, "Cannot open emulated SDL Wii remote");
    }
    void Axis(SDL_GamepadAxis axis, Sint16 value) {
        Check(SDL_SetJoystickVirtualAxis(joystick, axis, value), "Cannot move emulated axis"); SDL_UpdateJoysticks();
    }
    void Button(int button, bool down) {
        Check(SDL_SetJoystickVirtualButton(joystick, button, down), "Cannot press emulated button"); SDL_UpdateJoysticks();
    }
    void Motion(SDL_SensorType type, float x, float y, float z) {
        const float values[3]{x, y, z};
        Check(SDL_SendJoystickVirtualSensorData(joystick, type, SDL_GetTicksNS(), values, 3), "Cannot send emulated motion");
        SDL_UpdateJoysticks();
    }
    void Detach() {
        SDL_CloseJoystick(joystick); joystick = nullptr;
        Check(SDL_DetachVirtualJoystick(id), "Cannot detach emulated SDL Wii remote"); id = 0;
    }
};
WPADFSStatus Read() { WPADFSStatus report{}; WPADRead(0, reinterpret_cast<WPADStatus*>(&report)); return report; }
template<class F> void Until(Remote* remote, F fn, const char* message) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!fn()) {
        // A remote report is produced for each core accelerometer sample.
        if (remote && remote->joystick) remote->Motion(SDL_SENSOR_ACCEL, 0.0f, 9.80665f, 0.0f);
        (void)OSGetTime(); platform::ServiceNativeHardwareInput();
        if (std::chrono::steady_clock::now() >= end) {
            std::fprintf(stderr, "Timeout: connects=%u disconnects=%u extensions=%u channels=%zu err=%d dev=%d irq=%d\n",
                connected, disconnected, extensions, platform::WpadSDLConnectedChannels(), int(Read().err),
                int(Read().dev), int(platform::NativeInterruptsEnabled()));
            int count{}; auto* ids = SDL_GetGamepads(&count);
            for (int n = 0; n < count; ++n) std::fprintf(stderr, "SDL gamepad %u %04x:%04x %s sensors accel=%d\n", ids[n],
                SDL_GetGamepadVendorForID(ids[n]), SDL_GetGamepadProductForID(ids[n]), SDL_GetGamepadNameForID(ids[n]),
                int(SDL_GetNumJoystickButtons(SDL_GetJoystickFromID(ids[n]))));
            SDL_free(ids);
            throw std::runtime_error(message);
        }
        SDL_Delay(2);
    }
    ++checks;
}
}

int main() {
    SDL_Window* window{};
    try {
        // The hidden fixture window never has focus; deliver device events anyway,
        // as the desktop profile does for the focused game window.
        Check(SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1"), "Cannot allow background events");
        Check(SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD), "Cannot initialize SDL dummy devices");
        window = SDL_CreateWindow("Physical Nunchuk transport", 640, 448, SDL_WINDOW_HIDDEN);
        Check(window, "Cannot create SDL fixture window");
        aurora::g_config.mem1Size = MEM1_DEFAULT_SIZE;
        aurora::g_config.mem2Size = 64u * 1024u * 1024u;
        OSInit();
        platform::InitializeNativeSTMDevice();
        platform::InitializeNativeHardwareInput(window, {0, 3}, platform::GetNativeSTMInput(), {false, false});
        WPADInit();
        for (int n = 0; n < 4; ++n) {
            WPADSetConnectCallback(n, Connect); WPADSetExtensionCallback(n, Extension);
            WPADSetSamplingCallback(n, Sample); // original KPAD installs one
        }

        Remote remote(true);
        Until(&remote, [] { return connected == 1 && Read().err == WPAD_ERR_OK; },
            "Remote with Nunchuk was not discovered on channel 0");
        Check(WPADSetDataFormat(0, WPAD_FMT_FS_BTN_ACC) == WPAD_ERR_OK, "Freestyle report format rejected");
        Until(&remote, [] { return last_extension == WPAD_DEV_FREESTYLE && Read().dev == WPAD_DEV_FREESTYLE; },
            "Nunchuk extension handshake did not reach FREESTYLE");
        Check(extensions == 2, "Handshake must report INITIALIZING then FREESTYLE");
        Check(Read().fsAccZ == platform::kNativeNunchukGravity && Read().fsStickX == 0 && Read().fsStickY == 0,
            "Untouched Nunchuk must be level and centred");

        // Full right and up (SDL inverts Y), C and Z held, 2 g sideways shake.
        remote.Axis(SDL_GAMEPAD_AXIS_LEFTX, 32767);
        remote.Axis(SDL_GAMEPAD_AXIS_LEFTY, -32768);
        remote.Button(SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, true);
        remote.Axis(SDL_GAMEPAD_AXIS_LEFT_TRIGGER, 32767);
        remote.Motion(SDL_SENSOR_ACCEL_L, -2.0f * 9.80665f, 9.80665f, 0.0f);
        Until(&remote, [] {
            const auto r = Read();
            return r.fsStickX == 100 && r.fsStickY == 100 && (r.button & WPAD_BUTTON_FS_C) &&
                (r.button & WPAD_BUTTON_FS_Z) && r.fsAccX == 400 && r.fsAccY == 0 && r.fsAccZ == 200;
        }, "Nunchuk stick/C/Z/motion did not reach the freestyle report");
        remote.Axis(SDL_GAMEPAD_AXIS_LEFTX, -16384);
        remote.Button(SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, false);
        remote.Axis(SDL_GAMEPAD_AXIS_LEFT_TRIGGER, -32768);
        Until(&remote, [] {
            const auto r = Read();
            return r.fsStickX == -50 && !(r.button & (WPAD_BUTTON_FS_C | WPAD_BUTTON_FS_Z));
        }, "Nunchuk release/half tilt did not reach the freestyle report");
        remote.Button(SDL_GAMEPAD_BUTTON_MISC1, true);
        Until(&remote, [] { return (Read().button & WPAD_BUTTON_A) != 0; }, "Remote A lost beside the Nunchuk");

        // No IR from SDL: no camera until the desktop mouse camera is lent.
        Check(WPADControlDpd(0, WPAD_DPD_BASIC, DpdDone) == WPAD_ERR_INVALID && !WPADIsDpdEnabled(0),
            "Camera enabled without any camera source");
        auto pointer = platform::MakeDesktopDpdObservation(0.5f, 0.5f, 0, true);
        platform::SetNativeWpadSharedPointer(&pointer);
        Check(WPADControlDpd(0, WPAD_DPD_BASIC, DpdDone) == WPAD_ERR_OK, "Lent camera rejected");
        Until(&remote, [] { return dpd_done >= 2 && dpd_result == WPAD_ERR_OK && WPADIsDpdEnabled(0); },
            "Lent camera command did not complete");
        Until(&remote, [] { return Read().obj[0].size != 0; }, "Lent camera objects missing from reports");
        platform::SetNativeWpadSharedPointer(nullptr);
        Check(!WPADIsDpdEnabled(0), "Withdrawn camera stayed enabled");

        // SDL re-enumerates when the Nunchuk is unplugged: a bare remote.
        remote.Detach();
        Until(nullptr, [] { return disconnected == 1; }, "Unplug did not disconnect the remote");
        Remote bare(false);
        Until(&bare, [] { return connected == 2 && Read().err == WPAD_ERR_OK; }, "Bare remote did not reconnect");
        Check(WPADSetDataFormat(0, WPAD_FMT_FS_BTN_ACC) == WPAD_ERR_OK, "Format rejected after reconnect");
        Until(&bare, [] { return Read().dev == WPAD_DEV_CORE; }, "Bare remote reported an extension");
        bare.Detach();
        Until(nullptr, [] { return disconnected == 2; }, "Bare remote unplug was not reported");

        platform::ShutdownNativeHardwareInput();
        platform::ShutdownNativeSTMDevice();
        SDL_DestroyWindow(window); window = nullptr; SDL_Quit();
        AuroraOSShutdown();
        std::printf("Physical Wii Remote + Nunchuk transport: %u checks (emulated SDL Wii HID device shape).\n", checks);
        return 0;
    } catch (const std::exception& error) {
        try { platform::ShutdownNativeHardwareInput(); } catch (...) {}
        try { platform::ShutdownNativeSTMDevice(); } catch (...) {}
        if (window) SDL_DestroyWindow(window);
        SDL_Quit();
        AuroraOSShutdown();
        std::fprintf(stderr, "Physical Wii Remote + Nunchuk transport: %s\n", error.what());
        return 1;
    }
}
