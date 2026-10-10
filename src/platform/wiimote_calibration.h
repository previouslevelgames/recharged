#pragma once

#include "platform/desktop_dpd.h"

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>

namespace mscharged::platform {
// Wii Remote camera helpers shared by the native driver, the desktop mouse
// camera and the launcher. Original KPAD still selects objects, decides
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

// Pointer calibration of one remote (controls.remoteN_calibration): the spot
// it aims at on the game picture (0..1) as an affine function of the camera
// midpoint of the sensor-bar pair.
struct WiimoteCalibration {
    std::array<double, 6> affine{}; // x = a0*mx + a1*my + a2, y = a3*mx + a4*my + a5
};
struct WiimoteCalibrationSample {
    double camera_x{}, camera_y{}; // camera midpoint of the sensor-bar pair
    double screen_x{}, screen_y{}; // the aimed target, 0..1 across the game picture
};
// Least squares over the samples; fails unless they span the picture.
std::optional<WiimoteCalibration> FitWiimoteCalibration(std::span<const WiimoteCalibrationSample> samples);
std::string FormatWiimoteCalibration(const WiimoteCalibration& calibration);
std::optional<WiimoteCalibration> ParseWiimoteCalibration(const std::string& text);

// Follows the sensor-bar pair across frames that show only one of its ends.
struct WiimotePairTracker {
    bool valid = false;
    double mx = 0, my = 0, hx = 0, hy = 0; // midpoint, and half the pair vector
};
std::optional<std::array<double, 2>> TrackWiimotePair(WiimotePairTracker& tracker,
                                                       const NativeDpdObservation& dots);
// Moves the camera objects so that original KPAD puts the pointer where the
// calibration says the remote aims. Objects that leave the camera, and lone
// objects without a known pair, become absent.
NativeDpdObservation ApplyWiimoteCalibration(const WiimoteCalibration& calibration, WiimotePairTracker& tracker,
                                             const NativeDpdObservation& dots, std::uint8_t sensor_bar_position);
} // namespace mscharged::platform
