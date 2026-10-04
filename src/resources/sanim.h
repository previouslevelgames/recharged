#pragma once
#include "resources/binary_reader.h"
#include <array>
#include <string>
#include <vector>

namespace mscharged::resources
{
inline constexpr unsigned MaximumSAnimKeys = 65536, MaximumSAnimNodes = 4096;
inline constexpr unsigned MaximumSAnimMorphs = 20, MaximumSAnimTracks = 256;
struct SAnimNode
{
    std::uint32_t properties = 0, auxiliary_metadata = 0;
    bool auxiliary_present = false;
    // Quaternion packed bytes retain their Wii order for the native decoders.
    // Angle-only rotations and scalar channels are decoded into host values.
    std::vector<std::uint8_t> rotation, weights, auxiliary;
    std::vector<std::uint16_t> angles;
    std::vector<std::array<std::uint16_t, 3>> scale;
    std::vector<std::array<float, 3>> translation;
};
struct SAnimation
{
    std::string name;
    std::uint32_t hash = 0, frames = 0, hierarchy_signature = 0;
    std::vector<SAnimNode> nodes;
    std::vector<std::uint16_t> root_rotation;
    std::vector<std::array<float, 3>> root_translation;
    std::vector<std::uint32_t> morph_ids, morph_counts;
    std::vector<std::uint8_t> morph_keys;
};
SAnimation ReadSAnimation(Bytes file, std::size_t offset, std::size_t end);
// A .sanim inventory contains consecutive animation roots; preserve their order.
std::vector<SAnimation> ReadSAnimations(Bytes file);
}
