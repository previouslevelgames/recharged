#include "runtime/world_render.h"
#include "runtime/views.h"
#include "Game/Render/Frustum.h"
#include "NL/gl/glMemory.h"
#include "NL/gl/glState.h"
#include <cmath>
#include <stdexcept>

namespace mscharged
{
StaticWorldFrustum StaticWorldFrustum::FromCamera(const nlMatrix4& view, const nlMatrix4& projection)
{
    for (unsigned i = 0; i < 16; ++i)
        if (!std::isfinite(view.e[i]) || !std::isfinite(projection.e[i]))
            throw std::invalid_argument("Static world camera matrices must be finite");
    // NL transforms row vectors, whereas C_MTXPerspective/C_MTXOrtho provide
    // SDK column-vector matrices to GX. Combine projection * transpose(view).
    // GX clip depth is -w <= z <= 0. Original ExtractFrustumPlanes converts it
    // to 0..w by subtracting one from m33, relying on perspective w = -z;
    // extracting the clip inequalities directly also handles orthographic w=1.
    // Keep the original float transform precision, including cancellation of
    // translated clip boundaries; normalize in double to avoid squaring overflow.
    float clip[4][4]{};
    for (unsigned row = 0; row < 4; ++row)
        for (unsigned column = 0; column < 4; ++column)
            for (unsigned k = 0; k < 4; ++k)
                clip[row][column] += projection.e2[row][k] * view.e2[column][k];
    std::array<nlVector4, 6> planes;
    for (unsigned face = 0; face < planes.size(); ++face)
    {
        float p[4];
        for (unsigned i = 0; i < 4; ++i)
        {
            if (face < 4) p[i] = clip[3][i] + ((face & 1) ? -1 : 1) * clip[face / 2][i];
            else p[i] = face == 4 ? clip[3][i] + clip[2][i] : -clip[2][i];
        }
        const double length = std::sqrt(double(p[0])*p[0] + double(p[1])*p[1] + double(p[2])*p[2]);
        if (!(length > 0) || !std::isfinite(length))
            throw std::invalid_argument("Static world camera produces a degenerate frustum");
        planes[face] = {float(p[0]/length), float(p[1]/length), float(p[2]/length), float(p[3]/length)};
    }
    return StaticWorldFrustum(planes);
}

StaticWorldFrustum::StaticWorldFrustum(const std::array<nlVector4, 6>& planes) : planes_(planes)
{
    for (const auto& p : planes_)
    {
        const double length2 = double(p.x)*p.x + double(p.y)*p.y + double(p.z)*p.z;
        if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z)
            || !std::isfinite(p.w) || std::abs(length2 - 1.0) > 0.001)
            throw std::invalid_argument("Static world frustum requires finite normalized planes");
    }
}

bool StaticWorldFrustum::Visible(const resources::StaticWorldObject& object) const
{
    resources::ValidateStaticWorldObject(object);
    // Preserve the source's static rules: a common drawable's radius is not
    // rescaled by its matrix; a stadium box is already in world coordinates.
    if (object.type == 0x101)
    {
        const nlVector3 centre{object.transform[12], object.transform[13], object.transform[14]};
        return ClassifySphereInFrustum(planes_.data(), &centre, object.radius) != FRUSTUM_OUTSIDE;
    }
    if (object.type == 0x10002)
    {
        const nlVector3 minimum{object.bounds_min[0], object.bounds_min[1], object.bounds_min[2]};
        const nlVector3 maximum{object.bounds_max[0], object.bounds_max[1], object.bounds_max[2]};
        return ClassifyBoxInFrustum(planes_.data(), &minimum, &maximum, nullptr) != FRUSTUM_OUTSIDE;
    }
    throw std::invalid_argument("Unsupported static world drawable type");
}

StaticWorldSubmission SubmitStaticWorld(const StaticWorldObjects& world,
    GLView& opaque, GLView& alpha, const StaticWorldFrustum& frustum, bool cull)
{
    if (!OriginalViewsReady() || glNativeViewDispatchActive())
        throw std::logic_error("Static world submission requires idle initialized views");
    if (glGetCurrentResourcePool() != &world.Pool())
        throw std::logic_error("Select the static world resource pool before submission and dispatch");
    if (!opaque.m_Interface || !alpha.m_Interface)
        throw std::invalid_argument("Static world views require matrices");
    StaticWorldSubmission result;
    for (const auto& object : world.Objects())
    {
        ++result.objects;
        if (cull && !frustum.Visible(object.record)) continue;
        ++result.visible;
        for (unsigned long index = 0; index < object.model->numPackets; ++index)
        {
            const auto* packet = &object.model->packets[index];
            // Original WorldDrawable::DrawToView layer keys, including alpha
            // modes other than conventional source-alpha blending.
            if (glGetRasterState(packet->rasterState, GLS_AlphaBlend) == 0)
            {
                opaque.AttachPacket(packet, 0);
                ++result.opaque_packets;
            }
            else
            {
                alpha.AttachPacket(packet, 1);
                ++result.alpha_packets;
            }
        }
    }
    return result;
}
}
