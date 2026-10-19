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
    // Only Wii Remotes of the native HID driver have a speaker
    // (WiimoteHidSettings::speaker).
    std::uint8_t speaker_volume = 89;
    // Wii Remotes play the game's speaker sounds (controls.speaker_volume
    // above 0), as 8-bit PCM or as the game's own ADPCM
    // (controls.speaker_format, WiimoteHidSettings::speaker_pcm).
    bool remote_speaker = false;
    bool remote_speaker_pcm = true;
};
void ConfigureWpadSDL(WpadSDLSettings settings);
// The Wii Remote HID driver sent a speaker command WiimoteHidControlSpeaker
// queued for this joystick (in order). The caller's callback follows on the
// next service, as original WPAD completes its command queue.
void CompleteNativeWpadSpeaker(std::uint32_t joystick_id, bool ok);

// Call from the SDL owner while servicing host events. Bare Wii Remote and
// explicitly registered desktop virtual-device reports use this raw provider.
// Original connect/sampling callbacks run on this owner through the shared IRQ
// boundary. No callbacks are executed by an SDL watcher or device worker.
void ServiceWpadSDL();
// WPAD channel (player - 1) of an SDL joystick, or -1 while it has none.
int GetNativeWpadChannel(std::uint32_t joystick_id);
// Whether original WPAD has the camera (DPD) of that joystick's channel enabled.
bool GetNativeWpadCameraEnabled(std::uint32_t joystick_id);
// Player assignment for native providers (controls.player1-4). A joystick
// with a fixed channel connects only to that WPAD channel (player - 1) and
// waits while it is busy; -1 removes the assignment. Joysticks without one
// take the lowest free channel outside the reserved mask.
void SetNativeWpadFixedChannel(std::uint32_t joystick_id, int channel);
void SetNativeWpadReservedChannels(std::uint32_t mask);
std::size_t WpadSDLConnectedChannels();

// Lend the desktop mouse camera to connected remotes without their own camera
// source: SDL reports physical Wii Remotes without IR data. nullptr withdraws
// it. Call from the SDL owner; original DPD enable/disable still applies.
void SetNativeWpadSharedPointer(const NativeDpdObservation* observation);
// The desktop mouse alongside a controller (controls.mouse_pointer): on WPAD
// channel `channel` its camera observation replaces the controller's while
// `observation` is non-null, and the mouse buttons add A and B. -1 ends it.
// Call from the SDL owner; original DPD enable/disable still applies.
void SetNativeWpadMousePlayer(int channel, const NativeDpdObservation* observation, bool a, bool b);

} // namespace mscharged::platform
