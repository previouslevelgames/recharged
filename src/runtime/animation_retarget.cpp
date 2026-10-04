#include "runtime/animation_retarget.h"
#include "Game/SAnim/AnimRetargeter.h"
#include <climits>

namespace mscharged
{
struct AnimationRetargetAsset::Storage : AnimRetargetList
{
    std::vector<AnimRetarget> records;
    std::vector<std::vector<signed short>> maps;
    explicit Storage(resources::AnimationRetargetListData decoded)
    {
        static_assert(sizeof(signed short) == 2 && CHAR_BIT == 8);
        records.resize(decoded.maps.size()); maps.resize(decoded.maps.size());
        for (std::size_t i = 0; i < maps.size(); ++i)
        {
            const auto& map = decoded.maps[i]; maps[i].assign(map.nodes.begin(), map.nodes.end());
            auto& record = records[i];
            record.m_TargetHierarchySignature = map.signature;
            record.m_NumBones = map.metadata04; // Preserve the reconstructed member's disk value.
            record.m_Unknown08 = maps[i].size();
            record.m_pMap = maps[i].empty() ? nullptr : maps[i].data();
        }
        m_szName = nullptr; m_uHashID = decoded.hash;
        m_NumAnimRetargets = records.size(); m_pAnimRetarget = records.empty() ? nullptr : records.data();
    }
};
AnimationRetargetAsset::AnimationRetargetAsset(resources::AnimationRetargetListData data)
    : storage_(std::make_unique<Storage>(std::move(data))) {}
AnimationRetargetAsset::~AnimationRetargetAsset() = default;
const AnimRetargetList& AnimationRetargetAsset::Data() const { return *storage_; }
const AnimRetarget* AnimationRetargetAsset::Find(std::uint32_t signature) const
{ return storage_->GetAnimRetargetWithSignature(signature); }
const AnimRetarget* AnimationRetargetAsset::Find(const cSAnim& animation) const
{ return storage_->GetAnimRetargetWithSignature(&animation); }
const AnimRetarget& AnimationRetargetAsset::RequireMap(const cSAnim& animation, std::size_t target_nodes) const
{
    const auto* map = Find(animation);
    if (!map) throw std::runtime_error("Animation hierarchy signature has no retained retarget map");
    // Exact authored destination count excludes truncated maps and accidental
    // pairing of similarly sized rigs. Identity still comes from source assets.
    if (map->m_Unknown08 != target_nodes) throw std::runtime_error("Retarget map does not cover the authored target hierarchy");
    for (std::size_t i = 0; i < target_nodes; ++i)
        if (map->m_pMap[i] < -1 || (map->m_pMap[i] >= 0 && unsigned(map->m_pMap[i]) >= animation.m_nNumNodes))
            throw std::runtime_error("Retarget map references a missing source animation node");
    return *map;
}
AnimationRetargetAssets::AnimationRetargetAssets(resources::Bytes file)
{
    auto data = resources::ReadAnimationRetargets(file); lists_.reserve(data.size());
    for (auto& list : data) lists_.push_back(AnimationRetargetAsset::Handle(new AnimationRetargetAsset(std::move(list))));
}
}
