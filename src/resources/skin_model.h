#pragma once
#include "resources/binary_reader.h"
#include <array>
#include <optional>
#include <vector>
namespace mscharged::resources
{
struct SkinTextureBinding { std::uint32_t hash=0; std::uint8_t flags=0; };
struct SkinCustomMaterial
{
    std::array<SkinTextureBinding,2> textures;
    float blend=0,alpha=1;
    std::uint32_t shadow_level=0,lighting_enabled=0;
};
struct SkinVertex
{
    std::array<float,3> position,normal;
    std::array<std::array<std::int16_t,2>,2> uv;
    std::array<std::uint8_t,4> bones;
    std::array<float,4> weights;
};
struct SkinPacket
{
    std::uint8_t primitive=0;
    std::uint32_t program=0,raster=0;
    std::array<float,16> matrix;
    SkinCustomMaterial material;
    std::vector<SkinVertex> vertices;
    std::vector<std::uint16_t> indices;
    std::vector<std::uint32_t> bone_hashes;
};
struct SkinBind { std::uint32_t hash=0; std::array<float,16> matrix; };
struct RigidSkinModel
{
    std::uint32_t hash=0;
    std::vector<SkinBind> binds;
    std::vector<SkinPacket> packets;
    std::vector<std::uint8_t> metadata; // Original ignores optional chunk0x1B009.
};
// Original RLG skin/custom-material profile. Exactly one unit-weight influence
// per vertex, no morph channels, at most nine bones per packet. Wii records are
// decoded separately from host storage; exported pointers are never followed.
RigidSkinModel ReadRigidSkinModel(Bytes file,std::optional<std::uint32_t> selected={});
}
