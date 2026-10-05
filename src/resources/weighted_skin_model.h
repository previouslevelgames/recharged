#pragma once
#include "resources/skin_model.h"
#include "resources/specular_material.h"
namespace mscharged::resources
{
struct WeightedSkinVertex
{
    std::array<float,3> position,normal;
    std::array<std::array<std::int16_t,2>,3> uv;
    std::array<std::uint8_t,4> bones;
    std::array<float,4> weights;
};
struct WeightedSkinPacket
{
    std::uint8_t primitive=0;
    std::uint32_t program=0,raster=0;
    std::array<std::uint8_t,7> stream_slots;
    std::array<float,16> matrix;
    SpecularSkinMaterial material;
    std::vector<WeightedSkinVertex> vertices;
    std::vector<std::uint16_t> indices;
    std::vector<std::uint32_t> bone_hashes;
};
struct WeightedSkinModel
{
    std::uint32_t hash=0;
    std::vector<SkinBind> binds;
    std::vector<WeightedSkinPacket> packets;
    std::vector<std::uint8_t> metadata;
};
// Exact authored GXSpecular seven-stream skin records; positive influences keep
// their original float bits/order. No normalization, rigid conversion, pose or
// GL registration occurs here. Nonzero morph channels/other materials reject.
WeightedSkinModel ReadWeightedSkinModel(Bytes,std::optional<std::uint32_t> selected={});
}
