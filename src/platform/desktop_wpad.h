#pragma once

struct SDL_Window;

namespace mscharged::platform {
struct DesktopWpadSettings {
    bool keyboard = false;
    bool gamepads = false;
};

// Native desktop buttons are carried by explicit SDL virtual core-Wii devices.
// The existing native WPAD and whole original KPAD/game pads consume their raw
// reports. This profile supplies digital buttons and neutral gravity only;
// analog/Nunchuk/Classic/motion/IR gameplay are separate hardware prerequisites.
void InitializeDesktopWpad(SDL_Window* window, DesktopWpadSettings settings);
void ServiceDesktopWpad();
void ShutdownDesktopWpad();
} // namespace mscharged::platform
