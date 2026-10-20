#pragma once

#include <memory>
#include <string>

namespace mscharged::platform {
// Connects and pairs Wii Remotes over Bluetooth (Windows). Windows does not
// connect a Wii Remote by itself, and its pairing wizard asks for a PIN that
// cannot be typed (the Wii's PIN is a Bluetooth address as six raw bytes);
// Linux (BlueZ) and macOS keep this to the system.
//
//  - Scan (continuous while active, once on Refresh): forgets the Wii Remotes
//    Windows remembers but can no longer reconnect (not connected, not
//    paired), runs a Bluetooth inquiry and turns on the HID service of every
//    Wii Remote ("Nintendo RVL-CNT-01", "-TR") that answers after 1+2 or
//    SYNC. The connection lasts until the remote turns off. Paired remotes
//    are left alone: they connect themselves.
//  - Sync: three inquiries after SYNC (under the battery cover) was pressed,
//    pairing each new remote with the PC's Bluetooth address as its PIN, as a
//    Wii does. A paired remote reconnects itself when any button is pressed,
//    in the launcher and in the game, without searching.
//  - Reset: forgets every Wii Remote Windows remembers (paired or not).
// An inquiry can briefly disturb other Bluetooth devices (audio).
struct WiimoteBluetoothStatus {
    bool supported = false; // this system connects remotes itself (Windows)
    bool adapter = true;    // a Bluetooth adapter was found
    bool searching = false; // a scan, Refresh, Sync or Reset is running or waiting
    bool syncing = false;   // a Sync is running or waiting
    bool resetting = false; // a Reset is running or waiting
    int connected = 0;      // remotes connected by scans so far
    int paired = 0;         // remotes paired by Sync so far
    std::string last;       // the latest event, for the launcher ("" if none)
};

class WiimoteBluetoothSearch {
public:
    WiimoteBluetoothSearch();
    ~WiimoteBluetoothSearch(); // stops without waiting for an inquiry to end
    WiimoteBluetoothSearch(const WiimoteBluetoothSearch&) = delete;
    WiimoteBluetoothSearch& operator=(const WiimoteBluetoothSearch&) = delete;

    // Scan continuously while `on`; off ends it after the current round.
    void SetActive(bool on);
    // One scan (about 4 s), a Sync (about 12 s) or a Reset, after the round
    // in progress. Without Windows support they do nothing.
    void Refresh();
    void Sync();
    void Reset();
    WiimoteBluetoothStatus Status() const;

    struct State;

private:
    std::shared_ptr<State> state_;
};
} // namespace mscharged::platform
