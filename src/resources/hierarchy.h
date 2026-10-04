#pragma once
#include "resources/binary_reader.h"
#include <array>
#include <string>
#include <vector>

namespace mscharged::resources
{
// The selected PoseAccumulator uses 32 entries for its parent/scale stacks.
inline constexpr unsigned MaximumHierarchyDepth = 31; // Root has depth zero.
inline constexpr unsigned MaximumHierarchyNodes = 4096;
struct HierarchyNode
{
    std::uint32_t id = 0;
    int parent = -1, mirror = 0;
    std::vector<int> children;
    std::array<float, 3> translation{};
    std::uint8_t preserve_bone_length = 0;
};
struct HierarchyData
{
    std::string name;
    std::uint32_t hash = 0;
    int pelvis = -1, spine = -1;
    unsigned maximum_depth = 0;
    std::vector<HierarchyNode> nodes;
};
// Wii SHierarchy has translation offsets, not bind matrices. Disk pointers and
// push/pop scratch words are discarded. Every returned value owns its storage.
HierarchyData ReadHierarchy(Bytes file);
// Embedded roots retain alignment relative to their containing file.
HierarchyData ReadHierarchy(Bytes file, std::size_t offset, std::size_t end);
}
