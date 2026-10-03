#pragma once
#include "resources/binary_reader.h"
#include <array>
#include <string>
#include <vector>

namespace mscharged::resources
{
struct CameraKey
{
    std::array<float, 3> position{}, target{};
    std::array<float, 4> rotation{};
    float fov = 0, focal_length = 0;
    bool operator==(const CameraKey&) const = default;
};
struct CameraAnimation
{
    std::string name;
    std::vector<CameraKey> keys;
    bool operator==(const CameraAnimation&) const = default;
};
// Decode Wii records into owned host values; never retain pointers into the file.
// The playback equations and camera selection belong to the original game.
CameraAnimation ReadCameraAnimation(Bytes file);
}
