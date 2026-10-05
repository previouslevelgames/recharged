#pragma once
#include "runtime/weighted_skin_assets.h"
#include "runtime/animation_pose.h"
#include "runtime/skin_pose.h"

namespace mscharged
{
struct WeightedSkinPosePacket
{
    std::vector<SkinMatrix3x4> matrices;
    std::vector<std::array<float,3>> positions, normals;
};
struct WeightedSkinPoseFrame
{
    using Handle = std::shared_ptr<const WeightedSkinPoseFrame>;
    WeightedSkinAsset::Handle asset;
    AnimationPoseFrame::Handle pose;
    std::vector<nlMatrix4> matrices; // Source inverse-bind * global node pose.
    std::vector<WeightedSkinPosePacket> packets;
};

// Original nonrigid zero-morph software path, without actor or GL ownership.
// Published copies retain the exact asset/hierarchy/pose and isolated output
// arrays. A frame consumer must retain them through real draw completion.
// Nonzero morphs and rigid-only profiles are explicit unsupported boundaries.
class WeightedSkinPose final
{
    struct Implementation;
    std::unique_ptr<Implementation> impl_;
public:
    explicit WeightedSkinPose(WeightedSkinAsset::Handle);
    ~WeightedSkinPose();
    WeightedSkinPose(const WeightedSkinPose&) = delete;
    WeightedSkinPose& operator=(const WeightedSkinPose&) = delete;
    // Transactional: malformed/unsupported input or allocation failure leaves
    // Current() and every retained earlier frame unchanged.
    WeightedSkinPoseFrame::Handle Sample(AnimationPoseFrame::Handle, unsigned active_morphs = 0);
    WeightedSkinPoseFrame::Handle Current() const;
    void Reset();
    void Release();
};
}
