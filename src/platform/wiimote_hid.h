#pragma once

#include "platform/wiimote_ir.h"
#include "platform/wiimote_ids.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>

struct SDL_hid_device;
struct SDL_hid_device_info;
#include <string>
#include <vector>

namespace mscharged::platform {
// Native Wii Remote driver over raw HID (SDL_hid_*), for remotes behind a
// Mayflash DolphinBar in mode 4 and remotes paired directly over Bluetooth.
// Each remote that answers a status request becomes an SDL virtual core-Wii
// device for the existing WPAD transport: buttons, accelerometer and rumble on
// the device, the IR camera's raw objects and the Nunchuk through the same
// observation producers the desktop profile uses. The original WPAD/KPAD code
// still decides everything about pointer, motion and extension handling.
struct WiimoteHidSettings {
    // Original IR sensitivity preference (1-5), mapped to the console's
    // camera register blocks.
    std::uint8_t ir_sensitivity = 3;
    // The remotes offer original WPAD their speaker (controls.speaker_volume
    // above 0); without it WPAD finds none, and no sound goes to them.
    bool speaker = false;
    // What original WPAD's speaker ON writes to register 0xa20001 before the
    // configuration: 0x80 in the American game, 0x08 in the European one.
    std::uint8_t speaker_reg01 = 0x08;
    // Players by remote id (wiimote_ids.h): WPAD channel (player - 1); a
    // remote without an entry is not a player. Without fixed players every
    // remote plays, DolphinBar slots first.
    bool fixed_players = false;
    std::map<std::string, int> channels;
};

struct WiimoteHidRemote {
    std::string path;
    std::string id;         // settings name (wiimote_ids.h)
    int slot = -1;          // DolphinBar slot, -1 for a directly connected remote
    bool plus = false;      // Wii Remote Plus (RVL-CNT-01-TR)
    int channel = -1;       // WPAD channel (player - 1), -1 when unassigned
    bool nunchuk = false;
    bool ir_visible = false;
    int battery = 0;        // percent (status byte, 0xc8 = full)
};
struct WiimoteHidStatus {
    bool dolphinbar = false; // a Mayflash adapter in mode 4 is present
    bool dolphinbar_other_mode = false; // a DolphinBar in a mouse/gamepad mode (1-3)
    int adapter_slots = 0;
    std::vector<WiimoteHidRemote> remotes;
    // Wii Remotes the system lists but this user may not open (Linux: no
    // udev rule for the hidraw device).
    int no_permission = 0;
    // Launcher probe: ids of remotes that are listed and open but did not
    // answer this time (a flaky link, or just switched off).
    std::vector<std::string> silent;
};
// DolphinBar slots first, then directly connected remotes by address.
void SortWiimoteRemotes(std::vector<WiimoteHidRemote>& remotes);

// Owner thread. Probes immediately and waits briefly for remotes that are
// already on, so they can take their players before the keyboard device.
void InitializeWiimoteHid(WiimoteHidSettings settings);
// Owner-thread input cadence: reads reports, probes empty slots, retires
// silent remotes.
void ServiceWiimoteHid();
// Owner thread: the players' WPAD channels by remote id after the players
// closed up (InitializeWiimoteHid keeps them otherwise).
void SetWiimoteHidChannels(std::map<std::string, int> channels);
void ShutdownWiimoteHid();
WiimoteHidStatus GetWiimoteHidStatus();
// The driver runs and has opened at least one Wii Remote HID device.
bool WiimoteHidHasDevices();
// HID device scans since the driver started: one at start, then one per HID
// device arrival or removal that SDL reports.
unsigned WiimoteHidScans();

// The Wii Remote speaker for original WPAD (wpad_sdl.cpp), by the virtual
// joystick of a remote. Owner thread.
enum class WiimoteSpeakerCommand { Off, On, Mute, Unmute, Play };
// The output reports of original WPADControlSpeaker for a command, in order;
// `volume` (0-127) is the configuration's volume byte for On, `reg01` the
// game's byte for 0xa20001 (WiimoteHidSettings::speaker_reg01).
std::vector<std::vector<std::uint8_t>> WiimoteSpeakerReports(WiimoteSpeakerCommand command, std::uint8_t volume,
                                                             std::uint8_t reg01 = 0x08);
// The reports the driver sends a Remote it newly hears from: WPAD's OFF after
// enabling the speaker, so its registers take the writes. A Wii starts each
// game with freshly connected Remotes; on a PC a Remote stays connected and
// keeps the last game's speaker running.
std::vector<std::vector<std::uint8_t>> WiimoteSpeakerResetReports();
// A ready remote of this driver with the speaker setting on.
bool WiimoteHidHasSpeaker(std::uint32_t joystick_id);
// Queues the command's reports; the driver calls CompleteNativeWpadSpeaker
// (wpad_sdl.h) once for it when its last report is handed to the remote's
// writer thread (it goes out before frames handed over later), or its write
// was acknowledged. False: not a speaker of this driver.
bool WiimoteHidControlSpeaker(std::uint32_t joystick_id, WiimoteSpeakerCommand command, std::uint8_t volume);
// Speaker data (report 0x18) goes out on the remote's writer thread, after
// its other reports: the remote takes a frame when its speaker is on, its
// commands went out and fewer than three frames wait.
bool WiimoteHidCanStreamSpeaker(std::uint32_t joystick_id);
bool WiimoteHidStreamSpeaker(std::uint32_t joystick_id, const void* data, std::uint16_t size);


// Standalone detection for the launcher (no game input owner): opens each Wii
// Remote HID path, asks for a status report and closes it again. Start() and
// Poll() never block, so a UI can probe from its frame loop. Lives in
// wiimote_scan.cpp, which needs only SDL's HID API.
class WiimoteHidProbe {
public:
    WiimoteHidProbe() = default;
    WiimoteHidProbe(const WiimoteHidProbe&) = delete;
    WiimoteHidProbe& operator=(const WiimoteHidProbe&) = delete;
    ~WiimoteHidProbe();
    void Start(int timeout_ms = 300);
    // True once every remote answered or the timeout passed; Result() is then final.
    bool Poll();
    bool Active() const { return active_; }
    const WiimoteHidStatus& Result() const { return result_; }
    // Closes any open devices (before SDL shuts down).
    void Close();

private:
    struct Device;
    std::vector<Device*> devices_;
    WiimoteHidStatus result_{};
    std::uint64_t deadline_ns_ = 0;
    bool active_ = false, initialized_ = false;
};
WiimoteHidStatus ScanWiimoteHid(int timeout_ms = 400);

bool IsWiimoteHid(const SDL_hid_device_info* info);
bool IsDolphinBarHid(const SDL_hid_device_info* info);
// Detailed Wii Remote output: with advanced.verbose_console or the
// RECHARGED_WIIMOTE_DEBUG environment variable.
bool WiimoteDebug();
// Send one output report. Windows pads every write to the device's longest
// output report and returns that padded length, so success is any count of
// at least `size`, not exactly `size`.
bool WriteWiimoteReport(SDL_hid_device* hid, const std::uint8_t* report, std::size_t size);
// The settings id of a Wii Remote HID device (wiimote_ids.h). A DolphinBar
// interface takes its interface number as slot, or else its position among
// the interfaces of the same enumeration, which adapter_index counts.
std::string WiimoteHidId(const SDL_hid_device_info* info, int& adapter_index);
// A DolphinBar in modes 1-3 lists one Mayflash device instead of four remotes.
bool DolphinBarInOtherMode();
} // namespace mscharged::platform
