#pragma once

#include "platform/desktop_dpd.h"

#include <array>
#include <cstdint>

namespace mscharged::platform {
// Wii Remote camera helpers shared by the native driver and the desktop
// mouse camera. Original KPAD still selects objects, decides
// validity and computes the pointer from what these produce.

// Console IR camera sensitivity blocks (registers 0xb00000 and 0xb0001a) for
// the Wii's sensitivity levels 1-5.
extern const std::uint8_t kWiimoteIrBlock1[5][9];
extern const std::uint8_t kWiimoteIrBlock2[5][2];

// Camera objects of a basic-mode IR block (10 bytes of report 0x37), in the
// coordinates original WPAD gives KPAD.
NativeDpdObservation DecodeWiimoteBasicIr(const std::uint8_t* ir);

// The camera midpoint of a unit-distance sensor-bar pair that original KPAD
// reads as the pointer at (x, y), 0..1 across the game picture, for the Wii
// sensor-bar position (0 below, 1 above the screen).
std::array<double, 2> KpadPointerCentre(double x, double y, std::uint8_t sensor_bar_position);

} // namespace mscharged::platform
