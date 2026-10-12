#pragma once

#include <memory>
#include <string>

namespace mscharged::platform {
// Connects Wii Remotes over Bluetooth while it searches (Windows): press 1+2
// or SYNC on a remote and it becomes a HID device, which the Wii Remote
// probe and driver then find like any other. Windows does not connect a
// Wii Remote by itself without its pairing wizard, and does not reconnect
// one paired without a PIN; Linux (BlueZ) and macOS keep this to the system.
//
// While searching, each round:
//  - removes Wii Remotes Windows remembers but can no longer reconnect
//    (not connected, not authenticated), so 1+2 connects them again;
//  - runs a short Bluetooth inquiry and turns on the HID service of every
//    Wii Remote ("Nintendo RVL-CNT-01", "-TR") found that is not connected.
// An inquiry can briefly disturb other Bluetooth devices (audio), so the
// launcher searches only while its Controls page is open.
struct WiimoteBluetoothStatus {
    bool supported = false; // this system connects remotes itself (Windows)
    bool searching = false;
    bool adapter = true;    // a Bluetooth adapter was found
    int connected = 0;      // remotes connected by this search so far
    std::string last;       // the latest event, for the launcher ("" if none)
};

class WiimoteBluetoothSearch {
public:
    WiimoteBluetoothSearch();
    ~WiimoteBluetoothSearch(); // stops without waiting for an inquiry to end
    WiimoteBluetoothSearch(const WiimoteBluetoothSearch&) = delete;
    WiimoteBluetoothSearch& operator=(const WiimoteBluetoothSearch&) = delete;

    // Search while `on`; turning it off ends the search after the current round.
    void SetActive(bool on);
    WiimoteBluetoothStatus Status() const;

    struct State;

private:
    std::shared_ptr<State> state_;
};
} // namespace mscharged::platform
