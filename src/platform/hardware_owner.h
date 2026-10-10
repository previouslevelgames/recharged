#pragma once

#include "platform/wpad_sdl.h"
#include "platform/desktop_wpad.h"
#include "platform/stm_device.h"
#include "platform/wiimote_calibration.h"

#include <array>
#include <optional>

struct SDL_Window;

namespace mscharged::platform {

// Optional host-window lifecycle, installed before the first event pump. It
// observes queued/watch close events without consuming or changing them and
// coalesces one close intent for this exact window lifetime. No source callback
// or physical/reset input is deferred. Initialization that never completes
// still needs a separate host-abort policy.
struct NativeHardwareWindowCloseStatus {
    bool retaining{}, armed{}, requested{}, submitted{};
    std::uint32_t window_id{}, event_type{};
    std::uint64_t event_timestamp{}, stm_generation{};
};
void RetainNativeHardwareWindowClose(SDL_Window* window);
// Call on the inactive owning thread after the matching STM power-removal
// policy is genuinely configured; this function never services/delivers it.
void ArmNativeHardwareWindowClose();
NativeHardwareWindowCloseStatus GetNativeHardwareWindowCloseStatus();

// Compose input with an existing SDK hardware owner. The exact STM input is
// borrowed from that owner's initialized device and is never retired here.
// These functions do not register/replace any SDK hardware endpoint.
// Who plays (controls.player1-4): the WPAD channel (player - 1) of keyboard &
// mouse and of the Wii Remote in each DolphinBar slot, -1 = not a player.
// Without a DolphinBar at start keyboard & mouse is player 1 and plays alone.
struct NativePlayers {
    bool fixed = false; // false: remotes, then keyboard & mouse, as they connect
    int keyboard = 0;
    std::array<int, 4> remotes{-1, -1, -1, -1};
    // WPAD channel of the n-th connected standard gamepad, or -1.
    std::array<int, 4> gamepads{-1, -1, -1, -1};
    // controls.mouse_pointer: the mouse also points for player 1 when that
    // player is a Wii Remote or gamepad and plays alone.
    bool mouse_with_controller = false;
    // Pointer calibration of the Wii Remote in each DolphinBar slot.
    std::array<std::optional<WiimoteCalibration>, 4> calibrations{};
};
void InitializeNativeHardwareInput(SDL_Window* window, WpadSDLSettings settings,
    StmInput borrowed_stm, DesktopWpadSettings desktop = {}, NativePlayers players = {});
void ServiceNativeHardwareInput();
void ShutdownNativeHardwareInput();

// After native SDL/SDK host setup, before loading the original source module.
// The original source still initializes KPAD/pads and registers STM callbacks.
void InitializeNativeHardwareOwner(SDL_Window* window, WpadSDLSettings settings);

// Retire the SDK hook and borrowed hardware callbacks before source storage,
// SDK arenas or the window are released. This does not destroy game managers.
void ShutdownNativeHardwareOwner();

} // namespace mscharged::platform
