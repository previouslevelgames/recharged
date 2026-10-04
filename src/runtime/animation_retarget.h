#pragma once
#include "resources/animation_retarget.h"
#include <memory>

class AnimRetarget;
class AnimRetargetList;
class cSAnim;
namespace mscharged
{
// Stable host-owned native records. Retain this handle while an original
// consumer borrows Data/Find; no original in-place loader or inventory destroy.
class AnimationRetargetAsset
{
    struct Storage;
    std::unique_ptr<Storage> storage_;
    explicit AnimationRetargetAsset(resources::AnimationRetargetListData data);
    friend class AnimationRetargetAssets;
public:
    using Handle = std::shared_ptr<const AnimationRetargetAsset>;
    ~AnimationRetargetAsset();
    const AnimRetargetList& Data() const;
    const AnimRetarget* Find(std::uint32_t signature) const;
    const AnimRetarget* Find(const cSAnim& animation) const;
    // Validate every map entry before an original consumer can index channels.
    // No matching signature is an error here; an authored character's original
    // direct fallback is a separate checked bundle decision.
    const AnimRetarget& RequireMap(const cSAnim& animation, std::size_t target_nodes) const;
};
class AnimationRetargetAssets
{
    std::vector<AnimationRetargetAsset::Handle> lists_;
public:
    explicit AnimationRetargetAssets(resources::Bytes file);
    std::size_t Size() const { return lists_.size(); }
    AnimationRetargetAsset::Handle At(std::size_t index) const { return lists_.at(index); }
    // CharacterLoader uses cInventory::Find(0): last parsed list, not first.
    AnimationRetargetAsset::Handle Selected() const { return lists_.back(); }
};
}
