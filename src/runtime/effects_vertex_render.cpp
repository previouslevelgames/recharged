#include "runtime/effects_vertex_render.h"
#include "runtime/views.h"
#include "NL/gl/gl.h"
#include <climits>
#include <cmath>
#include <stdexcept>

namespace mscharged
{
unsigned SubmitEffectsVertex(EffectsVertexResources& resources,GLView& view,std::uint32_t id,
    const nlMatrix4& transform,int frame,std::uint32_t layer)
{
    if(!glIsFrameActive()||glNativeViewDispatchActive()||view.m_NativeIterating||!view.m_Interface)
        throw std::logic_error("Effects geometry submission requires a collecting original view");
    if(layer>INT_MAX)throw std::out_of_range("Effects geometry layer exceeds the qualified sort range");
    for(float value:transform.e)
        if(!std::isfinite(value)||std::abs(value)>1e7f)
            throw std::invalid_argument("Effects geometry transform must be finite and bounded");
    if(transform.e[3]!=0||transform.e[7]!=0||transform.e[11]!=0||transform.e[15]!=1)
        throw std::invalid_argument("Effects geometry transform must be affine");
    auto* model=resources.Model(id,frame);
    glModelSetMatrix(model,transform);
    view.AttachModel(model,layer);
    return model->numPackets;
}
}
