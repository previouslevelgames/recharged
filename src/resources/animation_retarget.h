#pragma once
#include "resources/binary_reader.h"
#include <vector>

namespace mscharged::resources
{
inline constexpr unsigned MaximumRetargetLists = 64, MaximumRetargetMaps = 256;
struct AnimationRetargetMap
{
    std::uint32_t signature = 0;
    // Serialized +4 is named m_NumBones in the reconstruction. Actual files
    // use 0/1 here; +8 holds the map length. Preserve the unconsumed word.
    std::int32_t metadata04 = 0;
    std::vector<std::int16_t> nodes;
};
struct AnimationRetargetListData
{
    std::uint32_t hash = 0;
    std::vector<AnimationRetargetMap> maps;
};
// Ordered 16-byte Wii list/record headers, signed-short maps, four-byte payload
// padding. Export pointer words and the padding halfword are never followed.
std::vector<AnimationRetargetListData> ReadAnimationRetargets(Bytes file);
}
