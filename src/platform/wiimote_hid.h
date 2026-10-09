#pragma once

#include "platform/desktop_dpd.h"

#include <cstdint>

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
};

struct WiimoteHidRemote {
    std::string path;
    int slot = -1;          // DolphinBar slot or enumeration order
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
};

// Owner thread. Probes immediately and waits briefly for remotes that are
// already on, so they can take their players before the keyboard device.
void InitializeWiimoteHid(WiimoteHidSettings settings);
// Owner-thread input cadence: reads reports, probes empty slots, retires
// silent remotes.
void ServiceWiimoteHid();
void ShutdownWiimoteHid();
WiimoteHidStatus GetWiimoteHidStatus();
// Camera objects of a basic-mode IR block (10 bytes of report 0x37), in the
// coordinates original WPAD gives KPAD.
NativeDpdObservation DecodeWiimoteBasicIr(const std::uint8_t* ir);

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
// A DolphinBar in modes 1-3 lists one Mayflash device instead of four remotes.
bool DolphinBarInOtherMode();
} // namespace mscharged::platform
