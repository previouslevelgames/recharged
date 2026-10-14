#include "platform/automatic_pipeline_prewarm.h"
#include "platform/session_log.h"

#include <aurora/pipeline_cache.h>
#include "Game/GL/GLInventory.h"
#include "Game/GL/GLVertexAnim.h"
#include "Game/Effects/EffectsGroup.h"
#include "Game/Effects/EffectsTemplate.h"
#include "NL/gl/glMaterialProgram.h"
#include "NL/gl/glMemory.h"
#include "NL/gl/glModel.h"
#include "NL/gl/glState.h"
#include "NL/glx/GXConstantColourMaterialProgram.h"
#include "NL/glx/GXFloatTexturedColourMaterialProgram.h"
#include "NL/glx/GXCompactColourMaterialProgram.h"
#include "NL/glx/GXFourTextureAddMaterialProgram.h"
#include "NL/glx/GXMovieMaterialProgram.h"
#include "NL/glx/GXShadowedDiffuseMaterialProgram.h"
#include "NL/glx/GXShadowVolumeMaterialProgram.h"
#include "NL/glx/GXVertexColourTextureMaterialProgram.h"
#include "NL/nlDLRing.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <stdexcept>
#include <utility>
#include <vector>

// This file belongs only to the original frontend module. Resource traversal
// and descriptor construction run on the resource owner, synchronously. Aurora
// copies finished pipeline configurations; no compiler job owns game pointers.
#if !defined(MSCHARGED_NATIVE) || !defined(MSCHARGED_GAME_MODULE)
#error Automatic resource preparation belongs to the original game module
#endif

