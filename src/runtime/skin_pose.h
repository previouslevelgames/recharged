#pragma once
#include "resources/skin_model.h"
#include "runtime/animation_pose.h"
#include <memory>
namespace mscharged
{
// Source-authored model/binds/bone maps retained alongside the exact checked
// hierarchy used for posing. This is not a Character/GLSkinMesh service owner.
class RigidSkinAsset
{
    struct Storage;
    std::unique_ptr<Storage> storage_;
    RigidSkinAsset(resources::Bytes,HierarchyAsset::Handle,std::optional<std::uint32_t>);
public:
    using Handle=std::shared_ptr<const RigidSkinAsset>;
    static Handle Decode(resources::Bytes,HierarchyAsset::Handle,std::optional<std::uint32_t> selected={});
    ~RigidSkinAsset();
    const resources::RigidSkinModel& Data() const;
    HierarchyAsset::Handle Hierarchy() const;
    const std::vector<nlMatrix4>& InverseBinds() const;
    const std::vector<std::vector<unsigned>>& NodeMaps() const;
};
struct SkinMatrix3x4 { float values[3][4]; };
struct SkinPoseFrame
{
    using Handle=std::shared_ptr<const SkinPoseFrame>;
    RigidSkinAsset::Handle asset;
    AnimationPoseFrame::Handle pose;
    std::vector<nlMatrix4> matrices; // Original inverse-bind * global node pose.
    // Original GX3x4 layout. Rendering must compose packet model * view, then
    // GX modelview * each skin matrix, and use its inverse transpose for normals.
    // Bone slots are GX_PNMTX1..9; packet model is not baked into these arrays.
    std::vector<std::vector<SkinMatrix3x4>> packets;
};
// Immutable transactional publication: failure retains Current(); the caller
// retains a published frame until every render packet borrowing it is drained.
// No allocation in game arenas or hidden GPU/texture registration is performed.
class SkinPose
{
    struct Implementation;
    std::unique_ptr<Implementation> impl_;
public:
    explicit SkinPose(RigidSkinAsset::Handle);
    ~SkinPose();
    SkinPose(const SkinPose&)=delete;
    SkinPose& operator=(const SkinPose&)=delete;
    SkinPoseFrame::Handle Sample(AnimationPoseFrame::Handle);
    SkinPoseFrame::Handle Current() const;
    void Reset();
    void Release();
};
}
