#include "resources/hierarchy.h"
#include "resources/chunk_reader.h"
#include <algorithm>
#include <bit>

namespace mscharged::resources
{
HierarchyData ReadHierarchy(Bytes file)
{ return ReadHierarchy(file, 0, file.size()); }
HierarchyData ReadHierarchy(Bytes file, std::size_t offset, std::size_t end)
{
    Require(file.size() <= MaximumAssetBytes, "SHierarchy exceeds the 16 MiB asset limit");
    const auto root = ReadChunk(file, offset, end);
    Require(root.id == 0x80018000 && root.next == end, "Invalid SHierarchy root or trailing data");
    // Initialize consumes these eleven chunks in this exact order. Validate
    // IDs as well as sizes before exposing data to its unchecked accessors.
    constexpr std::array<unsigned, 11> ids{
        0x18001, 0x18002, 0x18003, 0x18009, 0x18004, 0x18005,
        0x18006, 0x18007, 0x18008, 0x18010, 0x18011};
    std::array<Bytes, ids.size()> chunks;
    offset = std::size_t(root.payload.data() - file.data());
    end = offset + root.payload.size();
    for (std::size_t i = 0; i < ids.size(); ++i)
    {
        const auto c = ReadChunk(file, offset, end);
        Require(c.id == ids[i] && c.next <= end, "Missing, reordered or invalid SHierarchy child chunk");
        chunks[i] = c.payload;
        offset = c.next;
    }
    Require(offset == end, "Unexpected SHierarchy child or trailing bytes");
    Require(chunks[0].size() == 52, "Invalid Wii SHierarchy header size");
    const auto count = U32(chunks[0], 8);
    Require(count > 0 && count <= MaximumHierarchyNodes, "SHierarchy node count exceeds the selected 1..4096 range");
    // Wii cIdentifier contributes name pointer/hash; cSHierarchy contributes
    // count, six pointers, pelvis/spine and two more pointers. Pointer words
    // are stale export values, not offsets to follow or native object bytes.
    HierarchyData result;
    result.hash = U32(chunks[0], 4);
    result.pelvis = std::bit_cast<std::int32_t>(U32(chunks[0], 36));
    result.spine = std::bit_cast<std::int32_t>(U32(chunks[0], 40));
    for (int special : {result.pelvis, result.spine})
        Require(special >= -1 && special < int(count), "SHierarchy pelvis/spine index is out of bounds");
    const auto name = chunks[1];
    const auto nul = std::find(name.begin(), name.end(), 0);
    Require(nul != name.end() && nul != name.begin() && nul - name.begin() <= 255,
            "SHierarchy name is empty, unterminated or too long");
    Require(std::all_of(name.begin(), nul, [](auto c) { return c >= 32 && c < 127; }),
            "Unsupported SHierarchy name encoding");
    Require(name.size() == Align(std::size_t(nul - name.begin()) + 1, 4)
        && std::all_of(nul, name.end(), [](auto c) { return c == 0; }), "Invalid SHierarchy name padding");
    result.name.assign(name.begin(), nul);
    for (unsigned channel : {2u, 3u, 4u, 5u, 6u, 8u})
        Require(chunks[channel].size() == count * 4, "SHierarchy integer channel size disagrees with its node count");
    Require(chunks[7].size() == (count - 1) * 4, "SHierarchy child table does not describe one tree");
    Require(chunks[9].size() == count * 12 && chunks[10].size() == count,
            "SHierarchy translation/length channel size disagrees with its node count");
    result.nodes.resize(count);
    std::vector<unsigned> depth(count), incoming(count);
    std::size_t next_child = 0;
    for (unsigned i = 0; i < count; ++i)
    {
        auto& node = result.nodes[i];
        node.id = U32(chunks[2], i * 4);
        node.parent = std::bit_cast<std::int32_t>(U32(chunks[3], i * 4));
        Require(i == 0 ? node.parent == -1 : node.parent >= 0 && node.parent < int(i),
                "SHierarchy root/parent ordering is invalid");
        if (i != 0) depth[i] = depth[node.parent] + 1;
        Require(depth[i] <= MaximumHierarchyDepth, "SHierarchy exceeds the original 32-entry pose stack");
        result.maximum_depth = std::max(result.maximum_depth, depth[i]);
        node.mirror = std::bit_cast<std::int32_t>(U32(chunks[8], i * 4));
        Require(node.mirror >= 0 && node.mirror < int(count), "SHierarchy mirrored node is out of bounds");
        for (unsigned component = 0; component < 3; ++component)
            node.translation[component] = F32(chunks[9], i * 12 + component * 4);
        // Preserve the authored byte: the original accessor tests nonzero.
        node.preserve_bone_length = chunks[10][i];
        const auto children = U32(chunks[4], i * 4);
        Require(children <= count - 1 - next_child, "SHierarchy child counts overflow their table");
        node.children.reserve(children);
        for (unsigned j = 0; j < children; ++j)
        {
            const auto child = U32(chunks[7], next_child++ * 4);
            Require(child > i && child < count, "SHierarchy child ordering or index is invalid");
            Require(++incoming[child] == 1, "SHierarchy child has multiple incoming edges");
            node.children.push_back(int(child));
        }
    }
    Require(next_child == count - 1, "SHierarchy child counts leave unused records");
    for (unsigned i = 0; i < count; ++i)
        for (int child : result.nodes[i].children)
            Require(result.nodes[child].parent == int(i), "SHierarchy parent and child tables disagree");

    // BuildPushPopFlags writes node-1 when returning to an earlier depth;
    // PoseAccumulator iterates numeric node order. Require exactly the authored
    // depth-first preorder before either algorithm can run on native storage.
    std::vector<unsigned> pending{0};
    unsigned expected = 0;
    while (!pending.empty())
    {
        const auto node = pending.back(); pending.pop_back();
        Require(node == expected++, "SHierarchy nodes are not depth-first preorder");
        const auto& children = result.nodes[node].children;
        for (auto child = children.rbegin(); child != children.rend(); ++child)
            pending.push_back(unsigned(*child));
    }
    Require(expected == count, "SHierarchy contains unreachable nodes");
    return result;
}
}
