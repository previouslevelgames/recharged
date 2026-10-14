#include "platform/automatic_pipeline_prewarm.h"
#include "NL/gl/glModel.h"
#include "NL/glx/GXDetailModulateMaterialProgram.h"
#include "NL/glx/GXMaskedDetailBlendMaterialProgram.h"
#include "NL/glx/GXMaskedSpecularFresnelMaterialProgram.h"
#include "NL/glx/GXScrollingDiffuseMaterialProgram.h"
#include "NL/glx/GXScrollingMaskedDetailBlendMaterialProgram.h"
#include "NL/glx/GXScrollingShadowedDetailBlendMaterialProgram.h"
#include "NL/glx/GXScrollingSpecularMaterialProgram.h"
#include "NL/glx/GXShadowedDetailBlendMaterialProgram.h"
#include "NL/glx/GXSpecularDetailBlendMaterialProgram.h"
#include "NL/glx/GXMegaDiffuseMaterialProgram.h"
#include "NL/glx/GXMegaSpecularMaterialProgram.h"
#include "NL/glx/GXMegaSpecularFresnelMaterialProgram.h"
#include "NL/glx/GXSpecularMaterialProgram.h"
#include "NL/glx/GXSpecularFresnelMaterialProgram.h"
#include "NL/glx/GXCharacterSkinCustomMaterialProgram.h"
#include "NL/glx/GXCameraScrolledOverlayMaterialProgram.h"
#include "NL/glx/GXScrollingCameraOverlayMaterialProgram.h"
#include "NL/glx/GXShadowedDiffuseMaterialProgram.h"
#include "NL/glx/GXCrystalMaterialProgram.h"
#include "NL/glx/GXSpecularLookupMaterialProgram.h"
#include "NL/glx/GXShadowVolumeMaterialProgram.h"
#include "NL/glx/GXMaterialProgram.h"
#include "NL/glx/GXMovieMaterialProgram.h"
#include "NL/glx/GXCharacterDamageMaterialProgram.h"
#include "NL/glx/GXColourFresnelMaterialProgram.h"
#include "NL/glx/GXSkinnedMultiLightMaterialProgram.h"

void glxCollectDetailModulatePipelineVariants(const glModelPacket& packet);
void glxCollectMaskedDetailBlendPipelineVariants(const glModelPacket& packet);
void glxCollectMaskedSpecularFresnelPipelineVariants(const glModelPacket& packet);
void glxCollectScrollingDiffusePipelineVariants(const glModelPacket& packet);
void glxCollectScrollingMaskedDetailBlendPipelineVariants(const glModelPacket& packet);
void glxCollectScrollingShadowedDetailBlendPipelineVariants(const glModelPacket& packet);
void glxCollectScrollingSpecularPipelineVariants(const glModelPacket& packet);
void glxCollectShadowedDetailBlendPipelineVariants(const glModelPacket& packet);
void glxCollectSpecularDetailBlendPipelineVariants(const glModelPacket& packet);
void glxCollectMegaDiffusePipelineVariants(const glModelPacket& packet);
void glxCollectMegaSpecularPipelineVariants(const glModelPacket& packet);
void glxCollectMegaSpecularFresnelPipelineVariants(const glModelPacket& packet);
void glxCollectSpecularPipelineVariants(const glModelPacket& packet);
void glxCollectSpecularFresnelPipelineVariants(const glModelPacket& packet);
void glxCollectCharacterSkinCustomPipelineVariants(const glModelPacket& packet);
void glxCollectCameraScrolledOverlayPipelineVariants(const glModelPacket& packet);
void glxCollectScrollingCameraOverlayPipelineVariants(const glModelPacket& packet);
void glxCollectShadowedDiffusePipelineVariants(const glModelPacket& packet);
void glxCollectCrystalPipelineVariants(const glModelPacket& packet);
void glxCollectSpecularLookupPipelineVariants(const glModelPacket& packet);
void glxCollectShadowVolumePipelineVariants(const glModelPacket& packet);
void glxCollectWarblePipelineVariants(const glModelPacket& packet);
void glxCollectMoviePipelineVariants(const glModelPacket& packet);
void glxCollectCharacterDamagePipelineVariants(const glModelPacket& packet);
void glxCollectColourFresnelPipelineVariants(const glModelPacket& packet);
void glxCollectSkinnedMultiLightPipelineVariants(const glModelPacket& packet);

