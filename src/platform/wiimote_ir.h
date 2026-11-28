#pragma once

#include "platform/desktop_dpd.h"

#include <array>
#include <cstdint>
#include <optional>

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
// sensor-bar position (0 below, 1 above the screen). `sensor_height`: the
// game's KPAD sets the pointer centre for that position (KPADCalibrateDPD in
// the USA and Japanese builds, the aiming mode of the Korean build's newer
// KPAD); the European build's older KPAD leaves its centre at the bar.
std::array<double, 2> KpadPointerCentre(double x, double y, std::uint8_t sensor_bar_position,
                                        bool sensor_height = true);

// Where a Wii Remote points while the game's own pointer is held still
// (Recharged's menus), as the game's KPAD would compute it from the same
// camera objects: the sensor bar's two dots (90-512 camera pixels apart, as
// KPAD accepts them: the pair nearest the last one, else the most level one;
// one dot alone moves the last pair with it), their midpoint turned back by
// the Remote's roll (the dots' angle), KpadPointerCentre's mapping undone, and
// smoothed like the game's KPADSetPosParam(0.02, 0.95). KPAD's other checks
// (the accelerometer's horizon, the distance's speed) are left out.
class CameraPointer {
public:
    // Once a frame: the pointer, 0..1 across the game picture (beyond it when
    // pointing past its edges), or nothing while the camera doesn't see the
    // bar (the smoothing starts again when it does).
    std::optional<std::array<double, 2>> Update(const NativeDpdObservation& objects,
                                                std::uint8_t sensor_bar_position, bool sensor_height = true);
    void Reset();

private:
    // The last pair, its left dot first (camera objects' coordinates).
    bool pair_ = false;
    std::array<double, 2> left_{}, right_{};
    // KPAD's smoothed position (-1..1 across the picture).
    bool smoothed_ = false;
    std::array<double, 2> pos_{};
};

} // namespace mscharged::platform
