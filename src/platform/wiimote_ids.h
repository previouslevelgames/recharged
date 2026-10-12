#pragma once

#include <cctype>
#include <cstdint>
#include <string>

// Names of physical Wii Remotes in settings (controls.player1-4 and
// controls.<remote>_calibration), shared by the launcher, the configuration
// and the native driver. C++17: the configuration library includes it.
//   remote1-remote4      the Wii Remote in that slot of a Mayflash DolphinBar
//   remote-9ce635c69076  a Wii Remote paired directly over Bluetooth, by its
//                        Bluetooth address (12 lowercase hex digits)
namespace mscharged::platform {
inline int WiimoteDolphinBarSlot(const std::string& id) {
    return id.size() == 7 && id.compare(0, 6, "remote") == 0 && id[6] >= '1' && id[6] <= '4' ? id[6] - '1' : -1;
}

inline std::string WiimoteDolphinBarId(int slot) { return "remote" + std::to_string(slot + 1); }

inline bool IsBluetoothWiimoteId(const std::string& id) {
    if (id.size() != 19 || id.compare(0, 7, "remote-") != 0) return false;
    for (std::size_t n = 7; n < id.size(); ++n)
        if (!std::isdigit(static_cast<unsigned char>(id[n])) && (id[n] < 'a' || id[n] > 'f')) return false;
    return true;
}

inline bool IsWiimoteId(const std::string& id) { return WiimoteDolphinBarSlot(id) >= 0 || IsBluetoothWiimoteId(id); }

// The id of a directly connected remote from its HID serial number, which
// carries the Bluetooth address on Linux ("9c:e6:35:c6:90:76"), macOS
// ("9c-e6-35-c6-90-76") and Windows ("9ce635c69076"). Without an address the
// HID path stands in, which stays the same only while the remote is connected.
inline std::string BluetoothWiimoteId(const std::string& serial, const std::string& path) {
    std::string hex;
    for (const char c : serial) {
        if (std::isxdigit(static_cast<unsigned char>(c)))
            hex += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        else if (c != ':' && c != '-' && c != ' ') { hex.clear(); break; }
    }
    if (hex.size() != 12) {
        std::uint64_t hash = 0xcbf29ce484222325ull;
        for (const char c : path) hash = (hash ^ static_cast<unsigned char>(c)) * 0x100000001b3ull;
        static const char digits[] = "0123456789abcdef";
        hex.clear();
        for (int n = 11; n >= 0; --n) hex += digits[(hash >> (4 * n)) & 15];
    }
    return "remote-" + hex;
}

// "9C:E6:35:C6:90:76" for a Bluetooth id, empty otherwise.
inline std::string WiimoteBluetoothAddress(const std::string& id) {
    if (!IsBluetoothWiimoteId(id)) return {};
    std::string address;
    for (std::size_t n = 7; n < id.size(); n += 2) {
        if (!address.empty()) address += ':';
        address += static_cast<char>(std::toupper(static_cast<unsigned char>(id[n])));
        address += static_cast<char>(std::toupper(static_cast<unsigned char>(id[n + 1])));
    }
    return address;
}
} // namespace mscharged::platform
