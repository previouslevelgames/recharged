#include "platform/automatic_pipeline_prewarm.h"
#include "NL/gl/glMaterialProgram.h"
#include "NL/gl/glModel.h"

void glxPrepareBlackTextureAlphaPipeline(const glModelPacket& packet);
void glxPrepareCompactColourPipeline(const glModelPacket& packet);
void glxPrepareConstantColourPipeline(const glModelPacket& packet);
void glxPrepareFixedLightPipeline(const glModelPacket& packet);
void glxPrepareFloatTexturedColourPipeline(const glModelPacket& packet);
void glxPrepareFourTextureAddPipeline(const glModelPacket& packet);
void glxPrepareMaskedDiffuseBlendPipeline(const glModelPacket& packet);
void glxPrepareRedColourPipeline(const glModelPacket& packet);
void glxPrepareScissoredVertexColourTexturePipeline(const glModelPacket& packet);
void glxPrepareSkinnedUnlitTexturePipeline(const glModelPacket& packet);
void glxPrepareTextureBlendPipeline(const glModelPacket& packet);
void glxPrepareTextureColourAddPipeline(const glModelPacket& packet);
void glxPrepareThreeLightDiffusePipeline(const glModelPacket& packet);
void glxPrepareUnlitTexturePipeline(const glModelPacket& packet);
void glxPrepareVertexColourDetailBlendPipeline(const glModelPacket& packet);
void glxPrepareVertexColourPipeline(const glModelPacket& packet);
void glxPrepareVertexColourTexturePipeline(const glModelPacket& packet);

namespace mscharged::platform {
bool EnumerateAutomaticFixedMaterialPipelines(const glModelPacket& packet)
{
    const auto* program = static_cast<const GLMaterialProgram*>(packet.materialProgram);
    if (!program) return false;
    switch (program->programHash) {
    case 0xBACEA013: glxPrepareBlackTextureAlphaPipeline(packet); return true;
    case 0x4ED6C66F: glxPrepareCompactColourPipeline(packet); return true;
    case 0xEE9D919D: glxPrepareConstantColourPipeline(packet); return true;
    case 0xAD0DB7E9: glxPrepareFixedLightPipeline(packet); return true;
    case 0x19065BF6: glxPrepareFloatTexturedColourPipeline(packet); return true;
    case 0x2910966C: glxPrepareFourTextureAddPipeline(packet); return true;
    case 0x78D0AF4A: glxPrepareMaskedDiffuseBlendPipeline(packet); return true;
    case 0xDC56470F: glxPrepareRedColourPipeline(packet); return true;
    case 0x0027BCF6: glxPrepareScissoredVertexColourTexturePipeline(packet); return true;
    case 0x5D6C62BA: glxPrepareSkinnedUnlitTexturePipeline(packet); return true;
    case 0x46ABE398: glxPrepareTextureBlendPipeline(packet); return true;
    case 0x9557B266: glxPrepareTextureColourAddPipeline(packet); return true;
    case 0x8D359080: glxPrepareThreeLightDiffusePipeline(packet); return true;
    case 0x21DB4385: glxPrepareUnlitTexturePipeline(packet); return true;
    case 0x13DF86AD: glxPrepareVertexColourDetailBlendPipeline(packet); return true;
    case 0xD701656B: glxPrepareVertexColourPipeline(packet); return true;
    case 0xD3E572DA: glxPrepareVertexColourTexturePipeline(packet); return true;
    default: return false;
    }
}
} // namespace mscharged::platform
