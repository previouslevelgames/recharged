#pragma once

#include "platform/gamepad_bindings.h"
#include "platform/key_bindings.h"

#include <array>
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
    // Lend the mouse camera to physical Wii Remotes, which SDL reports without
    // IR data, so they can point at original menus. Requires the mouse profile.
    bool share_mouse_with_remotes = false;
    // WPAD channel (player - 1) of the keyboard & mouse remote; -1 takes the
    // lowest free channel.
    int keyboard_channel = -1;
    // Keys of each keyboard action ([keyboard] in mscharged.ini).
    KeyBindings keys = DefaultKeyBindings();
    // Standard gamepads (Xbox, PlayStation, Switch Pro...) as a Wii Remote
    // with Nunchuk: the n-th connected pad plays on WPAD channel
    // gamepad_channels[n]; -1 leaves that pad out. Requires `gamepads` and,
    // for the right-stick pointer, `pointer_projection`. Without
    // `gamepad_players` every pad takes the lowest free channel, buttons only.
    bool gamepad_players = false;
    std::array<int, 4> gamepad_channels{-1, -1, -1, -1};
    // Buttons and sticks of the n-th gamepad (its profile, controls.gamepadN_profile).
    std::array<GamepadProfile, 4> gamepad_profiles{
        GamepadProfile{DefaultGamepadBindings()}, GamepadProfile{DefaultGamepadBindings()},
        GamepadProfile{DefaultGamepadBindings()}, GamepadProfile{DefaultGamepadBindings()}};
    // controls.mouse_pointer: with keyboard & mouse not playing, the mouse
    // also points and presses A/B for the controller on this WPAD channel
    // while it is in use (moving or a button held, then 3 s). -1: off.
    // Requires `mouse` without `keyboard`.
    int mouse_player_channel = -1;
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
