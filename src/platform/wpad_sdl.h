#pragma once

#include "platform/desktop_dpd.h"

#include <cstddef>
#include <cstdint>

namespace mscharged::platform {

// Supply host system preferences before original KPADInit requests WPADInit.
// These are the Wii sensor-bar (0=bottom, 1=top) and sensitivity (1..5) values.
struct WpadSDLSettings {
    std::uint8_t sensor_bar_position;
    std::uint8_t dpd_sensitivity;
    // Selected host profiles may defer physical remotes while retaining their
    // existing default discovery policy for all other callers.
    bool physical_wii_remotes = true;
    // Staged native system preference read by original Options. Keyboard and
    // mouse profiles disable the unavailable physical motor by default.
    bool motor_enabled = true;
    // Staged BT.SPKV preference. The original SC getter uses 89 when the
    // record is absent; WPAD clamps this byte to 127 at initialization.
    // This preference does not provide a physical speaker output endpoint.
    std::uint8_t speaker_volume = 89;
};
void ConfigureWpadSDL(WpadSDLSettings settings);

// Call from the SDL owner while servicing host events. Bare Wii Remote and
// explicitly registered desktop virtual-device reports use this raw provider.
// Original connect/sampling callbacks run on this owner through the shared IRQ
// boundary. No callbacks are executed by an SDL watcher or device worker.
void ServiceWpadSDL();
// WPAD channel (player - 1) of an SDL joystick, or -1 while it has none.
int GetNativeWpadChannel(std::uint32_t joystick_id);
// Whether original WPAD has the camera (DPD) of that joystick's channel enabled.
bool GetNativeWpadCameraEnabled(std::uint32_t joystick_id);
std::size_t WpadSDLConnectedChannels();

// Lend the desktop mouse camera to connected remotes without their own camera
// source: SDL reports physical Wii Remotes without IR data. nullptr withdraws
// it. Call from the SDL owner; original DPD enable/disable still applies.
void SetNativeWpadSharedPointer(const NativeDpdObservation* observation);

} // namespace mscharged::platform
