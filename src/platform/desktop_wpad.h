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
    // Attach a virtual Nunchuk to the keyboard remote: W/A/S/D move its stick
    // and C/V press C/Z. Requires the keyboard profile.
    bool nunchuk = false;
};

// Native desktop buttons are carried by explicit SDL virtual core-Wii devices.
// The existing native WPAD and whole original KPAD/game pads consume their raw
// reports. This profile supplies digital buttons and neutral gravity only;
// gamepad analog/Nunchuk, Classic and motion remain separate prerequisites.
// The opt-in keyboard Nunchuk reports a digital stick (full deflection on a
// circular gate), C/Z and a level Nunchuk's gravity through native WPAD's
// extension sequence; original KPAD and game pads keep every stick decision.
// Opt-in mouse supplies an upright virtual raw IR camera only with an actual
// successful-Present projection. Physical remote IR is still unqualified.
// Focus policy: buttons and camera objects are reported only while the window
// has input focus. Focus loss, hide or minimize reports released buttons and no
// camera objects through the same raw path; a key pressed without focus stays
// released until pressed again. Mouse leave releases mouse buttons and hides
// the camera until a new in-window position arrives. KPAD keeps every edge,
// repeat and invalid-pointer decision. Actual keyboard/mouse removal retires
// only that SDL instance's latched keys/buttons; a retained pointer position is
// invalidated when its producing mouse leaves. Other live instances keep their
// contributions and the virtual core-Wii device remains connected.
void InitializeDesktopWpad(SDL_Window* window, DesktopWpadSettings settings);
void ServiceDesktopWpad();
void ShutdownDesktopWpad();
} // namespace mscharged::platform
