#pragma once

#include "platform/wiimote_calibration.h"

#include <array>
#include <cstdint>
#include <initializer_list>
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
    // Players by DolphinBar slot 1-4: WPAD channel (player - 1), -1 = not a
    // player. Without fixed players every remote plays, in slot order.
    bool fixed_players = false;
    std::array<int, 4> slot_channels{-1, -1, -1, -1};
    // Pointer calibration by DolphinBar slot (controls.remoteN_calibration),
    // and the Wii sensor-bar position it is applied for (0 below, 1 above).
    std::array<std::optional<WiimoteCalibration>, 4> calibrations{};
    std::uint8_t sensor_bar_position = 0;
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
// The driver runs and has opened at least one Wii Remote HID device.
bool WiimoteHidHasDevices();


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

// A live Wii Remote for the launcher's pointer calibration (no game input
// owner): turns the remote's camera on like the game's driver and reads its
// camera objects and buttons. Lives in wiimote_scan.cpp.
class WiimoteHidLive {
public:
    WiimoteHidLive() = default;
    WiimoteHidLive(const WiimoteHidLive&) = delete;
    WiimoteHidLive& operator=(const WiimoteHidLive&) = delete;
    ~WiimoteHidLive();
    // The remote in DolphinBar slot 0-3; false when it does not answer.
    bool Open(int slot);
    void Close();
    // Reads waiting reports; never blocks.
    void Poll();
    bool Connected() const;
    // Core buttons as in report bytes 1-2: 0x0008 A, 0x0004 B, 0x0080 HOME.
    std::uint16_t Buttons() const { return buttons_; }
    const NativeDpdObservation& Dots() const { return dots_; }

private:
    bool Send(std::initializer_list<std::uint8_t> report);
    bool Write(std::uint32_t address, const std::uint8_t* data, int size);
    void Handle(const std::uint8_t* report, int size);
    SDL_hid_device* hid_ = nullptr;
    bool initialized_ = false;
    std::uint16_t buttons_ = 0;
    NativeDpdObservation dots_{};
    std::uint64_t last_report_ns_ = 0;
};
bool IsWiimoteHid(const SDL_hid_device_info* info);
bool IsDolphinBarHid(const SDL_hid_device_info* info);
// A DolphinBar in modes 1-3 lists one Mayflash device instead of four remotes.
bool DolphinBarInOtherMode();
} // namespace mscharged::platform
