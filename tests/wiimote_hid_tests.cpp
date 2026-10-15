// Native Wii Remote IR decoding against original WPADHIDParser's basic DPD
// conversion: x as sent, y flipped (767 - y), absent objects (0, 767, size 0).
#include "platform/wiimote_hid.h"

#include <SDL3/SDL.h>

#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <thread>

namespace {
unsigned checks{}, failures{};
void Check(bool value, const char* message) {
    ++checks;
    if (!value) { ++failures; std::fprintf(stderr, "Wii Remote HID: %s\n", message); }
}
} // namespace

int main() {
    using mscharged::platform::DecodeWiimoteBasicIr;
    // Bytes from a real remote behind a DolphinBar: two objects in the first
    // group (with high coordinate bits), none in the second.
    const std::uint8_t pair[10]{0x9d, 0x1d, 0x77, 0xc9, 0x47, 0xff, 0xff, 0xff, 0xff, 0xff};
    const auto dots = DecodeWiimoteBasicIr(pair);
    Check(dots[0].x == 925 && dots[0].y == 767 - 285 && dots[0].size == 12 && dots[0].trace_id == 0,
          "first object lost its position, flip or size");
    Check(dots[1].x == 969 && dots[1].y == 767 - 327 && dots[1].size == 12 && dots[1].trace_id == 1,
          "second object lost its position, flip or size");
    for (int n = 2; n < 4; ++n)
        Check(dots[n].x == 0 && dots[n].y == 767 && dots[n].size == 0 && dots[n].trace_id == n,
              "an empty slot is not absent in the original form");

    // Original WPAD also drops an object on the top camera row (sent y 0)
    // and keeps its neighbour.
    const std::uint8_t edge[10]{0x10, 0x00, 0x00, 0x20, 0x05, 0xff, 0xff, 0xff, 0xff, 0xff};
    const auto top = DecodeWiimoteBasicIr(edge);
    Check(top[0].x == 0 && top[0].y == 767 && top[0].size == 0, "an object on the top row was kept");
    Check(top[1].x == 0x20 && top[1].y == 762 && top[1].size == 12, "the neighbour of a dropped object was lost");

    // Second group's high bits use the same layout.
    const std::uint8_t second[10]{0xff, 0xff, 0xff, 0xff, 0xff, 0x01, 0x02, 0xe4, 0x03, 0x04};
    const auto group = DecodeWiimoteBasicIr(second);
    Check(group[0].size == 0 && group[1].size == 0, "an empty first group produced objects");
    Check(group[2].x == (0x01 | 2 << 8) && group[2].y == 767 - (0x02 | 3 << 8), "third object bits");
    Check(group[3].x == (0x03 | 0 << 8) && group[3].y == 767 - (0x04 | 1 << 8), "fourth object bits");

    // KPAD's pointer centre for the screen centre (the desktop mouse camera).
    using namespace mscharged::platform;
    const auto near = [](double a, double b, double tolerance = 1e-6) { return std::fabs(a - b) <= tolerance; };
    const auto below = KpadPointerCentre(0.5, 0.5, 0), above = KpadPointerCentre(0.5, 0.5, 1);
    Check(near(below[0], 511.5) && near(below[1], (767.0 + 1024.0 * 0.2) / 2), "pointer centre, bar below");
    Check(near(above[1], (767.0 - 1024.0 * 0.2) / 2), "pointer centre, bar above");

    // Remote ids in settings: DolphinBar slots and Bluetooth addresses.
    Check(WiimoteDolphinBarSlot("remote3") == 2 && WiimoteDolphinBarId(2) == "remote3" &&
          WiimoteDolphinBarSlot("remote5") < 0 && WiimoteDolphinBarSlot("remote-9ce635c69076") < 0,
          "DolphinBar slot ids");
    const auto linux_id = BluetoothWiimoteId("9c:e6:35:C6:90:76", "/dev/hidraw5");
    Check(linux_id == "remote-9ce635c69076" && BluetoothWiimoteId("9c-e6-35-c6-90-76", "x") == linux_id &&
          BluetoothWiimoteId("9CE635C69076", "y") == linux_id, "a Bluetooth address gives the same id on each system");
    Check(IsBluetoothWiimoteId(linux_id) && IsWiimoteId(linux_id) && IsWiimoteId("remote1") &&
          !IsWiimoteId("remote-9ce635c6907") && !IsWiimoteId("remote-9CE635C69076") && !IsWiimoteId("keyboard"),
          "remote id forms");
    Check(WiimoteBluetoothAddress(linux_id) == "9C:E6:35:C6:90:76" && WiimoteBluetoothAddress("remote1").empty(),
          "shown Bluetooth address");
    // Without an address the path stands in, stable for the same path.
    const auto unnamed = BluetoothWiimoteId("", "/dev/hidraw7");
    Check(IsBluetoothWiimoteId(unnamed) && unnamed == BluetoothWiimoteId("serial?", "/dev/hidraw7") &&
          unnamed != BluetoothWiimoteId("", "/dev/hidraw8"), "a remote without an address");

    // The input cadence (every 10 ms) scans the HID devices only after SDL
    // reports a device change. A scan every 2 s held a tester's game for about
    // 70 ms each time on Windows. Real remotes are ignored (CTest environment).
    InitializeWiimoteHid({});
    if (!WiimoteHidScans()) {
        std::printf("Wii Remote HID scans: skipped, SDL HID is unavailable\n");
    } else {
        Check(WiimoteHidScans() == 1, "starting the driver did not scan once");
        unsigned changes = 0;
        auto counted = SDL_hid_device_change_count();
        const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(2500);
        while (std::chrono::steady_clock::now() < end) {
            ServiceWiimoteHid();
            const auto count = SDL_hid_device_change_count();
            if (count != counted) { counted = count; ++changes; }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        ServiceWiimoteHid(); // takes up a change the last check saw
        Check(WiimoteHidScans() == 1 + changes, "the driver scanned without a device change");
        ShutdownWiimoteHid();
    }

    std::printf("Wii Remote IR decoding, ids and HID scans: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
