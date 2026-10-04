#pragma once
#include "resources/world_objects.h"
#include "resources/static_model.h"
#include "resources/texture_bundle.h"

namespace mscharged::resources
{
inline constexpr std::size_t MaximumPreviewObjects = 256;
struct SceneBounds
{
    std::array<float, 3> center{};
    float radius = 0;
};
struct StaticWorldScene
{
    std::vector<StaticWorldObject> objects;
    std::vector<StaticModel> models;
    TextureBundle textures;
    SceneBounds bounds;
};
// Assemble an explicit subset from matching, decompressed resident/temporary
// world files. Every selected object must resolve; unsupported data is an error.
// Shared models/textures are decoded once and all output owns its storage.
StaticWorldScene ReadStaticWorldScene(Bytes resident, Bytes temporary,
    std::span<const std::uint32_t> objects);

struct UnavailableWorldObject
{
    std::uint32_t id, type;
    std::string reason;
};
struct AvailableWorldScene
{
    StaticWorldScene scene;
    std::size_t parent_records = 0;
    std::vector<UnavailableWorldObject> unavailable;
};
// Discover every drawable supported by the current static implementation.
// Unsupported features are returned explicitly; malformed selected data fails
// the whole load. This result is a partial scene, never a complete World owner.
AvailableWorldScene ReadAvailableWorldScene(Bytes resident, Bytes temporary);
}
