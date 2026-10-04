#include "resources/animation_retarget.h"
#include "resources/chunk_reader.h"
#include "resources/hierarchy.h"

namespace mscharged::resources
{
std::vector<AnimationRetargetListData> ReadAnimationRetargets(Bytes file)
{
    Require(!file.empty() && file.size() <= MaximumAssetBytes, "Retarget inventory is empty or exceeds 16 MiB");
    std::vector<AnimationRetargetListData> result;
    for (std::size_t offset = 0; offset < file.size();)
    {
        Require(result.size() < MaximumRetargetLists, "Retarget inventory exceeds 64 lists");
        const auto root = ReadChunk(file, offset, file.size());
        Require(root.id == 0x80017104 && root.next <= file.size(), "Invalid retarget list root");
        const auto begin = std::size_t(root.payload.data() - file.data()), end = begin + root.payload.size();
        const auto header = ReadChunk(file, begin, end);
        Require(header.id == 0x17105 && header.payload.size() == 16 && header.next <= end,
            "Invalid 16-byte Wii retarget list header");
        const auto count = U32(header.payload, 8);
        Require(count <= MaximumRetargetMaps, "Retarget list count exceeds 256 maps");
        const auto container = ReadChunk(file, header.next, end);
        Require(container.id == 0x80017106 && container.next == end, "Invalid retarget record container");
        const auto start = std::size_t(container.payload.data() - file.data()), stop = start + container.payload.size();
        const auto records = ReadChunk(file, start, stop);
        Require(records.id == 0x17107 && records.payload.size() == count * 16 && records.next <= stop,
            "Retarget records disagree with their Wii count/stride");
        AnimationRetargetListData list; list.hash = U32(header.payload, 4); list.maps.reserve(count);
        auto at = records.next;
        for (unsigned i = 0; i < count; ++i)
        {
            const auto length = U32(records.payload, i * 16 + 8);
            Require(length <= MaximumHierarchyNodes, "Retarget map exceeds 4096 target nodes");
            const auto data = ReadChunk(file, at, stop);
            Require(data.id == 0x17108 && data.next <= stop && data.payload.size() == Align(length * 2, 4),
                "Retarget map length/padding disagrees with its declared node count");
            AnimationRetargetMap map;
            map.signature = U32(records.payload, i * 16);
            map.metadata04 = std::bit_cast<std::int32_t>(U32(records.payload, i * 16 + 4));
            map.nodes.reserve(length);
            for (unsigned j = 0; j < length; ++j)
            {
                const auto node = std::bit_cast<std::int16_t>(U16(data.payload, j * 2));
                Require(node >= -1, "Retarget map has an unsupported negative node sentinel");
                map.nodes.push_back(node);
            }
            list.maps.push_back(std::move(map)); at = data.next;
        }
        Require(at == stop, "Retarget list contains trailing or missing map chunks");
        result.push_back(std::move(list)); offset = root.next;
    }
    return result;
}
}
