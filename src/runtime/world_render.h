#pragma once
#include "runtime/world_objects.h"
#include "NL/nlMath.h"
#include <array>

class GLView;
namespace mscharged
{
// Six inward-facing, normalized world-space planes. No camera is advanced here.
class StaticWorldFrustum
{
    std::array<nlVector4, 6> planes_;
public:
    explicit StaticWorldFrustum(const std::array<nlVector4, 6>& planes);
    // Original NL row-vector view and SDK/GX column-vector projection, exactly
    // as supplied to GLView. Supports perspective and orthographic projection.
    static StaticWorldFrustum FromCamera(const nlMatrix4& view, const nlMatrix4& projection);
    const std::array<nlVector4, 6>& Planes() const { return planes_; } // left/right/bottom/top/near/far
    bool Visible(const resources::StaticWorldObject& object) const;
};

struct StaticWorldSubmission
{
    std::size_t objects = 0, visible = 0, opaque_packets = 0, alpha_packets = 0;
};

// The static branches of World::Render / WorldDrawable::DrawToView. Uses the
// owner's persistent instance matrices and original GLView packet sorting.
// Select world.Pool() before submission and keep it selected through dispatch
// (texture IDs resolve in that pool). Both views and the frustum must describe
// the same camera. The caller orders opaque before alpha in the view graph.
// cull=false is an explicit diagnostic reference, submitting all selected objects.
// Nothing is ticked or dispatched here. Keep the owner alive until submitted
// packets are reset and GPU work is drained. On allocation/submission failure,
// discard the frame's packet lists before releasing or retrying the batch.
StaticWorldSubmission SubmitStaticWorld(const StaticWorldObjects& world,
    GLView& opaque, GLView& alpha, const StaticWorldFrustum& frustum, bool cull = true);
}
