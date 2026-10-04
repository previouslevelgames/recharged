#include "resources/world_objects.h"
#include "resources/chunk_reader.h"
#include <algorithm>
#include <map>
#include <optional>
#include <set>

namespace mscharged::resources
{
namespace
{
std::size_t RecordSize(std::uint32_t type)
{
    // World::CreateObject and the fixed Wii stadium factory record strides.
    // These describe file storage, never sizeof a native polymorphic object.
    switch (type)
    {
    case 0x101: case 0x108: case 0x10006: case 0x10007: return 0x70;
    case 0x102: return 0x30;
    case 0x103: case 0x106: case 0x10000: case 0x10002:
    case 0x10003: case 0x1000a: return 0x90;
    case 0x104: return 0x60;
    case 0x107: case 0x10001: case 0x10004: case 0x10005:
    case 0x10008: case 0x10009: return 0x80;
    case 0x109: return 0xa0;
    default: throw std::runtime_error("Unknown world object record type");
    }
}
struct Index
{
    std::vector<WorldObjectRecord> records;
    std::map<std::uint32_t, std::size_t> ids;
    std::set<std::uint32_t> parented;
};
void Supported(bool condition, const char* reason)
{ if (!condition) throw UnsupportedResource(reason); }
Index ReadIndex(Bytes file)
{
    Require(file.size() <= MaximumAssetBytes, "Resident world exceeds its budget");
    const auto root = ReadChunk(file, 0, file.size());
    Require(root.id == 0x80000001 && root.next == file.size(), "Invalid resident world root");
    const auto begin = std::size_t(root.payload.data() - file.data());
    const auto end = begin + root.payload.size();
    std::optional<Chunk> objects;
    for (auto pos = begin; pos < end;)
    {
        auto chunk = ReadChunk(file, pos, end);
        Require(chunk.next <= end, "World chunk padding exceeds its container");
        if (chunk.id == 0x26000)
        {
            Require(!objects, "Duplicate world object stream");
            objects = chunk;
        }
        // The index does not load other world services. Still bound each chunk.
        pos = chunk.next;
    }
    Require(objects.has_value(), "Resident world has no object stream");
    const auto bytes = objects->payload;
    Slice(bytes, 0, 16);
    const auto count = U32(bytes, 0);
    Require(count <= MaximumWorldObjects && count <= (bytes.size() - 16) / 16,
            "Invalid world object count");
    Index result;
    result.records.reserve(count);
    std::set<std::uint32_t> animated;
    std::optional<Bytes> parent;
    std::size_t pos = 16, binding_count = 0;
    for (std::uint32_t i = 0; i < count; ++i)
    {
        Slice(bytes, pos, 16);
        const auto id = U32(bytes, pos + 4), type = U32(bytes, pos + 8);
        const auto size = type == 0x10 ? U32(bytes, pos + 12) : RecordSize(type);
        Require(size >= 32 && size % 16 == 0, "Invalid world record size");
        const auto record = Slice(bytes, pos, size);
        result.records.push_back({id, type, std::size_t(record.data() - file.data()), size});
        if (type == 0x10)
        {
            Require(!parent, "Consecutive world parent records");
            parent = record;
        }
        else
        {
            Require(result.ids.emplace(id, result.records.size() - 1).second, "Duplicate world object ID");
            if (parent) result.parented.insert(id);
            if (type == 0x106)
            {
                // WorldAnimObject::Initialize reads these arrays from the
                // preceding parent's +0x20, then BindWorldAnimObjectDrawables
                // associates target IDs even when their saved controller is null.
                const auto bindings = U32(record, 0x60), animations = U32(record, 0x70);
                Require(bindings <= MaximumWorldObjects - binding_count && animations <= MaximumWorldObjects,
                        "World animation references exceed their budget");
                binding_count += bindings;
                Require(parent || (!bindings && !animations), "World animation references have no parent data");
                if (parent)
                {
                    const auto refs = Slice(*parent, 32, std::size_t(bindings) * 8 + std::size_t(animations) * 4);
                    for (std::size_t j = 0; j < bindings; ++j) animated.insert(U32(refs, j * 8));
                }
            }
            parent.reset();
        }
        pos += size;
    }
    Require(pos == bytes.size() && !parent, "Trailing world bytes or orphan parent record");
    for (auto& record : result.records) record.animated = animated.contains(record.id) && record.type != 0x10;
    return result;
}
}

std::vector<WorldObjectRecord> ReadWorldObjectIndex(Bytes resident)
{ return ReadIndex(resident).records; }

void ValidateStaticWorldObject(const StaticWorldObject& object)
{
    Supported(object.type == 0x101 || object.type == 0x10002, "Unsupported static world drawable type");
    Supported(object.creation_flags == 3, "Unsupported static world object lifecycle flags");
    for (float value : object.transform)
        Require(std::isfinite(value) && std::abs(value) <= 1e7f, "Invalid world transform");
    Require(object.transform[3] == 0 && object.transform[7] == 0 && object.transform[11] == 0
            && object.transform[15] == 1, "World transform must be affine");
    Require(std::isfinite(object.radius) && object.radius >= 0 && object.radius <= 1e7f,
            "Invalid world bounding radius");
    for (unsigned i = 0; i < 3; ++i)
        Require(std::isfinite(object.bounds_min[i]) && std::isfinite(object.bounds_max[i])
                && std::abs(object.bounds_min[i]) <= 1e7f && std::abs(object.bounds_max[i]) <= 1e7f
                && object.bounds_min[i] <= object.bounds_max[i], "Invalid world bounding box");
}

std::vector<StaticWorldObject> ReadStaticWorldObjects(Bytes resident, std::span<const std::uint32_t> selected)
{
    Require(!selected.empty() && selected.size() <= MaximumWorldObjects, "Select a bounded static world subset");
    const auto index = ReadIndex(resident);
    std::set<std::uint32_t> unique;
    std::vector<StaticWorldObject> result;
    result.reserve(selected.size());
    for (const auto id : selected)
    {
        Require(unique.insert(id).second, "Duplicate selected world object ID");
        const auto found = index.ids.find(id);
        Require(found != index.ids.end(), "Selected world object is absent");
        const auto& record = index.records[found->second];
        Supported(record.type == 0x101 || record.type == 0x10002, "Selected world object is not a supported static drawable");
        Supported(!record.animated && !index.parented.contains(id), "Selected world object requires animation or parent data");
        const auto bytes = Slice(resident, record.offset, record.size);
        Supported(U32(bytes, 0x14) == 0xffffffff, "Selected world object has an animation node");
        StaticWorldObject object;
        object.id = id; object.type = record.type; object.model = U32(bytes, 0x64);
        object.creation_flags = U32(bytes, 0xc);
        for (unsigned i = 0; i < 16; ++i) object.transform[i] = F32(bytes, 0x20 + i * 4);
        object.radius = F32(bytes, 0x60);
        if (record.type == 0x10002)
        {
            Supported(U32(bytes, 0x88) == 0 && F32(bytes, 0x8c) == 1,
                    "Selected stadium object requires dynamic visibility or material behavior");
            for (unsigned i = 0; i < 3; ++i)
            {
                object.bounds_min[i] = F32(bytes, 0x70 + i * 4);
                object.bounds_max[i] = F32(bytes, 0x7c + i * 4);
            }
        }
        ValidateStaticWorldObject(object);
        result.push_back(object);
    }
    return result;
}
}
