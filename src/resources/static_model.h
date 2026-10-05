#pragma once
#include "resources/binary_reader.h"
#include <array>
#include <optional>
#include <map>
#include <vector>

namespace mscharged::resources
{
struct Vertex
{
    std::array<float, 3> position;
    std::array<float, 2> uv;
    std::array<float, 3> normal{};
    std::array<std::uint8_t, 4> colour{255, 255, 255, 255};
    std::array<float, 2> uv1{}, uv2{}, uv3{};
};
struct MaterialBinding { std::uint32_t texture = 0; std::uint8_t flags = 0; };
struct Material
{
    std::uint32_t program = 0;
    std::array<MaterialBinding, 4> textures{};
    std::array<float, 4> scalars{};
    std::array<std::uint32_t, 5> switches{};
    std::array<float, 4> specular_colour{};
    std::array<std::array<float, 2>, 3> scroll_speeds{}; // Per-binding X/Y velocities.
};
struct Packet
{
    std::uint8_t primitive = 0;
    Material material;
    std::uint32_t raster = 0;
    std::vector<Vertex> vertices;
    std::vector<std::uint16_t> indices;
};
struct StaticModel
{
    std::uint32_t id = 0;
    std::vector<Packet> packets;
};
// Bounded static RLG profile. Skinning, animation and unknown materials fail explicitly.
std::vector<StaticModel> ReadStaticModels(Bytes data, std::optional<std::uint32_t> selected = {});
struct EffectsVertexAnimation
{
    std::uint32_t model, frames, vertices, stride, unknown;
    std::vector<std::uint32_t> streams;
    std::vector<std::array<float,3>> positions; // Source frame/packet/vertex order.
};
struct EffectsGeometry
{
    std::vector<StaticModel> models;
    std::vector<EffectsVertexAnimation> animations;
    // RLGReader copies these chunks into its skin-data inventory unchanged.
    // Retained metadata does not create a skinned-particle runtime.
    std::map<std::uint32_t,std::vector<std::uint8_t>> skin_metadata;
};
// Original effectsgeometry.bun: source-ordered model groups plus the qualified
// position-only vertex animations. Coordinates stay local until an effect
// supplies its transform. No GL registration or manager readiness is implied.
EffectsGeometry ReadEffectsGeometry(Bytes data);
struct StaticWorldModel
{
    StaticModel model;
    Bytes textures; // Borrows the decompressed world buffer, until textures are decoded.
};
// Standalone previews bake packet matrices. WorldDrawable::DrawToView replaces
// them with the instance matrix, so world instances must retain local vertices.
enum class ModelCoordinates { BakedPacket, Local };
// Decode one explicit static model; other materials are not interpreted. The
// world object graph, animation and original world loading remain separate.
StaticWorldModel ReadStaticWorldModel(Bytes data, std::uint32_t selected,
    ModelCoordinates coordinates = ModelCoordinates::BakedPacket);
}
