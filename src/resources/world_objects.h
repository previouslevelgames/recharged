#pragma once
#include "resources/binary_reader.h"
#include <array>
#include <vector>

namespace mscharged::resources
{
inline constexpr std::size_t MaximumWorldObjects = 16384;
struct WorldObjectRecord
{
    std::uint32_t id, type;
    std::size_t offset, size; // In the decompressed resident world file.
    bool animated = false;
};
struct StaticWorldObject
{
    std::uint32_t id = 0, type = 0, model = 0, creation_flags = 0;
    std::array<float, 16> transform{}; // Original row-major nlMatrix4, no normalization.
    float radius = 0;
    std::array<float, 3> bounds_min{}, bounds_max{}; // Stadium world-space box.
};

// Index the resident world's object stream (including parent records), checking
// every extent. Known unsupported types are reported, never instantiated. The
// common type extents come from World::CreateObject; stadium extents follow its
// Wii factory and declared records. An unknown type cannot be skipped safely.
std::vector<WorldObjectRecord> ReadWorldObjectIndex(Bytes resident);
// Explicit static subset only: WorldDrawable (0x101) and plain
// StadiumWorldDrawable (0x10002). Animation bindings, parent payloads and
// task/camera-dependent stadium flags reject. This is not the full World loader.
std::vector<StaticWorldObject> ReadStaticWorldObjects(Bytes resident,
    std::span<const std::uint32_t> selected);
// Also validate host-authored records before allocating native objects.
void ValidateStaticWorldObject(const StaticWorldObject& object);
}
