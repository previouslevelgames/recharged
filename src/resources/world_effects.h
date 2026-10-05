#pragma once
#include "resources/world_objects.h"
#include <memory>
namespace mscharged::resources
{
struct WorldEffectRecord
{
    std::uint32_t id, group, creation_flags, probability;
    std::int32_t count, timing, animation_node;
    float interval, repeat_offset;
    std::array<float,16> matrix;
    bool animated, always_visible;
};
// Immutable disk records in original stream order. Serialized vtables, native
// pointers and prior runtime counters are never interpreted as live state.
class WorldEffectData
{
    std::vector<WorldEffectRecord> records_;
    explicit WorldEffectData(std::vector<WorldEffectRecord>);
public:
    using Handle=std::shared_ptr<const WorldEffectData>;
    static Handle Decode(Bytes resident);
    const std::vector<WorldEffectRecord>& Records() const { return records_; }
};
}