namespace mscharged::platform {
namespace {
std::atomic<unsigned> sPreparationPhase{0};
bool sInventorySubmitted;
bool sMaterialProgramsReady;
bool sPreparingMatch;
std::chrono::steady_clock::time_point sGateStarted;
bool sCollectingResourceIdentity;
std::vector<std::pair<std::uint32_t, std::uint64_t>> sScopeResources;

void RecordResourceIdentity(std::uint32_t kind, std::uint64_t value)
{
    if (sCollectingResourceIdentity) sScopeResources.emplace_back(kind, value);
}

std::uint64_t ResourceScopeIdentity()
{
    // Original resource hashes identify the selected content independently of
    // loading order, pointers, backend, and state left by a preceding material.
    // A collision can only prepare additional valid pipelines, never select a
    // wrong one for a draw (the renderer still checks the complete draw key).
    std::sort(sScopeResources.begin(), sScopeResources.end());
    sScopeResources.erase(std::unique(sScopeResources.begin(), sScopeResources.end()), sScopeResources.end());
    std::uint64_t hash = 14695981039346656037ull;
    const auto append = [&](std::uint64_t value) {
        for (unsigned byte = 0; byte != 8; ++byte) {
            hash ^= (value >> (byte * 8)) & 255;
            hash *= 1099511628211ull;
        }
    };
    append(0x4155544f47584631ull); // Resource identity schema 1.
    append(sPreparingMatch);
    for (const auto& [kind, value] : sScopeResources) {
        append(kind);
        append(value);
    }
    return hash;
}

class Collection {
    bool active = true;
public:
    Collection()
    {
        if (!BeginAutomaticGraphicsCollection())
            throw std::runtime_error("Could not isolate graphics preparation");
    }
    ~Collection() { if (active) EndAutomaticGraphicsCollection(false); }
    void Submit()
    {
        active = false;
        if (!EndAutomaticGraphicsCollection(true))
            throw std::runtime_error("Invalid graphics preparation descriptor");
    }
};

unsigned Raster(bool world)
{
    // The values selected by gl_StateStartup and glSetDefaultState. Use the
    // source's packed-state setter, on a local value, without changing GL state.
    unsigned result = 0;
    glSetRasterState(result, GLS_DepthTest, world);
    glSetRasterState(result, GLS_DepthWrite, world);
    glSetRasterState(result, GLS_DepthFunc, 1);
    glSetRasterState(result, GLS_Culling, 1);
    glSetRasterState(result, GLS_ColourWrite, 3);
    return result;
}

void EmitSprite(unsigned programHash, unsigned raster, int primitive,
                void* parameters, bool floatUV = false, bool colour = true)
{
    glModelStream streams[6] = {
        {nullptr, 0, 12, 1, 0},
        {nullptr, 1, static_cast<u8>(floatUV ? 8 : 4), 4, 0},
        {nullptr, 2, 4, 3, 0},
    };
    if (programHash == 0x4ED6C66F) streams[0].stride = 6;
    if (programHash == 0x2910966C) {
        for (unsigned i = 1; i != 5; ++i)
            streams[i] = {nullptr, static_cast<u8>(i), 8, 4, 0};
        streams[5] = {nullptr, 5, 4, 3, 0};
    }
    if (programHash == 0x386ECBDD) {
        // GLShadowBlendMeshWriter: position, colour, float texture coordinate.
        streams[1] = {nullptr, 1, 4, 3, 0};
        streams[2] = {nullptr, 2, 8, 4, 0};
    }
    if (programHash == 0xD701656B) {
        // GLColourMeshWriter: no texture coordinate stream or parameters.
        streams[1] = {nullptr, 1, 4, 3, 0};
        colour = false;
    }
    glModelPacket packet{};
    packet.primType = static_cast<char>(primitive);
    packet.numStreams = programHash == 0x2910966C ? 6 : colour ? 3 : 2;
    packet.streams = streams;
    packet.materialProgram = glGetMaterialProgram(programHash);
    packet.materialParameters = parameters;
    packet.rasterState = raster;
    if (packet.materialProgram == nullptr)
        throw std::runtime_error("Procedural graphics material is not registered");
    EnumerateAutomaticMaterialPipelines(packet);
}

void EmitTexturedSprite(unsigned raster, int primitive = 3)
{
    GXVertexColourTextureParameters parameters{};
    EmitSprite(0xD3E572DA, raster, primitive, &parameters);
}

void EmitConstantMesh(unsigned raster, int primitive)
{
    GXConstantColourParameters parameters{};
    parameters.constantColour = {1.0f, 1.0f, 1.0f, 1.0f};
    EmitSprite(0xEE9D919D, raster, primitive, &parameters, false, false);
}

void EnumeratePacket(const glModelPacket& packet)
{
    EnumerateAutomaticMaterialPipelines(packet);
    // AttachShadowVolumeModels replaces the authored raster at runtime.
    auto* material = static_cast<GLMaterialProgram*>(packet.materialProgram);
    if (material->programHash == 0x386ECBDD) {
        for (unsigned side = 0; side != 2; ++side) {
            glModelPacket volume = packet;
            volume.rasterState = Raster(false);
            glSetRasterState(volume.rasterState, GLS_Culling, side + 1);
            glSetRasterState(volume.rasterState, GLS_DepthTest, 1);
            glSetRasterState(volume.rasterState, GLS_AlphaBlend, side ? 7 : 2);
            glSetRasterState(volume.rasterState, GLS_ColourWrite, 2);
            EnumerateAutomaticMaterialPipelines(volume);
        }
    }
}

struct ModelVisitor {
    void Visit(const unsigned long& key, glModel** model)
    {
        if (model == nullptr || *model == nullptr) return;
        RecordResourceIdentity(1, (std::uint64_t(std::uint32_t(key)) << 32) | std::uint32_t((*model)->id));
        for (unsigned i = 0; i < (*model)->numPackets; ++i) {
            const auto* program = static_cast<const GLMaterialProgram*>((*model)->packets[i].materialProgram);
            if (program)
                RecordResourceIdentity(2, (std::uint64_t(std::uint32_t((*model)->id)) << 32) |
                    std::uint32_t(program->programHash));
            EnumeratePacket((*model)->packets[i]);
        }
    }
};

void EnumerateModels()
{
    GLResourcePool* pools = glGetResourcePools();
    if (pools == nullptr) return;
    ModelVisitor visitor;
    GLResourcePool* pool = nlDLRingGetStart(pools);
    for (;; pool = pool->m_next) {
        GLInventory* inventory = pool->m_inventory;
        if (inventory != nullptr && inventory->m_bCreated) {
            if (inventory->m_nLevel < 0 || inventory->m_nLevel >= 16)
                throw std::runtime_error("Invalid graphics resource level");
            for (int level = 0; level <= inventory->m_nLevel; ++level)
                if (inventory->m_pModels[level] != nullptr)
                    inventory->m_pModels[level]->Walk(&visitor, &ModelVisitor::Visit);
        }
        if (nlDLRingIsEnd(pools, pool)) break;
    }
}

void EnumerateCommonProceduralPipelines()
{
    // Source-generated UI/HUD, debug text, screen fades and PhotoFlash use
    // these colour/texture writers; no scene, animation or frame is advanced.
    for (unsigned blend = 0; blend != 8; ++blend) {
        unsigned raster = Raster(false);
        glSetRasterState(raster, GLS_AlphaBlend, blend);
        for (unsigned cull = 0; cull != 2; ++cull) {
            glSetRasterState(raster, GLS_Culling, cull);
            for (int primitive : {0, 1, 3}) {
                EmitTexturedSprite(raster, primitive);
                EmitSprite(0xD701656B, raster, primitive, nullptr);
                GXFloatTexturedColourParameters floating{};
                EmitSprite(0x19065BF6, raster, primitive, &floating, true);
            }
        }
    }

    // glFontBegin selects an alpha-tested ordinary textured quad.
    unsigned font = Raster(false);
    glSetRasterState(font, GLS_AlphaTest, 1);
    EmitTexturedSprite(font);

    // FERender::RenderImageInstance applies authored blend mode, alpha > 0,
    // and no culling to ordinary images. Grab/movie images keep alpha testing
    // disabled and are covered by their own descriptors.
    for (unsigned blend = 0; blend != 8; ++blend) {
        unsigned image = Raster(false);
        glSetRasterState(image, GLS_Culling, 0);
        glSetRasterState(image, GLS_AlphaBlend, blend);
        glSetRasterState(image, GLS_AlphaTest, 1);
        EmitTexturedSprite(image);
    }

    // RenderLightOnField also runs for frontend character effects.
    unsigned light = Raster(true);
    glSetRasterState(light, GLS_AlphaBlend, 3);
    glSetRasterState(light, GLS_DepthWrite, 0);
    EmitTexturedSprite(light);

    // ImpostorManager's source selects normal and additive sprite passes.
    // The normal path may retain depth-write=0 after an additive character.
    unsigned impostor = Raster(true);
    glSetRasterState(impostor, GLS_Culling, 0);
    glSetRasterState(impostor, GLS_AlphaTest, 1);
    glSetRasterState(impostor, GLS_AlphaTestRef, 0x80);
    GXCompactColourParameters compact{};
    for (unsigned depthWrite = 0; depthWrite != 2; ++depthWrite) {
        glSetRasterState(impostor, GLS_DepthWrite, depthWrite);
        glSetRasterState(impostor, GLS_AlphaBlend, 1);
        EmitSprite(0x4ED6C66F, impostor, 3, &compact);
        // ImpostorCluster draws a float-position background quad immediately
        // after the manager, retaining its exact raster state.
        EmitTexturedSprite(impostor);
    }
    glSetRasterState(impostor, GLS_DepthWrite, 0);
    glSetRasterState(impostor, GLS_AlphaBlend, 2);
    EmitSprite(0x4ED6C66F, impostor, 3, &compact);
    EmitTexturedSprite(impostor);

    // RenderImageQuad uses the movie writer for YUV frames and ordinary image
    // textures. The material producer describes both without binding either.
    GXMovieParameters movie{};
    movie.tint = {1.0f, 1.0f, 1.0f, 1.0f};
    for (unsigned blend : {0u, 1u}) {
        unsigned image = Raster(false);
        glSetRasterState(image, GLS_AlphaBlend, blend);
        EmitSprite(0xEC35CAAB, image, 3, &movie, true, false);
        // FERender sets no culling before invoking RenderImageQuad.
        glSetRasterState(image, GLS_Culling, 0);
        EmitSprite(0xEC35CAAB, image, 3, &movie, true, false);
    }

    // RenderHighRangePass writes its alpha-only reduction pass.
    GXFourTextureAddParameters range{};
    unsigned highRange = Raster(false);
    glSetRasterState(highRange, GLS_AlphaBlend, 6);
    glSetRasterState(highRange, GLS_ColourWrite, 2);
    EmitSprite(0x2910966C, highRange, 3, &range);

    if (!sPreparingMatch) return;

    // ElectricFence.cpp: DrawPrimitive and RenderElectricFenceFlat.
    unsigned fence = Raster(true);
    glSetRasterState(fence, GLS_Culling, 0);
    glSetRasterState(fence, GLS_DepthWrite, 0);
    glSetRasterState(fence, GLS_AlphaBlend, 2);
    for (int primitive : {0, 1, 2, 3}) {
        EmitConstantMesh(fence, primitive);
        EmitTexturedSprite(fence, primitive);
    }

    // DrawableNetMesh.cpp: dynamic net and invisible goal planes.
    unsigned net = Raster(true);
    glSetRasterState(net, GLS_Culling, 0);
    glSetRasterState(net, GLS_AlphaBlend, 1);
    EmitTexturedSprite(net);
    glSetRasterState(net, GLS_AlphaTest, 1);
    EmitConstantMesh(net, 1);

    // RenderShadow's projected character quad (and powerup shadow quads).
    unsigned projectedShadow = Raster(true);
    glSetRasterState(projectedShadow, GLS_AlphaBlend, 1);
    glSetRasterState(projectedShadow, GLS_Culling, 0);
    glSetRasterState(projectedShadow, GLS_DepthWrite, 0);
    EmitTexturedSprite(projectedShadow);

    // Ball shadow/glow, FlareHandler and objectblur use these exact tested,
    // non-depth-writing world-quad blends.
    glSetRasterState(projectedShadow, GLS_AlphaTest, 1);
    for (unsigned blend : {1u, 2u, 3u}) {
        glSetRasterState(projectedShadow, GLS_AlphaBlend, blend);
        EmitTexturedSprite(projectedShadow);
    }

    // Indicators::DrawIndicator uses an alpha-tested HUD poly.
    for (unsigned blend : {1u, 2u}) {
        unsigned indicator = Raster(false);
        glSetRasterState(indicator, GLS_AlphaBlend, blend);
        glSetRasterState(indicator, GLS_AlphaTest, 1);
        EmitTexturedSprite(indicator);
    }

    // DepthOfFieldManager::Render retains the screen-space depth-write=0
    // state, enables depth testing, and blends only when intensity < 1.
    for (unsigned blend : {0u, 1u}) {
        unsigned depthOfField = Raster(false);
        glSetRasterState(depthOfField, GLS_DepthTest, 1);
        glSetRasterState(depthOfField, GLS_AlphaBlend, blend);
        EmitTexturedSprite(depthOfField);
    }

    // ShadowVolume.cpp: screen composition; volume geometry comes from loaded
    // packets with its additive/subtractive raster variants below.
    unsigned shadow = Raster(false);
    glSetRasterState(shadow, GLS_AlphaBlend, 1);
    glSetRasterState(shadow, GLS_Culling, 0);
    GXShadowVolumeParameters shadowParameters{};
    shadowParameters.useFixedColour = 0;
    EmitSprite(0x386ECBDD, shadow, 1, &shadowParameters);

    // RenderWarbleQuad: same vertex layout, one diffuse binding. The producer
    // enumerates the material's indirect-stage configuration without touching
    // gWarbleEnabled or creating the render-target texture.
    glTextureBinding warbleParameters{};
    EmitSprite(0xCFB7215C, Raster(false), 3, &warbleParameters);
}
} // namespace

void EnumerateAutomaticPlanarShadowModelPipelines(const glModel& model)
{
    RecordResourceIdentity(6, std::uint32_t(model.id));
    // DrawPlanarShadow duplicates these actual stadium drawable packets and
    // changes only blend/depth comparison. Geometry, streams and all other
    // authored raster fields remain unchanged; transforms/textures are uniforms.
    for (unsigned i = 0; i != model.numPackets; ++i) {
        glModelPacket packet = model.packets[i];
        glSetRasterState(packet.rasterState, GLS_AlphaBlend, 1);
        glSetRasterState(packet.rasterState, GLS_DepthFunc, 3);
        EnumerateAutomaticMaterialPipelines(packet);
    }
}

void EnumerateAutomaticNumberModelPipelines(const glModel& model)
{
    RecordResourceIdentity(5, std::uint32_t(model.id));
    // NumberDisplay::RenderGlyph duplicates these specific authored packets,
    // then changes only these raster fields. Position/UV storage and authored
    // depth-write remain unchanged. Opacity is a uniform, not a pipeline key.
    for (unsigned i = 0; i != model.numPackets; ++i) {
        glModelPacket packet = model.packets[i];
        glSetRasterState(packet.rasterState, GLS_Culling, 0);
        glSetRasterState(packet.rasterState, GLS_AlphaBlend, 1);
        glSetRasterState(packet.rasterState, GLS_AlphaTest, 1);
        glSetRasterState(packet.rasterState, GLS_AlphaTestRef, 3);
        EnumerateAutomaticMaterialPipelines(packet);
    }
}

void EnumerateAutomaticParticlePipelines(const EffectsTemplate& effect,
                                        const EffectsSpec* spec,
                                        const glModel* model)
{
    RecordResourceIdentity(3, (std::uint64_t(effect.m_uHashID) << 32) | effect.m_uModelID);
    RecordResourceIdentity(4, (std::uint64_t(effect.m_uHashID) << 32) |
        (std::uint32_t(effect.m_eBlend) << 16) | (std::uint32_t(effect.m_eBillboard) << 8) | effect.m_uFlags);
    if (spec != nullptr && spec->m_bLight) return; // common light-blob producer
    const unsigned blend = effect.m_eBlend == EfBlend_Normal ? 1 : 3;
    if (model != nullptr) {
        // ParticleSystem's model branch copies the packet, then overrides
        // these raster fields. No animation tick or geometry copy is needed.
        for (unsigned i = 0; i != model->numPackets; ++i) {
            glModelPacket packet = model->packets[i];
            glSetRasterState(packet.rasterState, GLS_Culling, 0);
            glSetRasterState(packet.rasterState, GLS_AlphaBlend, blend);
            glSetRasterState(packet.rasterState, GLS_AlphaTest, 1);
            glSetRasterState(packet.rasterState, GLS_AlphaTestRef, 3);
            if (effect.DisablesDepthWrite())
                glSetRasterState(packet.rasterState, GLS_DepthWrite, 0);
            EnumerateAutomaticMaterialPipelines(packet);
        }
        return;
    }
    if (effect.m_uModelID != 0xFFFFFFFFu)
        throw std::runtime_error("Effect graphics model is absent during preparation");

    unsigned raster = Raster(true);
    glSetRasterState(raster, GLS_DepthWrite, 0);
    glSetRasterState(raster, GLS_Culling,
                    effect.m_eBillboard == EfBill_SoftwareControlled ? 0 : 1);
    glSetRasterState(raster, GLS_AlphaBlend, blend);
    glSetRasterState(raster, GLS_AlphaTest, 1);
    // m_AllowInFront is a live controller property: both branches can be used
    // with an authored front-facing effect without constructing a controller.
    const bool inFront = effect.IsInFront() || (spec && spec->m_bInFront);
    for (unsigned front = 0; front <= unsigned(inFront); ++front) {
        glSetRasterState(raster, GLS_DepthTest, front ? 0 : 1);
        GXVertexColourTextureParameters ordinary{};
        ordinary.diffuseTexture.texture = effect.m_hTexture;
        EmitSprite(0xD3E572DA, raster, 3, &ordinary);
        GXShadowedDiffuseParameters shadowed{};
        shadowed.diffuseTexture = ordinary.diffuseTexture;
        shadowed.receiveShadows = effect.m_eBlend == EfBlend_Normal;
        EmitSprite(0x257C2FF3, raster, 3, &shadowed);
    }
}

void PrepareAutomaticModelPipelines(const glModel* models, unsigned long count)
{
    if (!sMaterialProgramsReady || count == 0) return;
    if (models == nullptr) throw std::runtime_error("Missing prepared graphics models");
    Collection collection;
    for (unsigned long m = 0; m != count; ++m)
        for (unsigned p = 0; p != models[m].numPackets; ++p) {
            EnumeratePacket(models[m].packets[p]);
        }
    collection.Submit();
}

void PrepareAutomaticEffectPipelines()
{
    if (!sMaterialProgramsReady) return;
    Collection collection;
    EnumerateAutomaticEffectResources(false);
    collection.Submit();
}

void InitializeAutomaticGraphicsPreparation()
{
    sMaterialProgramsReady = true;
    Collection collection;
    EnumerateCommonProceduralPipelines();
    collection.Submit();
}

void ResetAutomaticGraphicsPreparation()
{
    sInventorySubmitted = false;
    sPreparingMatch = false;
    sPreparationPhase.store(0, std::memory_order_release);
    if (sMaterialProgramsReady) {
        if (!aurora_begin_pipeline_generation())
            throw std::runtime_error("Could not begin graphics resource generation");
        Collection collection;
        EnumerateCommonProceduralPipelines();
        collection.Submit();
    }
}

void SetAutomaticGraphicsPreparationForMatch(bool match)
{
    const bool changed = sPreparingMatch != match;
    sPreparingMatch = match;
    if (changed && sMaterialProgramsReady) {
        Collection collection;
        EnumerateCommonProceduralPipelines();
        collection.Submit();
    }
}

bool AutomaticGraphicsPreparationForMatch() { return sPreparingMatch; }

bool PollAutomaticGraphicsPreparation(bool match)
{
    SetAutomaticGraphicsPreparationForMatch(match);
    sPreparationPhase.store(match ? 2 : 1, std::memory_order_release);
    if (!sMaterialProgramsReady)
        throw std::runtime_error("Graphics preparation preceded material initialization");
    if (!sInventorySubmitted) {
        sGateStarted = std::chrono::steady_clock::now();
        if (!aurora_begin_pipeline_scope_census())
            throw std::runtime_error("Could not begin graphics resource census");
        sScopeResources.clear();
        struct IdentityScope {
            IdentityScope() { sCollectingResourceIdentity = true; }
            ~IdentityScope() { sCollectingResourceIdentity = false; }
        } identityScope;
        Collection collection;
        EnumerateCommonProceduralPipelines();
        EnumerateModels();
        if (match) {
            EnumerateAutomaticNumberResources();
            EnumerateAutomaticPlanarShadowResources();
        }
        EnumerateAutomaticEffectResources(true);
        collection.Submit();
        const auto scope = ResourceScopeIdentity();
        if (!aurora_seal_pipeline_generation_for_resources(scope))
            throw std::runtime_error("Could not prepare learned graphics variants");
        sInventorySubmitted = true;
        const auto status = aurora_pipeline_warmup_status();
        // A libc++ DLL can allocate a grown std::string outside the game's
        // allocator. Pass borrowed stack text; the host owns all log strings.
        char message[384];
        std::snprintf(message, sizeof message,
            "Graphics preparation: %s resources discovered (scope %llu), %llu configurations, %llu/%llu pipelines ready",
            match ? "match" : "frontend", static_cast<unsigned long long>(scope),
            static_cast<unsigned long long>(status.knownConfigs),
            static_cast<unsigned long long>(status.readyPipelines),
            static_cast<unsigned long long>(status.totalPipelines));
        SessionLogText(message);
    }
    if (!AutomaticGraphicsPipelinesReady()) return false;
    const auto status = aurora_pipeline_warmup_status();
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - sGateStarted).count();
    char message[384];
    std::snprintf(message, sizeof message,
        "Graphics preparation: %s ready after %lld ms at the loading gate; %llu pipelines, %llu demand hits, %llu demand misses",
        match ? "match" : "frontend", static_cast<long long>(elapsed),
        static_cast<unsigned long long>(status.readyPipelines),
        static_cast<unsigned long long>(status.demandHits),
        static_cast<unsigned long long>(status.demandMisses));
    SessionLogText(message);
    sPreparationPhase.store(0, std::memory_order_release);
    return true;
}

bool AutomaticGraphicsPipelinesReady()
{
    const auto status = aurora_pipeline_warmup_status();
    if (status.failedPipelines != 0)
        throw std::runtime_error("Automatic graphics preparation failed");
    return status.ready;
}
} // namespace mscharged::platform

extern "C" __attribute__((visibility("default")))
bool charged_graphics_preparation_pending()
{
    return mscharged::platform::sPreparationPhase.load(std::memory_order_acquire) != 0;
}

extern "C" __attribute__((visibility("default")))
const char* charged_graphics_preparation_label()
{
    return mscharged::platform::sPreparationPhase.load(std::memory_order_acquire) == 2
        ? "Preparing match" : "Preparing graphics";
}
