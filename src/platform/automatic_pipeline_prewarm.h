#pragma once

#include <dolphin/gx/GXEnum.h>

struct glModel;
struct glModelPacket;
class EffectsTemplate;
struct EffectsSpec;

namespace mscharged::platform {

// Synchronous owner-thread discovery. Original resources are borrowed only for
// this call; the graphics compiler receives copied pipeline configurations.
bool BeginAutomaticGraphicsCollection();
bool EndAutomaticGraphicsCollection(bool submit);
void BeginAutomaticMaterialVariant(const glModelPacket& packet);
void EmitAutomaticMaterialPipeline(const glModelPacket& packet);
// ShaderSkinMesh replaces position/normal streams with native float3 output.
void EmitAutomaticSkinnedMaterialPipeline(const glModelPacket& packet);
void EnumerateAutomaticMaterialPipelines(const glModelPacket& packet);
bool EnumerateAutomaticFixedMaterialPipelines(const glModelPacket& packet);
bool EnumerateAutomaticDynamicMaterialPipelines(const glModelPacket& packet);

// BindVertexArrays remains the authority for attribute slots and strides. In a
// preparation scope this records metadata only, without reading vertex bytes.
bool RecordAutomaticGraphicsArray(GXAttr attribute, const void* data, unsigned char stride);

// Source-owned GLX mirrors must be restored alongside the SDK capture state.
void SaveAutomaticGXMirrors();
void RestoreAutomaticGXMirrors();
void ConfigureAutomaticGXState(const glModelPacket& packet);

void PrepareAutomaticModelPipelines(const glModel* models, unsigned long count);
void PrepareAutomaticEffectPipelines();
void EnumerateAutomaticEffectResources(bool requireModels = false);
// The stadium owns the twelve score-glyph instances and their valid lifetime.
void EnumerateAutomaticNumberResources();
void EnumerateAutomaticNumberModelPipelines(const glModel& model);
void EnumerateAutomaticPlanarShadowResources();
void EnumerateAutomaticPlanarShadowModelPipelines(const glModel& model);
void EnumerateAutomaticParticlePipelines(const EffectsTemplate& effect,
                                        const EffectsSpec* spec, const glModel* model);
void InitializeAutomaticGraphicsPreparation();
bool PollAutomaticGraphicsPreparation(bool match);
bool AutomaticGraphicsPipelinesReady();
bool AutomaticGraphicsPreparationForMatch();
void SetAutomaticGraphicsPreparationForMatch(bool match);
void ResetAutomaticGraphicsPreparation();

} // namespace mscharged::platform
