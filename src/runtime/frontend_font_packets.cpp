#include "runtime/frontend_font_packets.h"
#include "runtime/views.h"
#include "NL/gl/glMatrix.h"
#include "NL/FontTextState.h"
#include "NL/gl/gl.h"
#include "NL/gl/glView.h"
#include <cmath>
namespace mscharged::detail
{
void CheckFrontendCoordinate(float value)
{ resources::Require(std::isfinite(value) && std::abs(value) <= 1e7f,"Invalid frontend polygon coordinate or matrix"); }
FrontendPacketState::FrontendPacketState() : raster_(glHandleizeRasterState()), texture_(glHandleizeTextureState())
{
    glStateSave(bundle_);
    // Fail before installing the noexcept restoration scope if a caller left
    // an expired matrix handle in its state bundle.
    glStateRestore(bundle_);
}
FrontendPacketState::~FrontendPacketState()
{
    for (unsigned i=0;i<GLS_Num;++i)
        glSetRasterState(static_cast<eGLState>(i),glGetRasterState(raster_,static_cast<eGLState>(i)));
    for (unsigned i=0;i<GLTS_Num;++i)
        glSetTextureState(static_cast<eGLTextureState>(i),glGetTextureState(texture_,static_cast<eGLTextureState>(i)));
    glStateRestore(bundle_);
}
FontPackets PrepareFontPackets(const resources::FontLayout& layout,const nlMatrix4& model,std::array<std::uint8_t,4> colour)
{
    resources::Require(layout.font && layout.quads.size() <= 4096,"Invalid font polygon batch");
    for (float value:model.e) CheckFrontendCoordinate(value);
    FontPackets result; result.model=model; result.quads.resize(layout.quads.size());
    for (std::size_t i=0;i<layout.quads.size();++i)
    {
        const auto& q=layout.quads[i]; auto& poly=result.quads[i];
        resources::Require(q.page < layout.font->pages.size(),"Font polygon page is not registered");
        for (float value:{q.left,q.top,q.right,q.bottom,q.u0,q.v0,q.u1,q.v1}) CheckFrontendCoordinate(value);
        for (float value:{q.u0,q.v0,q.u1,q.v1})
            resources::Require(value*1024.f>=-32768.f && value*1024.f<32768.f,"Font polygon UV exceeds original signed16 storage");
        poly.m_pos[0]={q.left,q.top}; poly.m_pos[1]={q.left,q.bottom};
        poly.m_pos[2]={q.right,q.bottom}; poly.m_pos[3]={q.right,q.top};
        poly.m_uv[0]={q.u0,q.v0}; poly.m_uv[1]={q.u0,q.v1}; poly.m_uv[2]={q.u1,q.v1}; poly.m_uv[3]={q.u1,q.v0};
        poly.depth=0; poly.SetColour(nlColour{{colour[0],colour[1],colour[2],colour[3]}});
    }
    for (std::size_t begin=0;begin<result.quads.size();)
    {
        const auto page=layout.quads[begin].page; auto end=begin+1;
        while (end<result.quads.size() && layout.quads[end].page==page) ++end;
        result.runs.push_back({begin,end,layout.font->pages[page].id}); begin=end;
    }
    return result;
}
void AttachFontPackets(GLView& view,const FontPackets& input,int layer)
{
    if (input.quads.empty()) return;
    FrontendPacketState state;
    FontSetTextRasterState();
    auto matrix=glAllocSetMatrix(input.model);
    if (matrix==GL_INVALID_MATRIX) throw std::runtime_error("Font polygon matrix allocation failed");
    // Preserve original page/line ordering; never merge nonadjacent runs.
    for (const auto& run:input.runs)
    {
        glSetCurrentTexture(run.texture,GLTT_Diffuse);
        if (!glAttachPoly2(&view,layer,run.end-run.begin,const_cast<glPoly2*>(input.quads.data()+run.begin),&matrix))
            throw std::runtime_error("Original font polygon attachment failed");
    }
}
}
