#include "platform/automatic_pipeline_prewarm.h"
#include "platform/game_allocation_ownership.h"

#include "NL/gl/glMaterialProgram.h"
#include "NL/gl/glModel.h"
#include "NL/glx/GXMaterialProgramInternal.h"
#include "NL/glx/glxGX.h"
#include "NL/glx/glxSend.h"

#include <aurora/pipeline_cache.h>
#include <dolphin/gx.h>

#include <cstdio>
#include <stdexcept>

namespace mscharged::platform {
namespace {
bool sCollectionOpen;
struct ArrayLayout {
    unsigned char stride = 0;
    bool littleEndian = false;
    bool bound = false;
};
ArrayLayout sPositionLayout;
ArrayLayout sNormalLayout;
}

bool BeginAutomaticGraphicsCollection()
{
    if (sCollectionOpen || !aurora_begin_pipeline_collection()) return false;
    SaveAutomaticGXMirrors();
    sCollectionOpen = true;
    return true;
}

bool EndAutomaticGraphicsCollection(bool submit)
{
    if (!sCollectionOpen) return false;
    RestoreAutomaticGXMirrors();
    sCollectionOpen = false;
    return aurora_end_pipeline_collection(submit);
}

void BeginAutomaticMaterialVariant(const glModelPacket& packet)
{
    if (!sCollectionOpen || !aurora_reset_pipeline_collection())
        throw std::runtime_error("Cannot reset graphics preparation state");
    RestoreAutomaticGXMirrors();
    ConfigureAutomaticGXState(packet);
    sPositionLayout = {};
    sNormalLayout = {};
}

bool RecordAutomaticGraphicsArray(GXAttr attribute, const void* data, unsigned char stride)
{
    if (!sCollectionOpen) return false;
    // Authored streams keep their verified byte domain. Procedural descriptors
    // have no vertex storage; the source creates their streams in native order.
    const bool littleEndian = data ? ResolveGameGraphicsArray(data).little_endian : true;
    if (attribute == GX_VA_POS) sPositionLayout = {stride, littleEndian, true};
    if (attribute == GX_VA_NRM) sNormalLayout = {stride, littleEndian, true};
    GXSetArray(attribute, nullptr, 0, stride, littleEndian);
    return true;
}

void EmitAutomaticMaterialPipeline(const glModelPacket& packet)
{
    if (!sCollectionOpen || static_cast<unsigned>(packet.primType) >= 6)
        throw std::runtime_error("Invalid material pipeline preparation descriptor");
    const auto primitive = glxGetPrimitiveType(static_cast<unsigned char>(packet.primType));
    const auto emit = [&](bool coPlanar) {
        if (!aurora_collect_pipeline(static_cast<unsigned>(primitive), GX_VTXFMT0, coPlanar))
            throw std::runtime_error("Unsupported graphics state during preparation");
    };
    const auto emitView = [&] {
        emit(false);
        // RLView enables co-planar handling only for indexed source packets.
        if (packet.indexBuffer) emit(true);
    };
    const auto emitFog = [&] {
        glx_Fog(false);
        emitView();
        glx_Fog(true);
        emitView();
    };
    emitFog();
    // Views can suppress EFB alpha writes. Preserve the family/pass colour and
    // depth settings; generate only the corresponding alpha-write variation.
    const bool alpha = gxSetAlphaUpdate(false);
    if (alpha) emitFog();
    gxSetAlphaUpdate(alpha);
}

void EmitAutomaticSkinnedMaterialPipeline(const glModelPacket& packet)
{
    EmitAutomaticMaterialPipeline(packet);
    if ((!sPositionLayout.bound || sPositionLayout.littleEndian) &&
        (!sNormalLayout.bound || sNormalLayout.littleEndian)) return;

    // SoftwareSkinModel allocates native nlVector3 positions and, when present,
    // normals; BindVertexArrays still owns the slot and stride description.
    // UV and colour streams retain their authored byte order. No vertices are
    // read or fabricated by either of these metadata-only captures.
    if (sPositionLayout.bound)
        GXSetArray(GX_VA_POS, nullptr, 0, sPositionLayout.stride, true);
    if (sNormalLayout.bound)
        GXSetArray(GX_VA_NRM, nullptr, 0, sNormalLayout.stride, true);
    EmitAutomaticMaterialPipeline(packet);

    // A producer may emit several opacity/depth passes without beginning a
    // new material variant. Restore authored metadata before its next pass.
    if (sPositionLayout.bound)
        GXSetArray(GX_VA_POS, nullptr, 0, sPositionLayout.stride, sPositionLayout.littleEndian);
    if (sNormalLayout.bound)
        GXSetArray(GX_VA_NRM, nullptr, 0, sNormalLayout.stride, sNormalLayout.littleEndian);
}

void EnumerateAutomaticMaterialPipelines(const glModelPacket& packet)
{
    if (!sCollectionOpen)
        throw std::runtime_error("Material discovery requires an isolated graphics collection");
    if (EnumerateAutomaticFixedMaterialPipelines(packet) ||
        EnumerateAutomaticDynamicMaterialPipelines(packet)) return;
    const auto* program = static_cast<const GLMaterialProgram*>(packet.materialProgram);
    char message[96];
    std::snprintf(message, sizeof(message), "Uncovered graphics material 0x%08lx",
                  program ? static_cast<unsigned long>(program->programHash) : 0ul);
    throw std::runtime_error(message);
}

} // namespace mscharged::platform
