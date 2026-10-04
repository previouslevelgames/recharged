#pragma once
#include "runtime/animation_bundle.h"
#include "NL/nlMath.h"
#include <array>
#include <memory>
#include <span>
#include <vector>

namespace mscharged
{
struct AnimationPoseLayer
{
    std::size_t animation = 0;
    float time = 0; // Normalized, inclusive [0,1]. Playback/controller time is external.
    float weight = 1;
    bool mirror = false;
    bool scale_only = false; // Original EvaluateScale multiplicative channel path.
};
struct AnimationPoseFrame
{
    using Handle = std::shared_ptr<const AnimationPoseFrame>;
    HierarchyAsset::Handle hierarchy;
    std::vector<nlMatrix4> matrices, previous;
    std::vector<std::array<float,4>> quaternions;
};

// A retained bone-pose sampler, using the shared original SAnim interpolation,
// target-mirror/retarget decision and original accumulator/matrix equations.
// Root motion, morphs, PoseNode controllers, callbacks, replay and skinning are
// not evaluated. Morph-bearing full-pose layers are explicitly rejected.
// Two scratch accumulators use the real game arenas; this owner must be released
// on its creating thread before arena shutdown. Published copies survive it.
class AnimationPose
{
    struct Implementation;
    std::unique_ptr<Implementation> impl_;
public:
    explicit AnimationPose(AnimationBundle::Handle bundle);
    ~AnimationPose();
    AnimationPose(const AnimationPose&) = delete;
    AnimationPose& operator=(const AnimationPose&) = delete;
    // Layer order is meaningful, including unmapped-node fallback writes.
    // Any failure retains Current() and its previous matrices unchanged.
    AnimationPoseFrame::Handle Sample(std::span<const AnimationPoseLayer> layers,
        const nlMatrix4& world, float scale = 1);
    AnimationPoseFrame::Handle Current() const;
    void Reset(); // Hides publication and resets history; retained frames survive.
    void Release();
};
}
