#pragma once
#include "runtime/hierarchy_assets.h"
#include "runtime/sanim_assets.h"
#include "runtime/animation_retarget.h"
#include <array>
#include <optional>
#include <string>
#include <string_view>

namespace mscharged
{
struct CharacterAnimationProfile
{
    unsigned index;
    std::string_view name, hierarchy_path, animation_path, retarget_path;
};
// Generated from the original CharacterTemplate table, in source order 0..19.
const CharacterAnimationProfile& CharacterAnimation(unsigned index);
// World::LoadChunks assigns following animations to the most recent hierarchy,
// retaining that association across resident then temporary data. Node count
// equality and similarly named standalone files do not establish this identity.
// The world profile selects one authored direct-index set. The character profile
// selects the original CharacterTemplate hierarchy/FE-animation/retarget paths.
// Neither constructs a pose/controller. Native objects remain read-only and
// valid while their individual handles live.
class AnimationBundle
{
    HierarchyAsset::Handle hierarchy_;
    std::vector<SAnimAsset::Handle> animations_;
    AnimationRetargetAsset::Handle retargets_;
    std::vector<const AnimRetarget*> maps_;
    AnimationBundle() = default;
public:
    using Handle = std::shared_ptr<const AnimationBundle>;
    static Handle Decode(resources::Bytes resident, resources::Bytes temporary, std::uint32_t hierarchy_hash);
    static Handle DecodeCharacter(resources::Bytes hierarchy, resources::Bytes animations,
        resources::Bytes retargets, unsigned character);
    HierarchyAsset::Handle Hierarchy() const { return hierarchy_; }
    std::size_t Size() const { return animations_.size(); }
    SAnimAsset::Handle At(std::size_t index) const { return animations_.at(index); }
    // Original cInventory::AddStart/Find: the last file entry wins hash aliases.
    SAnimAsset::Handle Find(std::uint32_t hash) const;
    // cPN_SAnimController::Evaluate mirrors the target index before identity
    // mapping. Cross-signature/remapped sets require a qualified AnimRetarget
    // table and are rejected here; this method never synthesizes a pose.
    std::size_t AnimationNode(std::size_t node, bool mirror = false) const;
    // The map direction is target hierarchy -> source animation, after target
    // mirroring. nullopt is original -1: Evaluate uses identity rotation/scale
    // and the ORIGINAL target node's hierarchy translation; EvaluateScale skips
    // that node. This only exposes the decision and does not synthesize a pose.
    std::optional<std::size_t> MappedNode(std::size_t animation, std::size_t node, bool mirror = false) const;
    const AnimRetarget* Retarget(std::size_t animation) const;
    AnimationRetargetAsset::Handle Retargets() const { return retargets_; }
};

struct AnimationBundleRequest
{
    std::string resident, temporary; // Real NL paths, optionally ending .zlib.
    std::uint32_t hierarchy_hash = 0;
};
enum class AnimationBundleState { Idle, Loading, Ready, Failed, Cancelled };

// Two world reads or three character reads form one NL transaction. Begin
// replaces any pending request;
// only complete validation replaces Current(). Failures/cancellation retain the
// previous bundle. Unload drops this owner's current handle, not external ones.
// Service/mutate/destroy on the creating thread before NL/arena shutdown.
class AnimationBundleLoad
{
    struct Implementation;
    std::unique_ptr<Implementation> impl_;
    void BeginImpl(AnimationBundleRequest request, std::optional<unsigned> character);
public:
    static constexpr std::size_t MaximumRetainedBytes = 32 * 1024 * 1024;
    AnimationBundleLoad();
    ~AnimationBundleLoad();
    AnimationBundleLoad(const AnimationBundleLoad&) = delete;
    AnimationBundleLoad& operator=(const AnimationBundleLoad&) = delete;
    void Begin(AnimationBundleRequest request);
    // Three required original CharacterTemplate paths: hierarchy, FE animation
    // inventory and retarget inventory. Same transaction/replacement contract.
    void BeginCharacter(unsigned index);
    void Poll();
    void Service();
    void Cancel();
    void Unload();
    AnimationBundleState State() const;
    unsigned CompletedFiles() const;
    AnimationBundle::Handle Current() const;
    // Reports the latest transaction's error or pending/cancelled state.
    AnimationBundle::Handle Result() const;
};
}
