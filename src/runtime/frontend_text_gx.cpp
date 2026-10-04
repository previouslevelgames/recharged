#include "runtime/frontend_text_gx.h"
#include <dolphin/gx.h>
#include <dolphin/mtx.h>
#include <cmath>
#include <limits>

namespace mscharged
{
void DrawFrontendText(const resources::FontLayout& layout, float x, float y,
                      unsigned width, unsigned height, std::array<std::uint8_t, 4> colour)
{
    resources::Require(std::isfinite(x) && std::isfinite(y) && std::abs(x) < 1e6f && std::abs(y) < 1e6f,
        "Invalid frontend text position");
    DrawFrontendText(layout, TextDrawTransform{{{1,0,0,x}, {0,1,0,y}, {0,0,1,0}}}, width, height, colour);
}
void DrawFrontendText(const resources::FontLayout& layout, const TextDrawTransform& transform,
                      unsigned width, unsigned height, std::array<std::uint8_t, 4> colour)
{
    using resources::Require;
    Require(layout.font && layout.quads.size() <= 4096 && width && height && width <= 16384 && height <= 16384,
        "Invalid frontend text draw");
    for (const auto& row : transform)
        for (float value : row)
            Require(std::isfinite(value) && std::abs(value) <= 1e7f, "Invalid frontend text transform");
    // Validate every record before changing state or issuing any draw.
    for (const auto& quad : layout.quads)
    {
        Require(quad.page < layout.font->pages.size(), "Invalid frontend font page");
        for (float value : {quad.left, quad.right, quad.top, quad.bottom, quad.u0, quad.v0, quad.u1, quad.v1})
            Require(std::isfinite(value) && std::abs(value) <= 1e7f, "Invalid frontend text quad");
        const auto& page = layout.font->pages[quad.page];
        // This renderer initially selects the actual USA CI8/RGB5A3 font profile.
        Require(page.gx_format == 9 && page.game_format == 8 && page.levels == 1 && page.width && page.height
            && page.palette_entries && page.palette_entries <= 256
            && page.palette.size() == std::size_t(page.palette_entries) * 2
            && page.pixels.size() == std::size_t((page.width + 7) / 8) * ((page.height + 3) / 4) * 32,
            "Unsupported or malformed font texture draw profile");
    }
    Mtx44 projection{{2.0f / width, 0, 0, -1}, {0, -2.0f / height, 0, 1}, {0, 0, -1, 0}, {0, 0, 0, 1}};
    Mtx model;
    for (unsigned row = 0; row < 3; ++row)
        for (unsigned col = 0; col < 4; ++col) model[row][col] = transform[row][col];
    GXSetProjection(projection, GX_ORTHOGRAPHIC);
    GXLoadPosMtxImm(model, GX_PNMTX0); GXSetCurrentMtx(GX_PNMTX0);
    GXSetViewport(0, 0, width, height, 0, 1); GXSetScissor(0, 0, width, height);
    GXSetCullMode(GX_CULL_NONE); GXSetClipMode(GX_CLIP_ENABLE);
    GXSetZMode(GX_FALSE, GX_ALWAYS, GX_FALSE); GXSetZCompLoc(GX_FALSE);
    GXSetBlendMode(GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, GX_LO_CLEAR);
    GXSetAlphaCompare(GX_GREATER, 0, GX_AOP_AND, GX_ALWAYS, 0);
    GXSetColorUpdate(GX_TRUE); GXSetAlphaUpdate(GX_TRUE);
    GXSetFog(GX_FOG_NONE, 0, 0, 0, 0, GXColor{0, 0, 0, 0});
    GXSetNumChans(1); GXSetChanCtrl(GX_COLOR0A0, GX_FALSE, GX_SRC_REG, GX_SRC_VTX, GX_LIGHT_NULL, GX_DF_NONE, GX_AF_NONE);
    GXSetNumTexGens(1); GXSetTexCoordGen(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_IDENTITY);
    GXSetNumTevStages(1); GXSetNumIndStages(0); GXSetTevDirect(GX_TEVSTAGE0);
    GXSetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR0A0); GXSetTevOp(GX_TEVSTAGE0, GX_MODULATE);
    GXSetTevSwapMode(GX_TEVSTAGE0, GX_TEV_SWAP0, GX_TEV_SWAP0);
    GXSetTevSwapModeTable(GX_TEV_SWAP0, GX_CH_RED, GX_CH_GREEN, GX_CH_BLUE, GX_CH_ALPHA);
    GXClearVtxDesc(); GXSetVtxDesc(GX_VA_POS, GX_DIRECT); GXSetVtxDesc(GX_VA_CLR0, GX_DIRECT); GXSetVtxDesc(GX_VA_TEX0, GX_DIRECT);
    GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
    GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0);
    GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_TEX0, GX_TEX_ST, GX_F32, 0);
    unsigned bound_page = std::numeric_limits<unsigned>::max();
    for (const auto& quad : layout.quads)
    {
        if (bound_page != quad.page)
        {
            const auto& page = layout.font->pages[quad.page]; GXTexObj texture{}; GXTlutObj palette{};
            GXInitTlutObj(&palette, page.palette.data(), GX_TL_RGB5A3, page.palette_entries); GXLoadTlut(&palette, GX_TLUT0);
            GXInitTexObjCI(&texture, page.pixels.data(), page.width, page.height, GX_TF_C8, GX_CLAMP, GX_CLAMP, GX_FALSE, GX_TLUT0);
            GXInitTexObjLOD(&texture, GX_LINEAR, GX_LINEAR, 0, 0, 0, GX_FALSE, GX_FALSE, GX_ANISO_1);
            GXLoadTexObj(&texture, GX_TEXMAP0); bound_page = quad.page;
        }
        const auto vertex = [&](float px, float py, float u, float v)
        { GXPosition3f32(px, py, 0); GXColor4u8(colour[0], colour[1], colour[2], colour[3]); GXTexCoord2f32(u, v); };
        GXBegin(GX_QUADS, GX_VTXFMT0, 4);
        vertex(quad.left, quad.top, quad.u0, quad.v0); vertex(quad.right, quad.top, quad.u1, quad.v0);
        vertex(quad.right, quad.bottom, quad.u1, quad.v1); vertex(quad.left, quad.bottom, quad.u0, quad.v1);
        GXEnd();
    }
}
}
