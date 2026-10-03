#include "runtime/materials.h"
#include "runtime/material_environment.h"
#include "runtime/lighting_state.h"
#include "NL/gl/glMaterialProgram.h"
#include "NL/gl/glState.h"
#include "NL/platvmath.h"
#include "NL/gl/glMatrix.h"
#include "NL/glx/glxMatrix.h"
#include "NL/glx/glxTexture.h"
#include "NL/glx/glxGX.h"
#include "NL/gl/glView.h"
#include "NL/glx/GXUnlitTextureMaterialProgram.h"
#include "NL/glx/GXVertexColourTextureMaterialProgram.h"
#include "NL/glx/GXScrollingDiffuseMaterialProgram.h"
#include "NL/glx/GXMaskedSpecularFresnelMaterialProgram.h"
#include "NL/glx/GXShadowVolumeMaterialProgram.h"
#include "NL/glx/GXSpecularDetailBlendMaterialProgram.h"
#include "NL/glx/GXScrollingSpecularMaterialProgram.h"
#include "NL/glx/GXCameraScrolledOverlayMaterialProgram.h"
#include <dolphin/gx.h>
#include <dolphin/mtx.h>
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace mscharged
{
namespace
{
bool programs_live = false, preview_live = false;
nlMatrix4 preview_view;
float preview_time = 0;
nlVector3 preview_camera;
bool preview_has_camera = false;
constexpr std::uint32_t unlit = 0x21db4385, vertex = 0xd3e572da, scrolling = 0x2169db5c, masked = 0x32475c7d;
constexpr std::uint32_t shadow_volume = 0x386ecbdd;
constexpr std::uint32_t detail_blend = 0x112ab470;
constexpr std::uint32_t scrolling_specular = 0x3eccd955;
constexpr std::uint32_t camera_overlay = 0x32bc21e8;
glTextureBinding Binding(const resources::MaterialBinding &input)
{
    if (input.flags & ~3u)
        throw std::invalid_argument("Invalid material texture flags");
    if (!glx_GetTex(input.texture))
        throw std::runtime_error("Material texture is missing from the native inventory");
    return {input.texture, static_cast<u8>(input.flags & 1), static_cast<u8>((input.flags >> 1) & 1)};
}
void Baseline()
{
    for (unsigned i = 0; i < 16; ++i)
    {
        const auto stage = GXTevStageID(i);
        GXSetTevColorOp(stage, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
        GXSetTevAlphaOp(stage, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
        GXSetTevKColorSel(stage, GX_TEV_KCSEL_1);
        GXSetTevKAlphaSel(stage, GX_TEV_KASEL_1);
        GXSetTevSwapMode(stage, GX_TEV_SWAP0, GX_TEV_SWAP0);
        GXSetTevDirect(stage);
    }
    for (unsigned i = 0; i < 8; ++i)
        GXSetTexCoordGen(GXTexCoordID(i), GX_TG_MTX2x4, GXTexGenSrc(GX_TG_TEX0 + i), GX_IDENTITY);
    GXSetChanCtrl(GX_COLOR0A0, GX_FALSE, GX_SRC_REG, GX_SRC_VTX, GX_LIGHT_NULL, GX_DF_NONE, GX_AF_NONE);
    GXSetChanCtrl(GX_COLOR1A1, GX_FALSE, GX_SRC_REG, GX_SRC_VTX, GX_LIGHT_NULL, GX_DF_NONE, GX_AF_NONE);
}
void Raster(u32 state)
{
    // Match the original glx_SwitchRaster mapping for the selected profile.
    for (int i = GLS_SolidOffset; i < GLS_Num; ++i)
        if (glGetRasterState(state, eGLState(i)))
            throw std::runtime_error("Unsupported material raster effect");
    const GXCompare depths[] = {GX_ALWAYS, GX_LEQUAL, GX_EQUAL, GX_LESS};
    const GXCullMode culling[] = {GX_CULL_NONE, GX_CULL_FRONT, GX_CULL_BACK, GX_CULL_ALL};
    const auto alpha = glGetRasterState(state, GLS_AlphaTest), colour = glGetRasterState(state, GLS_ColourWrite);
    gxSetZMode(glGetRasterState(state, GLS_DepthTest), depths[glGetRasterState(state, GLS_DepthFunc)],
               glGetRasterState(state, GLS_DepthWrite));
    GXSetCullMode(culling[glGetRasterState(state, GLS_Culling)]);
    GXSetAlphaCompare(alpha ? GX_GREATER : GX_ALWAYS, glGetRasterState(state, GLS_AlphaTestRef), GX_AOP_AND, GX_ALWAYS,
                      0);
    GXSetZCompLoc(!alpha);
    const GXBlendFactor source[] = {GX_BL_ONE,    GX_BL_SRCALPHA,  GX_BL_ONE, GX_BL_SRCALPHA,
                                    GX_BL_SRCCLR, GX_BL_INVSRCCLR, GX_BL_ONE, GX_BL_SRCCLR};
    const GXBlendFactor dest[] = {GX_BL_ZERO, GX_BL_INVSRCALPHA, GX_BL_ONE,  GX_BL_ONE,
                                  GX_BL_ZERO, GX_BL_ONE,         GX_BL_ZERO, GX_BL_ZERO};
    const auto blend = glGetRasterState(state, GLS_AlphaBlend);
    GXSetBlendMode(blend == 0   ? GX_BM_NONE
                   : blend == 7 ? GX_BM_SUBTRACT
                                : GX_BM_BLEND,
                   source[blend], dest[blend], GX_LO_CLEAR);
    gxSetColourUpdate(colour & 1);
    gxSetAlphaUpdate((colour >> 1) & 1);
}
} // namespace
struct MaterialPrograms::Impl
{
    GXUnlitTextureMaterialProgram unlit;
    GXVertexColourTextureMaterialProgram vertex;
    GXScrollingDiffuseMaterialProgram scrolling;
    GXMaskedSpecularFresnelMaterialProgram masked;
    GXShadowVolumeMaterialProgram shadow;
    GXSpecularDetailBlendMaterialProgram detail;
    GXScrollingSpecularMaterialProgram scrolling_highlight;
    GXCameraScrolledOverlayMaterialProgram overlay;
    Impl()
    {
        unlit.Initialize();
        vertex.Initialize();
        scrolling.Initialize();
        masked.Initialize();
        shadow.Initialize();
        detail.Initialize();
        scrolling_highlight.Initialize();
        overlay.Initialize();
    }
};
MaterialPrograms::MaterialPrograms()
{
    if (programs_live || glGetMaterialProgram(unlit) || glGetMaterialProgram(vertex) ||
        glGetMaterialProgram(scrolling) || glGetMaterialProgram(masked) || glGetMaterialProgram(shadow_volume)
        || glGetMaterialProgram(detail_blend) || glGetMaterialProgram(scrolling_specular)
        || glGetMaterialProgram(camera_overlay))
        throw std::logic_error("Material registry already initialized");
    try
    {
        impl_ = std::make_unique<Impl>();
        programs_live = true;
    }
    catch (...)
    {
        glClearMaterialPrograms();
        throw;
    }
}
MaterialPrograms::~MaterialPrograms()
{
    Release();
}
void MaterialPrograms::Release()
{
    if (!impl_)
        return;
    glClearMaterialPrograms();
    impl_.reset();
    programs_live = false;
}
std::size_t MaterialParameterSize(std::uint32_t id)
{
    auto *program = static_cast<GLMaterialProgram *>(glGetMaterialProgram(id));
    if (!program)
        throw std::runtime_error("Static material program is not registered");
    return program->parameterDataSize;
}
void InstallMaterial(glModelPacket &packet, const resources::Material &material, void *storage)
{
    static_assert(sizeof(GXMaterialParameter) == 12 && sizeof(glTextureBinding) == 8);
    static_assert(sizeof(GXScrollingDiffuseParameters) == 36 && sizeof(GXMaskedSpecularFresnelParameters) == 48);
    static_assert(sizeof(GXSpecularDetailBlendParameters) == 68);
    static_assert(sizeof(GXScrollingSpecularParameters) == 60);
    static_assert(sizeof(GXCameraScrolledOverlayParameters) == 48);
    auto *program = static_cast<GLMaterialProgram *>(glGetMaterialProgram(material.program));
    if (!program || !storage)
        throw std::invalid_argument("Unregistered material or missing parameter storage");
    for (float value : material.scalars)
        if (!std::isfinite(value) || std::abs(value) > 1e4f)
            throw std::invalid_argument("Invalid material scalar");
    for (auto value : material.switches)
        if (value > 1)
            throw std::invalid_argument("Invalid material switch");
    const auto binding = Binding(material.textures[0]);
    switch (material.program)
    {
    case camera_overlay:
        if ((material.scalars[0] != 0 && !std::isfinite(1.f / material.scalars[0]))
            || material.scalars[2] < 0 || material.scalars[2] > 1)
            throw std::invalid_argument("Invalid camera overlay scale/amount");
        new (storage) GXCameraScrolledOverlayParameters{binding,
            Binding(material.textures[1]), Binding(material.textures[2]),
            material.scalars[0], material.scalars[1], material.scalars[2],
            int(material.switches[0]), int(material.switches[1]), int(material.switches[2])};
        break;
    case scrolling_specular:
        if (material.scalars[0] < 0 || material.scalars[0] > 1 || material.scalars[1] < 0)
            throw std::invalid_argument("Invalid scrolling specular level/exponent");
        for (float value : material.specular_colour)
            if (!std::isfinite(value) || value < 0 || value > 1)
                throw std::invalid_argument("Invalid scrolling specular colour");
        new (storage) GXScrollingSpecularParameters{binding, Binding(material.textures[1]),
            material.scalars[0], material.scalars[1],
            {{material.specular_colour[0], material.specular_colour[1], material.specular_colour[2], material.specular_colour[3]}},
            material.scalars[2], material.scalars[3],
            int(material.switches[0]), int(material.switches[1]), int(material.switches[2])};
        break;
    case detail_blend:
        if (material.scalars[0] < 0 || material.scalars[0] > 1 || material.scalars[1] < 0 || material.scalars[1] > 1
            || material.scalars[2] < 0)
            throw std::invalid_argument("Invalid detail blend/specular scalar");
        for (float value : material.specular_colour)
            if (!std::isfinite(value) || value < 0 || value > 1)
                throw std::invalid_argument("Invalid detail specular colour");
        new (storage) GXSpecularDetailBlendParameters{binding,
            Binding(material.textures[1]), Binding(material.textures[2]), Binding(material.textures[3]),
            material.scalars[0], material.scalars[1], material.scalars[2],
            {{material.specular_colour[0], material.specular_colour[1], material.specular_colour[2], material.specular_colour[3]}},
            int(material.switches[0]), int(material.switches[1])};
        break;
    case unlit:
        new (storage) GXUnlitTextureParameters{binding};
        break;
    case shadow_volume:
        new (storage) GXShadowVolumeParameters{binding, int(material.switches[0])};
        break;
    case vertex:
        new (storage) GXVertexColourTextureParameters{binding};
        break;
    case scrolling:
        new (storage) GXScrollingDiffuseParameters{binding,
                                                   material.scalars[0],
                                                   material.scalars[1],
                                                   int(material.switches[0]),
                                                   int(material.switches[1]),
                                                   int(material.switches[2]),
                                                   int(material.switches[3]),
                                                   int(material.switches[4])};
        break;
    case masked:
        if (material.scalars[0] < 0 || material.scalars[0] > 1)
            throw std::invalid_argument("Invalid material specular amount");
        new (storage) GXMaskedSpecularFresnelParameters{binding,
                                                        Binding(material.textures[1]),
                                                        Binding(material.textures[2]),
                                                        material.scalars[0],
                                                        material.scalars[1],
                                                        material.scalars[2],
                                                        material.scalars[3],
                                                        int(material.switches[0]),
                                                        int(material.switches[1])};
        break;
    default:
        throw std::invalid_argument("Unsupported native material parameter layout");
    }
    packet.materialProgram = program;
    packet.materialParameters = storage;
    program->Configure(&packet);
    program->Prepare(&packet);
}
std::vector<std::uint32_t> MaterialLookupTextures(const resources::StaticModel &model)
{
    std::vector<std::uint32_t> result;
    for (const auto &packet : model.packets)
        if (packet.material.program == masked)
        {
            const float ramp = packet.material.scalars[3];
            const char *name = ramp < .2f      ? "global/white"
                               : ramp < .95f   ? "global/fresnel0"
                               : ramp < 1.666f ? "global/fresnel1"
                               : ramp < 3.25f  ? "global/fresnel2"
                                               : "global/fresnel4";
            const auto id = glGetTexture(name);
            if (std::find(result.begin(), result.end(), id) == result.end())
                result.push_back(id);
        }
    return result;
}
MaterialPreviewScope::MaterialPreviewScope(const nlMatrix4 &view, float time)
    : MaterialPreviewScope(view, time, GameLighting{})
{
}
MaterialPreviewScope::MaterialPreviewScope(const nlMatrix4 &view, float time, const GameLighting& lighting,
                                         const nlVector3* camera_position)
{
    if (preview_live || !std::isfinite(time) || time < 0 || time > 1e8f)
        throw std::invalid_argument("Invalid or nested material preview context");
    for (unsigned i = 0; i < 16; ++i)
        if (!std::isfinite(view.e[i])) throw std::invalid_argument("Non-finite material view matrix");
    if (camera_position && (!std::isfinite(camera_position->x) || !std::isfinite(camera_position->y)
        || !std::isfinite(camera_position->z)))
        throw std::invalid_argument("Non-finite material camera position");
    BeginGameLighting(lighting);
    preview_view = view;
    preview_time = time;
    preview_has_camera = camera_position != nullptr;
    if (camera_position) preview_camera = *camera_position;
    preview_live = true;
}
MaterialPreviewScope::~MaterialPreviewScope()
{
    EndGameLighting();
    preview_live = false;
    preview_has_camera = false;
}
void RequireMaterialPreview()
{
    if (!preview_live)
        throw std::logic_error("Material drawing requires an explicit view and lighting context");
}
const nlMatrix4 &MaterialPreviewView()
{
    RequireMaterialPreview();
    return preview_view;
}
float MaterialPreviewTime()
{
    RequireMaterialPreview();
    return preview_time;
}
const nlVector3& MaterialPreviewCameraPosition()
{
    RequireMaterialPreview();
    if (!preview_has_camera)
        throw std::logic_error("Camera overlay requires an explicit active camera position");
    return preview_camera;
}
void MaterialNormalMatrix(const nlMatrix4 &modelview, float output[3][4])
{
    nlMatrix4 inverse;
    nlInvertMatrix(inverse, modelview);
    for (unsigned row = 0; row < 3; ++row)
    {
        for (unsigned col = 0; col < 3; ++col)
            output[row][col] = inverse.e2[row][col];
        output[row][3] = 0;
    }
}
void DrawMaterial(const glModelPacket &packet, GLView* view)
{
    RequireMaterialPreview();
    auto *program = static_cast<GLMaterialProgram *>(packet.materialProgram);
    if (!program || !packet.materialParameters || packet.displayList
        || (!packet.indexBuffer && program->programHash != shadow_volume))
        throw std::runtime_error("Incomplete or unsupported native material packet");
    if (program->programHash == shadow_volume)
    {
        const auto& params = *static_cast<const GXShadowVolumeParameters*>(packet.materialParameters);
        if (packet.numStreams != 3 || !packet.streams || !packet.numUniqueVertices
            || (params.useFixedColour != 0 && params.useFixedColour != 1))
            throw std::invalid_argument("Invalid shadow volume packet");
        const unsigned strides[] = {12, 4, 8};
        for (unsigned i = 0; i < 3; ++i)
            if (!packet.streams[i].address || packet.streams[i].stride != strides[i])
                throw std::invalid_argument("Invalid shadow volume stream");
        if (packet.numVertices > 65535)
            throw std::invalid_argument("Shadow index count exceeds GX limits");
        if (packet.indexBuffer)
            for (unsigned i = 0; i < packet.numVertices; ++i)
                if (packet.indexBuffer[i] >= packet.numUniqueVertices)
                    throw std::out_of_range("Shadow index exceeds its vertex arrays");
    }
    Baseline();
    Raster(packet.rasterState);
    // Original glx_SwitchRaster always permits alpha-only writes. The view's
    // allow-alpha flag gates only combined colour/alpha writes (mode 3).
    if (view && !view->m_Enabled && glGetRasterState(packet.rasterState, GLS_ColourWrite) == 3)
        gxSetAlphaUpdate(false);
    nlMatrix4 world, modelview;
    glGetMatrix(packet.matrix, world);
    nlMultMatrices(modelview, world, preview_view);
    Mtx transform, normal;
    glxCopyMatrix(transform, modelview);
    GXLoadPosMtxImm(transform, GX_PNMTX0);
    if (program->programHash == scrolling || program->programHash == masked || program->programHash == detail_blend
        || program->programHash == scrolling_specular || program->programHash == camera_overlay)
    {
        MaterialNormalMatrix(modelview, normal);
        GXLoadNrmMtxImm(normal, GX_PNMTX0);
    }
    try
    {
        program->Activate(view);
        program->Draw(&packet);
    }
    catch (...)
    {
        program->Deactivate();
        throw;
    }
    program->Deactivate();
}
void MaterialConcatMatrices(const float left[3][4], const float right[3][4], float output[3][4])
{
    nlMatrix4 a, b, result;
    a.SetIdentity(); b.SetIdentity();
    for (unsigned row = 0; row < 3; ++row)
        for (unsigned column = 0; column < 4; ++column)
        {
            a.e2[column][row] = left[row][column];
            b.e2[column][row] = right[row][column];
        }
    nlMultMatrices(result, b, a);
    for (unsigned row = 0; row < 3; ++row)
        for (unsigned column = 0; column < 4; ++column)
            output[row][column] = result.e2[column][row];
}
} // namespace mscharged
