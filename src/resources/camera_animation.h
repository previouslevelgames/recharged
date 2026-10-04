#pragma once
#include "resources/binary_reader.h"
#include <array>
#include <string>
#include <vector>

namespace mscharged::resources
{
struct CameraKey
{
    std::array<float, 3> position{};
    // Finite authored bits, without a model-space magnitude limit. Playback
    // checks target arithmetic and validates any explicit look-at consumption.
    std::array<float, 3> target{};
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
// Decode one complete CAM chunk inside a larger file. All chunk alignment is
// relative to file, and end includes the CAM chunk's final four-byte padding.
CameraAnimation ReadCameraAnimation(Bytes file, std::size_t offset, std::size_t end);
}
