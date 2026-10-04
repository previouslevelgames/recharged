#pragma once
#include "runtime/hierarchy_assets.h"
#include "NL/nlMath.h"
#include <array>
#include <cstdint>
#include <memory>

namespace mscharged
{
// Retains a checked hierarchy and real original accumulator arrays in MEM2.
// The game arenas outlive this owner. This executes the currently reconstructed
// original blend/BuildNodeMatrices code; it is not a full pose tree, SAnim
// interpolation, skinning, callback, replay or character implementation.
class PoseAccumulator
{
    struct Implementation;
    std::unique_ptr<Implementation> impl_;
public:
    explicit PoseAccumulator(HierarchyAsset::Handle hierarchy, bool store_previous = true);
    ~PoseAccumulator();
    PoseAccumulator(const PoseAccumulator&) = delete;
    PoseAccumulator& operator=(const PoseAccumulator&) = delete;
    // Original InitAccumulators: preserved-bone translations/weights survive.
    // Hides the current published pose until Build, retaining matrix history.
    void Reset();
    void BlendRotation(unsigned node, std::array<float,4>, float weight, bool mirror = false);
    void BlendAngle(unsigned node, std::uint16_t angle, float weight);
    void BlendScale(unsigned node, std::array<float,3>, float weight, bool mirror = false);
    void BlendTranslation(unsigned node, std::array<float,3>, float weight, bool mirror = false);
    void BlendRotationIdentity(unsigned node, float weight);
    void BlendScaleIdentity(unsigned node, float weight);
    void BlendTranslationIdentity(unsigned node, float weight);
    void MultiplyScale(unsigned node, std::array<float,3>, float weight);
    // Positive weights are bounded to[0,1]; original negligible threshold and
    // blend equations remain intact. Proper affine world matrices only.
    // Rejected preflight leaves the last successful published pose unchanged.
    void Build(const nlMatrix4& world, float scale = 1);
    unsigned Nodes() const;
    bool HasPose() const;
    nlMatrix4 Matrix(unsigned node) const;
    nlMatrix4 MatrixByHash(std::uint32_t id) const;
    nlMatrix4 PreviousMatrix(unsigned node) const;
    std::array<float,4> Quaternion(unsigned node) const;
    // Returned matrices/quaternions are copies and survive Release.
    void Release();
};
}
