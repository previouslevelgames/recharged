#pragma once
#include "resources/binary_reader.h"
#include <array>
#include <vector>

namespace mscharged::resources
{
struct Vertex
{
    std::array<float, 3> position;
    std::array<float, 2> uv;
};
struct Packet
{
    std::uint8_t primitive = 0;
    std::uint8_t texture_flags = 0;
    std::uint32_t program = 0;
    std::uint32_t texture = 0;
    std::vector<Vertex> vertices;
    std::vector<std::uint16_t> indices;
};
struct StaticModel
{
    std::uint32_t id = 0;
    std::vector<Packet> packets;
};
// Bounded static RLG profile. Skinning, animation and unknown materials fail explicitly.
std::vector<StaticModel> ReadStaticModels(Bytes data);
}