namespace mscharged::platform {
bool EnumerateAutomaticDynamicMaterialPipelines(const glModelPacket& packet)
{
    if (packet.materialProgram == GXDetailModulateMaterialProgram::Instance)
    {
        glxCollectDetailModulatePipelineVariants(packet);
        return true;
    }
    if (packet.materialProgram == GXMaskedDetailBlendMaterialProgram::Instance)
    {
        glxCollectMaskedDetailBlendPipelineVariants(packet);
        return true;
    }
    if (packet.materialProgram == GXMaskedSpecularFresnelMaterialProgram::Instance)
    {
        glxCollectMaskedSpecularFresnelPipelineVariants(packet);
        return true;
    }
    if (packet.materialProgram == GXScrollingDiffuseMaterialProgram::Instance)
    {
        glxCollectScrollingDiffusePipelineVariants(packet);
        return true;
    }
    if (packet.materialProgram == GXScrollingMaskedDetailBlendMaterialProgram::Instance)
    {
        glxCollectScrollingMaskedDetailBlendPipelineVariants(packet);
        return true;
    }
    if (packet.materialProgram == GXScrollingShadowedDetailBlendMaterialProgram::Instance)
    {
        glxCollectScrollingShadowedDetailBlendPipelineVariants(packet);
        return true;
    }
    if (packet.materialProgram == GXScrollingSpecularMaterialProgram::Instance)
    {
        glxCollectScrollingSpecularPipelineVariants(packet);
        return true;
    }
    if (packet.materialProgram == GXShadowedDetailBlendMaterialProgram::Instance)
    {
        glxCollectShadowedDetailBlendPipelineVariants(packet);
        return true;
    }
    if (packet.materialProgram == GXSpecularDetailBlendMaterialProgram::Instance)
    {
        glxCollectSpecularDetailBlendPipelineVariants(packet);
        return true;
    }
    if (packet.materialProgram == GXMegaDiffuseMaterialProgram::Instance)
    {
        glxCollectMegaDiffusePipelineVariants(packet);
        return true;
    }
    if (packet.materialProgram == GXMegaSpecularMaterialProgram::Instance)
    {
        glxCollectMegaSpecularPipelineVariants(packet);
        return true;
    }
    if (packet.materialProgram == GXMegaSpecularFresnelMaterialProgram::Instance)
    {
        glxCollectMegaSpecularFresnelPipelineVariants(packet);
        return true;
    }
    if (packet.materialProgram == GXSpecularMaterialProgram::Instance)
    {
        glxCollectSpecularPipelineVariants(packet);
        return true;
    }
    if (packet.materialProgram == GXSpecularFresnelMaterialProgram::Instance)
    {
        glxCollectSpecularFresnelPipelineVariants(packet);
        return true;
    }
    if (packet.materialProgram == GXCharacterSkinCustomMaterialProgram::Instance)
    {
        glxCollectCharacterSkinCustomPipelineVariants(packet);
        return true;
    }
    if (packet.materialProgram == GXCameraScrolledOverlayMaterialProgram::Instance)
    {
        glxCollectCameraScrolledOverlayPipelineVariants(packet);
        return true;
    }
    if (packet.materialProgram == GXScrollingCameraOverlayMaterialProgram::Instance)
    {
        glxCollectScrollingCameraOverlayPipelineVariants(packet);
        return true;
    }
    if (packet.materialProgram == GXShadowedDiffuseMaterialProgram::Instance)
    {
        glxCollectShadowedDiffusePipelineVariants(packet);
        return true;
    }
    if (packet.materialProgram == GXCrystalMaterialProgram::Instance)
    {
        glxCollectCrystalPipelineVariants(packet);
        return true;
    }
    if (packet.materialProgram == GXSpecularLookupMaterialProgram::Instance)
    {
        glxCollectSpecularLookupPipelineVariants(packet);
        return true;
    }
    if (packet.materialProgram == GXShadowVolumeMaterialProgram::Instance)
    {
        glxCollectShadowVolumePipelineVariants(packet);
        return true;
    }
    if (packet.materialProgram == GXWarbleMaterialProgram::Instance)
    {
        glxCollectWarblePipelineVariants(packet);
        return true;
    }
    if (packet.materialProgram == GXMovieMaterialProgram::Instance)
    {
        glxCollectMoviePipelineVariants(packet);
        return true;
    }
    if (packet.materialProgram == GXCharacterDamageMaterialProgram::Instance)
    {
        glxCollectCharacterDamagePipelineVariants(packet);
        return true;
    }
    if (packet.materialProgram == GXColourFresnelMaterialProgram::Instance)
    {
        glxCollectColourFresnelPipelineVariants(packet);
        return true;
    }
    if (packet.materialProgram == GXSkinnedMultiLightMaterialProgram::Instance)
    {
        glxCollectSkinnedMultiLightPipelineVariants(packet);
        return true;
    }
    return false;
}
} // namespace mscharged::platform
