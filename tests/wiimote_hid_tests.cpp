// Native Wii Remote IR decoding against original WPADHIDParser's basic DPD
// conversion: x as sent, y flipped (767 - y), absent objects (0, 767, size 0).
#include "platform/wiimote_hid.h"

#include <cstdint>
#include <cstdio>

namespace {
unsigned checks{}, failures{};
void Check(bool value, const char* message) {
    ++checks;
    if (!value) { ++failures; std::fprintf(stderr, "Wii Remote IR decoding: %s\n", message); }
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

    std::printf("Wii Remote IR decoding: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
