#pragma once

#include <cstdint>

struct SDL_Window;

namespace mscharged::platform {
// The caller must supply the rectangle of a successful native Present in
// SDL window coordinates. A desired resize or EFB size is not a projection.
struct DesktopDpdProjection {
    std::uint64_t presented_revision{};
    float left{}, top{}, width{}, height{};
};
using DesktopDpdProjectionQuery = bool (*)(void*, SDL_Window*, DesktopDpdProjection*);

struct DesktopWpadSettings {
    bool keyboard = false;
    bool gamepads = false;
    bool mouse = false;
    DesktopDpdProjectionQuery pointer_projection = nullptr;
    void* pointer_projection_context = nullptr;
};

// Native desktop buttons are carried by explicit SDL virtual core-Wii devices.
// The existing native WPAD and whole original KPAD/game pads consume their raw
// reports. This profile supplies digital buttons and neutral gravity only;
// analog/Nunchuk/Classic/motion remain separate hardware prerequisites.
// Opt-in mouse supplies an upright virtual raw IR camera only with an actual
// successful-Present projection. Physical remote IR is still unqualified.
void InitializeDesktopWpad(SDL_Window* window, DesktopWpadSettings settings);
void ServiceDesktopWpad();
void ShutdownDesktopWpad();
} // namespace mscharged::platform
