#include "runtime/frontend_image_gx.h"
#include <dolphin/gx.h>
#include <dolphin/mtx.h>
#include <cmath>

namespace mscharged
{
void DrawFrontendImage(const resources::FrontendLayoutImage& image,unsigned width,unsigned height)
{
    using resources::Require;
    Require(image.texture&&width&&height&&width<=16384&&height<=16384&&image.blend<=7,"Invalid frontend image draw");
    resources::ValidateFrontendImageTexture(*image.texture);
    Require(image.texture->id!=resources::FrontendNameHash("movie")&&image.texture->id!=resources::FrontendNameHash("target/grab_texture"),
        "Dynamic frontend image requires its original service");
    for(float value:image.transform)Require(std::isfinite(value)&&std::abs(value)<=1e7f,"Invalid frontend image transform");
    for(const auto& vertex:image.vertices)for(float value:{vertex.x,vertex.y,vertex.u,vertex.v})
        Require(std::isfinite(value)&&std::abs(value)<=1e7f,"Invalid frontend image vertex");
    for(const auto& vertex:image.vertices)for(float value:{vertex.u,vertex.v})
        Require(value*1024.f>=-32768.f&&value*1024.f<32768.f,"Frontend image UV exceeds original signed16 storage");
    const auto& m=image.transform;
    Mtx model{{m[0],m[4],0,m[12]},{m[1],m[5],0,m[13]},{0,0,0,0}};
    Mtx44 projection{{2.0f/width,0,0,-1},{0,-2.0f/height,0,1},{0,0,-1,0},{0,0,0,1}};
    GXSetProjection(projection,GX_ORTHOGRAPHIC);GXLoadPosMtxImm(model,GX_PNMTX0);GXSetCurrentMtx(GX_PNMTX0);
    GXSetViewport(0,0,width,height,0,1);GXSetScissor(0,0,width,height);
    GXSetCullMode(GX_CULL_NONE);GXSetClipMode(GX_CLIP_ENABLE);
    GXSetZMode(GX_FALSE,GX_ALWAYS,GX_FALSE);GXSetZCompLoc(GX_FALSE);
    // Original glx_SwitchRaster mappings for GLS_AlphaBlend 0..7.
    const GXBlendFactor source[]={GX_BL_ONE,GX_BL_SRCALPHA,GX_BL_ONE,GX_BL_SRCALPHA,GX_BL_DSTCLR,GX_BL_INVDSTCLR,GX_BL_ONE,GX_BL_DSTCLR};
    const GXBlendFactor dest[]={GX_BL_ZERO,GX_BL_INVSRCALPHA,GX_BL_ONE,GX_BL_ONE,GX_BL_ZERO,GX_BL_ONE,GX_BL_ZERO,GX_BL_ZERO};
    GXSetBlendMode(image.blend==0?GX_BM_NONE:image.blend==7?GX_BM_SUBTRACT:GX_BM_BLEND,source[image.blend],dest[image.blend],GX_LO_CLEAR);
    // Original enabled frontend view permits alpha writes. Disabled-view/custom
    // framebuffer policies are outside this final-pass draw API.
    GXSetAlphaCompare(GX_GREATER,0,GX_AOP_AND,GX_ALWAYS,0);GXSetColorUpdate(GX_TRUE);GXSetAlphaUpdate(GX_TRUE);
    GXSetFog(GX_FOG_NONE,0,0,0,0,GXColor{0,0,0,0});
    GXSetNumChans(1);GXSetChanCtrl(GX_COLOR0A0,GX_FALSE,GX_SRC_REG,GX_SRC_VTX,GX_LIGHT_NULL,GX_DF_NONE,GX_AF_NONE);
    GXSetNumTexGens(1);GXSetTexCoordGen(GX_TEXCOORD0,GX_TG_MTX2x4,GX_TG_TEX0,GX_IDENTITY);
    GXSetNumTevStages(1);GXSetNumIndStages(0);GXSetTevDirect(GX_TEVSTAGE0);
    GXSetTevOrder(GX_TEVSTAGE0,GX_TEXCOORD0,GX_TEXMAP0,GX_COLOR0A0);GXSetTevOp(GX_TEVSTAGE0,GX_MODULATE);
    GXSetTevSwapMode(GX_TEVSTAGE0,GX_TEV_SWAP0,GX_TEV_SWAP0);
    GXSetTevSwapModeTable(GX_TEV_SWAP0,GX_CH_RED,GX_CH_GREEN,GX_CH_BLUE,GX_CH_ALPHA);
    const auto& texture=*image.texture;GXTexObj object{};GXTlutObj palette{};
    // Original default DiffuseWrap0 produces repeat in glQuad3/glxTexture.
    if(texture.palette_entries)
    {
        GXInitTlutObj(&palette,texture.palette.data(),GX_TL_RGB5A3,texture.palette_entries);GXLoadTlut(&palette,GX_TLUT0);
        GXInitTexObjCI(&object,texture.pixels.data(),texture.width,texture.height,GXCITexFmt(texture.gx_format),GX_REPEAT,GX_REPEAT,texture.levels>1,GX_TLUT0);
    }
    else GXInitTexObj(&object,texture.pixels.data(),texture.width,texture.height,GXTexFmt(texture.gx_format),GX_REPEAT,GX_REPEAT,texture.levels>1);
    GXInitTexObjLOD(&object,texture.levels==1?GX_LINEAR:texture.palette_entries?GX_LIN_MIP_NEAR:GX_LIN_MIP_LIN,
        GX_LINEAR,0,texture.levels-1,0,GX_FALSE,GX_FALSE,GX_ANISO_1);
    GXLoadTexObj(&object,GX_TEXMAP0);
    GXClearVtxDesc();GXSetVtxDesc(GX_VA_POS,GX_DIRECT);GXSetVtxDesc(GX_VA_CLR0,GX_DIRECT);GXSetVtxDesc(GX_VA_TEX0,GX_DIRECT);
    GXSetVtxAttrFmt(GX_VTXFMT0,GX_VA_POS,GX_POS_XYZ,GX_F32,0);
    GXSetVtxAttrFmt(GX_VTXFMT0,GX_VA_CLR0,GX_CLR_RGBA,GX_RGBA8,0);
    GXSetVtxAttrFmt(GX_VTXFMT0,GX_VA_TEX0,GX_TEX_ST,GX_S16,10);
    GXBegin(GX_QUADS,GX_VTXFMT0,4);
    for(const auto& vertex:image.vertices)
    {GXPosition3f32(vertex.x,vertex.y,0);GXColor4u8(image.colour[0],image.colour[1],image.colour[2],image.colour[3]);
        GXTexCoord2s16(static_cast<std::int16_t>(vertex.u*1024.f),static_cast<std::int16_t>(vertex.v*1024.f));}
    GXEnd();
}
}
