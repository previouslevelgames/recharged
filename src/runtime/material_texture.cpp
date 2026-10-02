#include "NL/gl/glModel.h"
#include "NL/gl/glTextureManager.h"
#include "NL/glx/glxTexture.h"
#include <dolphin/gx.h>
#include <stdexcept>

void glx_BindTexture(int slot, glTextureBinding *binding)
{
    if (!binding || slot < 0 || slot > 7 || !glGetTextureManager())
        throw std::invalid_argument("Invalid native material texture binding");
    const auto *texture = glGetTextureManager()->GetTexture(binding);
    if (!texture)
        throw std::runtime_error("Material texture is missing from the native inventory");
    const unsigned formats[] = {4, 5, 14, 6, 1, 0, 1, 3, 9};
    if (texture->m_Format >= GXTex_Num || !texture->m_Levels || !texture->m_SwizzledData)
        throw std::runtime_error("Invalid native texture metadata");
    const auto format = formats[texture->m_Format];
    const auto wrap_s = binding->flags & 1 ? GX_CLAMP : GX_REPEAT;
    const auto wrap_t = binding->flags & 2 ? GX_CLAMP : GX_REPEAT;
    // Aurora's objects are larger than the Wii fixed arrays in PlatTexture.
    // GXLoad* copies their state; the pool owns the retained pixel/palette data.
    GXTexObj object{};
    GXTlutObj palette{};
    if (texture->m_nPaletteEntries)
    {
        GXInitTlutObj(&palette, texture->m_PaletteData, GX_TL_RGB5A3, texture->m_nPaletteEntries);
        GXLoadTlut(&palette, slot);
        GXInitTexObjCI(&object, texture->m_SwizzledData, texture->m_Width, texture->m_Height, GXCITexFmt(format),
                       wrap_s, wrap_t, texture->m_Levels > 1, slot);
    }
    else
        GXInitTexObj(&object, texture->m_SwizzledData, texture->m_Width, texture->m_Height, GXTexFmt(format), wrap_s,
                     wrap_t, texture->m_Levels > 1);
    GXInitTexObjLOD(&object,
                    texture->m_Levels == 1       ? GX_LINEAR
                    : texture->m_nPaletteEntries ? GX_LIN_MIP_NEAR
                                                 : GX_LIN_MIP_LIN,
                    GX_LINEAR, 0, texture->m_Levels - 1, 0, GX_FALSE, GX_FALSE, GX_ANISO_1);
    GXLoadTexObj(&object, GXTexMapID(slot));
}
