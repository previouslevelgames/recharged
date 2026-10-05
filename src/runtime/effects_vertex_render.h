#pragma once
#include "runtime/effects_vertex.h"
class GLView;
namespace mscharged
{
// Submit retained animated geometry through original model matrix allocation and
// GLView attachment. Authored packet/material/raster state stays intact. This
// does not apply ParticleSystem's separate model-particle transform/colour rules.
// Caller advances resources once before the collecting frame, then sends or
// cancels that frame and calls resources.FinishFrame() before release/reuse.
unsigned SubmitEffectsVertex(EffectsVertexResources&, GLView&, std::uint32_t model,
    const nlMatrix4& transform, int frame = -1, std::uint32_t layer = 0);
}
