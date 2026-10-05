// Independent synthetic pixel expectations for the original material TEV recipes.
#include "runtime/materials.h"
#include "runtime/views.h"
#include "runtime/gpu_readback.h"
#include "runtime/material_environment.h"
#include "runtime/static_inventory.h"
#include "Game/GL/GLInventory.h"
#include "Game/GL/GLTextureAnim.h"
#include "runtime/graphics_memory.h"
#include "runtime/graphics_state.h"
#include "runtime/startup.h"
#include "runtime/startup_files.h"
#include "Game/GraphicsMemoryStartup.h"
#include "NL/gl/glMemoryInit.h"
#include "NL/gl/glMatrix.h"
#include "NL/gl/glState.h"
#include "NL/glx/glxTarget.h"
#include "NL/glx/GXMaskedSpecularFresnelMaterialProgram.h"
#include "NL/glx/GXScrollingDiffuseMaterialProgram.h"
#include "NL/glx/GXSpecularDetailBlendMaterialProgram.h"
#include "NL/glx/GXScrollingSpecularMaterialProgram.h"
#include "NL/glx/GXCameraScrolledOverlayMaterialProgram.h"
#include "NL/glx/GXScrollingCameraOverlayMaterialProgram.h"
#include "NL/glx/GXMaskedDetailBlendMaterialProgram.h"
#include "NL/glx/GXScrollingMaskedDetailBlendMaterialProgram.h"
#include "Game/Render/LightingLookup.h"
#include "NL/nlMemory.h"
#include "NL/MemAlloc.h"
#include <aurora/aurora.h>
#include <aurora/event.h>
#include <aurora/gfx.h>
#include <dolphin/gx.h>
#include <dolphin/gx/GXAurora.h>
#include <dolphin/mtx.h>
#include <dolphin/vi.h>
#include <SDL3/SDL.h>
#include <imgui.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string_view>

extern bool gScrollingSpecularHighlightsEnabled;
extern bool gScrollingSpecularAnimationEnabled;
extern bool gScrollingSpecularShadowsEnabled;
extern bool gCameraOverlayShadowsEnabled;
extern bool gScrollingCameraOverlayShadowsEnabled;
extern bool gMaskedDetailBlendShadowsEnabled;
extern bool gScrollingMaskedDetailBlendShadowsEnabled;

using namespace mscharged;
namespace
{
std::atomic_uint errors = 0;
void Log(AuroraLogLevel level, const char *, const char *message, unsigned length)
{
    if (level >= LOG_ERROR)
        ++errors;
    std::cerr.write(message, length);
    std::cerr << '\n';
}
void Check(bool value, const char *message)
{
    if (!value)
        throw std::runtime_error(message);
}
void Drain()
{
    AuroraGXSync();
}
void Invalidate()
{
    GXInvalidateVtxCache();
    GXInvalidateTexAll();
}
struct Session
{
    bool live = false, gx = false;
    ~Session()
    {
        if (!live)
            return;
        if (gx)
            Drain();
        glShutdownMemory();
        ResetStartupFiles();
        ResetStartupMemory();
        aurora_shutdown();
    }
};
resources::Texture Texture(unsigned id, std::array<unsigned char, 4> c, unsigned alpha_bits = 0, bool stripe = false, bool vertical = false)
{
    resources::Texture t;
    t.id = id;
    t.width = t.height = 4;
    t.levels = 1;
    t.game_format = 3;
    t.gx_format = 6;
    t.bits = {8, 8, 8, static_cast<unsigned char>(alpha_bits)};
    t.pixels.resize(64);
    for (unsigned i = 0; i < 16; ++i)
    {
        const auto pixel = stripe && (vertical ? i / 4 : i % 4) >= 2 ? std::array<unsigned char, 4>{20, 40, 200, 255} : c;
        t.pixels[i * 2] = pixel[3];
        t.pixels[i * 2 + 1] = pixel[0];
        t.pixels[i * 2 + 32] = pixel[1];
        t.pixels[i * 2 + 33] = pixel[2];
    }
    return t;
}
resources::StaticModel Model(unsigned id, unsigned program, unsigned texture)
{
    resources::Packet p;
    p.primitive = 0;
    p.material.program = program;
    p.material.textures[0] = {texture, 3};
    p.raster = 0xC0007;
    p.vertices = {
        {{-.8f, -.8f, -.3f}, {.125f, .125f}}, {{.8f, -.8f, -.3f}, {.125f, .125f}}, {{0, .8f, -.3f}, {.125f, .125f}}};
    for (auto &v : p.vertices)
    {
        v.normal = {0, 0, 1};
        v.uv1 = v.uv2 = v.uv;
    }
    p.indices = {0, 1, 2};
    return {id, {p}};
}
void PixelCase(const char *name, glModel &model, float time, std::array<unsigned char, 3> expected,
               const GameLighting& lighting = {}, const nlMatrix4* world = nullptr, const nlMatrix4* view = nullptr,
               glModel* copy_source = nullptr, GLRenderPair copy_target = {}, unsigned copy_mode = 8, GLRenderPair after_clear = {}, const nlVector3* camera_position = nullptr)
{
    unsigned draws = 0;
    std::array<unsigned char, 3> pixel{};
    nlMatrix4 identity;
    identity.SetIdentity();
    ViewMatrices matrices;
    matrices.view = view ? *view : identity;
    glMatrixOrthographicCentered(matrices.projection, 2, 2, 0, 1);
    auto submitted = std::make_unique<GLView>(&matrices, GLRenderPair{}, GLViewSort_None);
    std::unique_ptr<GLView> producer, clear_probe;
    if (copy_source)
    {
        producer = std::make_unique<GLView>(&matrices, copy_target, GLViewSort_None);
        producer->m_Target = copy_mode;
        gRootView.AddChild(producer.get());
        if (after_clear)
        {
            clear_probe = std::make_unique<GLView>(&matrices, after_clear, GLViewSort_None);
            clear_probe->m_Target = GLViewTarget_Mode8;
            gRootView.AddChild(clear_probe.get());
        }
        submitted->m_ClearColour = submitted->m_ClearDepth = true;
    }
    gRootView.AddChild(submitted.get());
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    for (unsigned frame = 0; frame < 40;)
    {
        Check(std::chrono::steady_clock::now() < deadline, "Material pixel readback deadline exceeded");
        for (const auto *event = aurora_update(); event->type != AURORA_NONE; ++event)
            Check(event->type != AURORA_EXIT, "Material test window closed");
        if (!aurora_begin_frame())
        {
            SDL_Delay(1);
            continue;
        }
        glplatFrameAllocNextFrame();
        glModelSetMatrix(&model, world ? *world : identity);
        GXSetPixelFmt(copy_target && copy_target.target->mFormat == GLTargetFormat_A8 ? GX_PF_RGBA6_Z24 : GX_PF_RGB8_Z24,
                      GX_ZC_LINEAR);
        GXSetCopyClear({20, 24, 30, static_cast<u8>(after_clear ? 0 : 255)}, GX_MAX_Z24);
        if (producer)
        {
            glModelSetMatrix(copy_source, identity);
            producer->AttachModel(copy_source, 0);
        }
        submitted->AttachModel(&model, 0);
        try { RenderOriginalViews(time, lighting, camera_position); }
        catch (...) { aurora_end_frame(); throw; }
        GXDrawDone();
        if (frame == 39)
        {
            const auto samples = EndFrameAndReadColours();
            std::copy_n(samples[4].begin(), 3, pixel.begin());
        }
        else
            aurora_end_frame();
        draws += aurora_get_stats()->drawCallCount;
        ++frame;
    }
    const bool match = std::abs(int(pixel[0]) - expected[0]) <= 3 && std::abs(int(pixel[1]) - expected[1]) <= 3 &&
                       std::abs(int(pixel[2]) - expected[2]) <= 3;
    std::cout << name << ": RGB " << unsigned(pixel[0]) << ',' << unsigned(pixel[1]) << ',' << unsigned(pixel[2])
              << ", draws " << draws << '\n';
    Check(match && draws > 0, "Original material shader pixel mismatch");
}

void TextureAnimationCases()
{
    auto source=Model(1400,0xf2d57ac6,1450);
    auto& m=source.packets[0].material;m.textures[1]={1451,3};m.textures[2]={1452,3};m.scalars[0]=1;
    auto& pool=*glGetCurrentResourcePool();
    StaticInventory inventory(pool,{source},
        {Texture(1401,{80,100,120,255},8,true),Texture(1402,{200,40,20,128},8,true),
         Texture(1403,{160,180,200,255}),Texture(1406,{40,80,120,255}),
         Texture(1404,{255,255,255,255}),Texture(1405,{0,0,0,255})},Drain,
        {{1450,0,0,false,0,{{1401,.25f},{1402,.5f}}},
         {1451,2,0,false,0,{{1403,.5f},{1406,.5f}}},
         {1452,1,1,false,0,{{1404,.75f},{1405,.75f}}}});
    auto& model=*inventory.Model(1400);
    auto& p=*static_cast<GXScrollingMaskedDetailBlendParameters*>(model.packets[0].materialParameters);
    auto* diffuse=glGetTextureAnim(1450);auto* mask=glGetTextureAnim(1452);
    PixelCase("IFL initial diffuse frame",model,0,{80,100,120});
    pool.m_inventory->UpdateTextureAnims(.125f);
    PixelCase("IFL partial frame duration",model,0,{80,100,120});
    pool.m_inventory->UpdateTextureAnims(.125f);
    PixelCase("IFL diffuse alias refresh and alpha",model,0,{110,32,25});
    p.diffuseScrollSpeedX=.5f;
    PixelCase("IFL frame and UV scrolling are independent",model,1,{20,40,200});
    p.diffuseScrollSpeedX=0;diffuse->m_bPaused=1;diffuse->Update(10);
    PixelCase("IFL paused frame retains pixels",model,0,{110,32,25});
    diffuse->m_bPaused=0;pool.m_inventory->UpdateTextureAnims(10);
    PixelCase("IFL one-step loop drops overshoot",model,0,{80,100,120});
    p.blendAmount=0;
    PixelCase("IFL animated black mask retains diffuse",model,0,{80,100,120});
    mask->Update(.75f);
    PixelCase("IFL ping-pong mask reveals held detail",model,0,{40,80,120});
    p.blendAmount=.5f;
    PixelCase("IFL masked detail fractional blend",model,0,{60,90,120});
    // Resolve an existing cached animation binding to a static ID and back.
    p.diffuseTexture.texture=1406;p.blendAmount=1;
    PixelCase("IFL cached alias changed to static texture",model,0,{40,80,120});
    p.diffuseTexture.texture=1450;
    PixelCase("IFL cached binding returns to animation",model,0,{80,100,120});
    inventory.Release();
    StaticInventory restart(pool,{Model(1400,0x21db4385,1450)},
        {Texture(1401,{20,40,200,255})},Drain,{{1450,0,0,false,0,{{1401,0}}}});
    pool.m_inventory->UpdateTextureAnims(100);
    PixelCase("IFL single-frame alias after release and reuse",*restart.Model(1400),0,{20,40,200});
}

void SpecularDetailCases()
{
    auto source = Model(501, 0x112ab470, 501);
    auto& material = source.packets[0].material;
    for (unsigned i = 0; i < 4; ++i) material.textures[i] = {501 + i, 3};
    material.scalars = {1,0,0,0};
    material.specular_colour = {1,.5f,.25f,1};
    for (auto& v : source.packets[0].vertices) v.uv3 = v.uv;
    StaticInventory inventory(*glGetCurrentResourcePool(), {source},
        {Texture(501,{80,100,120,255},0,true), Texture(502,{200,40,20,255},0,true),
         Texture(503,{128,128,128,255},0,true), Texture(504,{128,64,32,255},0,true)}, Drain);
    auto& model = *inventory.Model(501);
    auto& packet = model.packets[0];
    auto& p = *static_cast<GXSpecularDetailBlendParameters*>(packet.materialParameters);
    auto coordinate = [&](unsigned set, float u) {
        auto* values = static_cast<float*>(packet.streams[2 + set].address);
        for (unsigned i = 0; i < packet.numUniqueVertices; ++i) values[i * 2] = u;
    };
    auto normal = [&](float x, float z) {
        auto* values = static_cast<float*>(packet.streams[1].address);
        for (unsigned i = 0; i < packet.numUniqueVertices; ++i)
        { values[i * 3] = x; values[i * 3 + 1] = 0; values[i * 3 + 2] = z; }
    };
    // Independent equation: weight = mask * (1-blend), then interpolate
    // diffuse/detail per channel. GX's byte quantization permits +/-3 here.
    auto blended = [](std::array<unsigned char,3> diffuse, std::array<unsigned char,3> detail,
                      std::array<unsigned char,3> mask, float blend) {
        std::array<unsigned char,3> result{};
        for (unsigned i = 0; i < 3; ++i)
        {
            const float weight = mask[i] / 255.f * (1-blend);
            result[i] = std::lround(diffuse[i] * (1-weight) + detail[i] * weight);
        }
        return result;
    };
    PixelCase("Detail blend=1 retains diffuse", model, 0, {80,100,120});
    p.blendAmount = 0;
    PixelCase("Detail blend=0 uses mask", model, 0, {140,70,70});
    p.blendAmount = .5f;
    PixelCase("Detail fractional blend", model, 0, {110,85,95});
    coordinate(0,.875f);
    PixelCase("Detail independent diffuse UV", model, 0,
        blended({20,40,200},{200,40,20},{128,128,128},.5f));
    coordinate(0,.125f); coordinate(1,.875f);
    PixelCase("Detail independent detail UV", model, 0,
        blended({80,100,120},{20,40,200},{128,128,128},.5f));
    coordinate(1,.125f); coordinate(2,.875f);
    PixelCase("Detail independent mask UV", model, 0,
        blended({80,100,120},{200,40,20},{20,40,200},.5f));
    coordinate(2,.125f);
    auto* colours = static_cast<u8*>(packet.streams[6].address);
    for (unsigned i = 0; i < packet.numUniqueVertices; ++i)
    { colours[i*4] = 128; colours[i*4+2] = 128; }
    PixelCase("Detail vertex colour", model, 0, {55,85,47});
    for (unsigned i = 0; i < packet.numUniqueVertices; ++i)
    { colours[i*4] = colours[i*4+2] = 255; }
    GameLighting lighting;
    lighting.enabled = true; lighting.ambient = {{64,128,192,0}};
    p.lightingEnabled = 1;
    PixelCase("Detail original ambient", model, 0, {28,43,71}, lighting);
    lighting.ambient = {{0,0,0,0}};
    lighting.light_count = 1;
    lighting.lights[0].useWorldPosition = true;
    lighting.lights[0].worldPosition = {0,0,1};
    lighting.lights[0].intensity = .5f;
    PixelCase("Detail original diffuse key", model, 0, {55,42,47}, lighting);
    p.lightingEnabled = 0; p.blendAmount = 1; p.specularLevel = .5f;
    lighting.lights[0].intensity = 1;
    // Front-facing half-vector gives unit specular attenuation, independently
    // of exponent. Gloss * level * RGB tint adds (64,16,4) to diffuse.
    PixelCase("Detail zero-exponent specular", model, 0, {144,116,124}, lighting);
    p.specularExponent = 64;
    PixelCase("Detail original specular light", model, 0, {144,116,124}, lighting);
    coordinate(3,.875f);
    PixelCase("Detail independent gloss UV", model, 0, {90,110,145}, lighting);
    coordinate(3,.125f);
    normal(.5f, std::sqrt(.75f));
    // Attenuation at n.h=sqrt(.75): .75 / (32 - 31*.75) = 3/35.
    PixelCase("Detail exponent controls highlight", model, 0, {85,101,120}, lighting);
    normal(0,1);
    nlMatrix4 rotated;
    nlMakeRotationMatrixY(rotated, 3.1415927f / 6);
    rotated.SetTranslation({.15f,0,-.04019238f});
    PixelCase("Detail specular follows model normals", model, 0, {85,101,120}, lighting, &rotated);
    normal(.5f, std::sqrt(.75f));
    p.specularExponent = 0;
    // With exponent zero the nonzero half-vector term cancels in the
    // numerator/denominator, restoring unit attenuation even for a tilt.
    PixelCase("Detail zero exponent after another exponent", model, 0, {144,116,124}, lighting);
    normal(0,1);
    lighting.lights[0].intensity = .25f;
    PixelCase("Detail light refresh at unchanged exponent", model, 0, {96,104,121}, lighting);
    lighting.light_count = 0;
    PixelCase("Detail zero lights clears previous specular", model, 0, {80,100,120}, lighting);
    lighting.light_count = 2; lighting.lights[0].intensity = 0;
    lighting.lights[1] = lighting.lights[0]; lighting.lights[1].intensity = 1;
    PixelCase("Detail original slot-6 mask excludes slot 7", model, 0, {80,100,120}, lighting);
    lighting.lights[0].intensity = 1;
    lighting.lights[0].worldPosition = {0,0,-1};
    p.specularExponent = 64;
    PixelCase("Detail back-facing specular", model, 0, {80,100,120}, lighting);
    lighting = {};
    LightingLookup shadow;
    shadow.LoadTexture(17);
    lighting.shadow.lookup = &shadow; lighting.shadow.texture = 17;
    p.shadowEnabled = 1; p.specularLevel = 0; p.blendAmount = .5f;
    PixelCase("Detail original projected shadow", model, 0, {57,44,49}, lighting);
    PixelCase("Detail shadow state restored", model, 0, {110,85,95});
}

void ScrollingSpecularCases()
{
    auto source=Model(601,0x3eccd955,601);
    auto& material=source.packets[0].material;
    material.textures[1]={602,3}; material.scalars={.5f,0,.5f,0};
    material.specular_colour={1,.5f,.25f,1};
    StaticInventory inventory(*glGetCurrentResourcePool(),{source},
        {Texture(601,{80,100,120,255},0,true),Texture(602,{128,64,32,255},0,true),
         Texture(603,{80,100,120,255},0,true,true),Texture(604,{80,100,120,0},1),
         Texture(605,{80,100,120,128},8)},Drain);
    auto& model=*inventory.Model(601); auto& packet=model.packets[0];
    auto& p=*static_cast<GXScrollingSpecularParameters*>(packet.materialParameters);
    GameLighting light;light.enabled=true;light.light_count=1;
    light.lights[0].useWorldPosition=true;light.lights[0].worldPosition={0,0,1};light.lights[0].intensity=1;
    // Independent TEV equation: diffuse*lighting + gloss*level*specular*tint.
    // Front-facing light gives unit attenuation, so the highlight is (64,16,4).
    PixelCase("Scrolling specular zero exponent",model,0,{144,116,124},light);
    p.specularExponent=64;
    PixelCase("Scrolling specular fixed gloss at t=1",model,1,{84,56,204},light);
    p.scrollSpecularTexture=1;
    PixelCase("Scrolling specular both textures at t=1",model,1,{30,50,225},light);
    PixelCase("Scrolling specular wrapped t=2",model,2,{144,116,124},light);
    p.scrollSpeedX=-.5f;
    PixelCase("Scrolling specular negative offset clamps",model,1,{144,116,124},light);
    p.diffuseTexture.flags=p.specularTexture.flags=0;
    PixelCase("Scrolling specular negative speed wraps",model,1,{30,50,225},light);
    p.diffuseTexture.flags=p.specularTexture.flags=3;
    p.scrollSpeedX=.5f;
    gScrollingSpecularAnimationEnabled=false;
    PixelCase("Scrolling specular animation disabled",model,1,{144,116,124},light);
    gScrollingSpecularAnimationEnabled=true;
    auto* uv=static_cast<float*>(packet.streams[3].address);
    for(unsigned i=0;i<packet.numUniqueVertices;++i)uv[i*2]=.875f;
    PixelCase("Scrolling specular independent gloss UV",model,0,{90,110,145},light);
    for(unsigned i=0;i<packet.numUniqueVertices;++i)uv[i*2]=.125f;
    p.diffuseTexture.texture=603;p.scrollSpeedX=0;p.scrollSpeedY=.5f;
    PixelCase("Scrolling specular vertical diffuse scroll",model,1,{84,56,204},light);
    p.diffuseTexture.texture=601;p.scrollSpeedY=0;p.scrollSpeedX=.5f;
    gScrollingSpecularHighlightsEnabled=false;
    PixelCase("Scrolling specular highlights disabled",model,0,{80,100,120},light);
    gScrollingSpecularHighlightsEnabled=true;
    p.specularLevel=0;
    PixelCase("Scrolling specular zero level",model,0,{80,100,120},light);
    p.specularLevel=.5f;
    auto* colours=static_cast<u8*>(packet.streams[4].address);
    for(unsigned i=0;i<packet.numUniqueVertices;++i){colours[i*4]=128;colours[i*4+2]=128;}
    PixelCase("Scrolling specular vertex modulation",model,0,{104,116,64},light);
    for(unsigned i=0;i<packet.numUniqueVertices;++i){colours[i*4]=255;colours[i*4+2]=255;}
    p.lightingEnabled=1;light.light_count=0;light.ambient={{64,128,192,0}};
    PixelCase("Scrolling specular ambient without highlights",model,0,{20,50,90},light);
    light.light_count=1;light.ambient={{0,0,0,0}};light.lights[0].intensity=.5f;
    PixelCase("Scrolling specular diffuse and highlight",model,0,{72,58,62},light);
    light.double_intensity=true;
    PixelCase("Scrolling specular doubled diffuse",model,0,{112,108,122},light);
    light.double_intensity=false;light.ramp_texture=19;
    PixelCase("Scrolling specular texture light ramp",model,0,{52,58,92},light);
    light.double_intensity=true;
    PixelCase("Scrolling specular doubled light ramp",model,0,{72,108,182},light);
    light.double_intensity=false;light.ramp_texture=UINT32_MAX;p.lightingEnabled=0;light.lights[0].intensity=1;
    auto* normals=static_cast<float*>(packet.streams[1].address);
    for(unsigned i=0;i<packet.numUniqueVertices;++i){normals[i*3]=.5f;normals[i*3+2]=std::sqrt(.75f);}
    // n.h=sqrt(.75), attenuation=.75/(32-31*.75)=3/35 at exponent 64.
    PixelCase("Scrolling specular exponent attenuates tilted normal",model,0,{85,101,120},light);
    p.specularExponent=0;
    PixelCase("Scrolling specular zero exponent refresh",model,0,{144,116,124},light);
    for(unsigned i=0;i<packet.numUniqueVertices;++i){normals[i*3]=0;normals[i*3+2]=1;}
    p.specularExponent=64;
    nlMatrix4 rotated;nlMakeRotationMatrixY(rotated,3.1415927f/6);rotated.SetTranslation({.15f,0,-.04019238f});
    PixelCase("Scrolling specular model normal matrix",model,0,{85,101,120},light,&rotated);
    light.lights[0].intensity=.25f;
    PixelCase("Scrolling specular refresh unchanged exponent",model,0,{96,104,121},light);
    light.light_count=0;
    PixelCase("Scrolling specular zero lights clears stale slot",model,0,{80,100,120},light);
    light={};LightingLookup shadow;shadow.LoadTexture(17);
    light.shadow.lookup=&shadow;light.shadow.texture=17;p.shadowEnabled=1;p.specularLevel=0;
    PixelCase("Scrolling specular projected shadow",model,0,{42,52,62},light);
    gScrollingSpecularShadowsEnabled=false;
    PixelCase("Scrolling specular shadow switch",model,0,{80,100,120},light);
    gScrollingSpecularShadowsEnabled=true;
    PixelCase("Scrolling specular shadow state restored",model,0,{80,100,120});
    auto* program=static_cast<GLMaterialProgram*>(packet.materialProgram);
    p.diffuseTexture.texture=604;program->Prepare(&packet);
    PixelCase("Scrolling specular alpha discard",model,0,{20,24,30});
    p.diffuseTexture.texture=605;program->Prepare(&packet);
    PixelCase("Scrolling specular alpha blend",model,0,{50,62,75});
}

template<class Parameters>
void MaskedDetailCases(unsigned id, bool& shadow_switch)
{
    auto source=Model(801,id,801);
    auto& material=source.packets[0].material;
    material.textures[1]={802,3}; material.textures[2]={803,3}; material.scalars[0]=1;
    StaticInventory inventory(*glGetCurrentResourcePool(),{source},
        {Texture(801,{80,100,120,255},0,true),Texture(802,{200,40,20,255},0,true),
         Texture(803,{128,64,192,255},0,true),Texture(804,{80,100,120,0},1),
         Texture(805,{80,100,120,128},8),Texture(806,{0,0,0,255}),Texture(807,{255,255,255,255}),
         Texture(808,{200,40,20,0},8)},Drain);
    auto& model=*inventory.Model(801);auto& packet=model.packets[0];
    auto& p=*static_cast<Parameters*>(packet.materialParameters);
    int& receive_shadows = [&]() -> int& {
        if constexpr (requires { p.receiveShadows; }) return p.receiveShadows;
        else return p.shadowEnabled;
    }();
    auto coordinate=[&](unsigned set,float u){auto* uv=static_cast<float*>(packet.streams[2+set].address);
        for(unsigned i=0;i<packet.numUniqueVertices;++i)uv[i*2]=u;};
    // Independent equation: w=(1-blend)*mask.rgb/255, output=diffuse*(1-w)+detail*w.
    auto mixed=[](std::array<unsigned char,3> diffuse,std::array<unsigned char,3> detail,
                  std::array<unsigned char,3> mask,float blend){std::array<unsigned char,3> result{};
        for(unsigned i=0;i<3;++i){float w=(1-blend)*mask[i]/255.f;result[i]=std::lround(diffuse[i]*(1-w)+detail[i]*w);}return result;};
    PixelCase("Masked detail full diffuse",model,0,{80,100,120});
    p.blendAmount=0;
    PixelCase("Masked detail per-channel mask",model,0,{140,85,45});
    p.blendAmount=.5f;
    PixelCase("Masked detail fractional blend",model,0,{110,92,82});
    coordinate(0,.875f);
    PixelCase("Masked detail independent diffuse UV",model,0,mixed({20,40,200},{200,40,20},{128,64,192},.5f));
    coordinate(0,.125f);coordinate(1,.875f);
    PixelCase("Masked detail independent detail UV",model,0,mixed({80,100,120},{20,40,200},{128,64,192},.5f));
    coordinate(1,.125f);coordinate(2,.875f);
    PixelCase("Masked detail independent mask UV",model,0,mixed({80,100,120},{200,40,20},{20,40,200},.5f));
    coordinate(2,.125f);p.blendMaskTexture.texture=806;
    PixelCase("Masked detail black mask",model,0,{80,100,120});
    p.blendMaskTexture.texture=807;p.blendAmount=0;
    PixelCase("Masked detail white mask",model,0,{200,40,20});
    p.blendMaskTexture.texture=803;p.blendAmount=.5f;p.detailTexture.texture=808;
    PixelCase("Masked detail ignores detail alpha",model,0,{110,92,82});
    p.detailTexture.texture=802;
    auto* colours=static_cast<u8*>(packet.streams[5].address);
    for(unsigned i=0;i<packet.numUniqueVertices;++i){colours[i*4]=128;colours[i*4+2]=128;}
    PixelCase("Masked detail vertex modulation",model,0,{55,92,41});
    for(unsigned i=0;i<packet.numUniqueVertices;++i)colours[i*4]=colours[i*4+2]=255;
    GameLighting light;light.enabled=true;light.ambient={{64,128,192,0}};p.lightingEnabled=1;
    PixelCase("Masked detail ambient",model,0,{28,46,62},light);
    light.ambient={{0,0,0,0}};light.light_count=1;light.lights[0].useWorldPosition=true;
    light.lights[0].worldPosition={0,0,1};light.lights[0].intensity=.5f;
    PixelCase("Masked detail directional light",model,0,{55,46,41},light);
    light.ramp_texture=19;
    PixelCase("Masked detail texture light ramp",model,0,{28,46,62},light);
    light.double_intensity=true;
    PixelCase("Masked detail doubled texture ramp",model,0,{55,92,123},light);
    light.double_intensity=false;light.ramp_texture=UINT32_MAX;
    nlMatrix4 rotation;nlMakeRotationMatrixY(rotation,3.1415927f/3);rotation.SetTranslation({.25980762f,0,-.15f});
    PixelCase("Masked detail model normal transform",model,0,{27,23,20},light,&rotation);
    p.lightingEnabled=0;
    PixelCase("Masked detail lighting disabled",model,0,{110,92,82},light);
    light={};LightingLookup shadow;shadow.LoadTexture(17);light.shadow.lookup=&shadow;light.shadow.texture=17;
    receive_shadows=1;
    PixelCase("Masked detail projected shadow",model,0,{57,48,43},light);
    shadow_switch=false;
    PixelCase("Masked detail global shadow switch",model,0,{110,92,82},light);
    shadow_switch=true;receive_shadows=0;
    PixelCase("Masked detail material shadow switch",model,0,{110,92,82},light);
    auto* program=static_cast<GLMaterialProgram*>(packet.materialProgram);
    p.diffuseTexture.texture=804;program->Prepare(&packet);
    PixelCase("Masked detail diffuse alpha discard",model,0,{20,24,30});
    p.diffuseTexture.texture=805;program->Prepare(&packet);
    PixelCase("Masked detail diffuse alpha blend",model,0,{65,58,56});
    p.diffuseTexture.texture=801;program->Prepare(&packet);
    PixelCase("Masked detail restores alpha state",model,0,{110,92,82});
}

void ScrollingMaskedDetailCases()
{
    auto source=Model(901,0xf2d57ac6,901);
    auto& material=source.packets[0].material;
    material.textures[1]={902,3};material.textures[2]={903,3};material.scalars[0]=.5f;
    StaticInventory inventory(*glGetCurrentResourcePool(),{source},
        {Texture(901,{80,100,120,255},0,true),Texture(902,{200,40,20,255},0,true),Texture(903,{128,64,192,255},0,true),
         Texture(904,{80,100,120,255},0,true,true),Texture(905,{200,40,20,255},0,true,true),Texture(906,{128,64,192,255},0,true,true)},Drain);
    auto& model=*inventory.Model(901);auto& packet=model.packets[0];
    auto& p=*static_cast<GXScrollingMaskedDetailBlendParameters*>(packet.materialParameters);
    std::array<glTextureBinding*,3> bindings{&p.diffuseTexture,&p.detailTexture,&p.blendMaskTexture};
    std::array<float*,6> speeds{&p.diffuseScrollSpeedX,&p.diffuseScrollSpeedY,&p.detailScrollSpeedX,
        &p.detailScrollSpeedY,&p.blendMaskScrollSpeedX,&p.blendMaskScrollSpeedY};
    const std::array<std::array<unsigned char,3>,3> original{{{80,100,120},{200,40,20},{128,64,192}}};
    auto blended=[](const auto& samples){std::array<unsigned char,3> result{};
        for(unsigned i=0;i<3;++i){float w=.5f*samples[2][i]/255.f;result[i]=std::lround(samples[0][i]*(1-w)+samples[1][i]*w);}return result;};
    PixelCase("Scrolling masked detail zero time",model,0,blended(original));
    for(unsigned set=0;set<3;++set)
    {
        std::cout << "Scrolling texture binding " << set << '\n';
        auto sampled=original;sampled[set]={20,40,200};
        *speeds[set*2]=.5f;
        PixelCase("Independent positive X scroll",model,1,blended(sampled));
        PixelCase("Original offset wraps at whole period",model,2,blended(original));
        *speeds[set*2]=-.5f;
        PixelCase("Negative X scroll clamps",model,1,blended(original));
        bindings[set]->flags=0;
        PixelCase("Negative X scroll repeats",model,1,blended(sampled));
        *speeds[set*2]=0;*speeds[set*2+1]=.5f;bindings[set]->texture=904+set;bindings[set]->flags=3;
        PixelCase("Independent positive Y scroll",model,1,blended(sampled));
        *speeds[set*2+1]=-.5f;bindings[set]->flags=0;
        PixelCase("Independent negative Y scroll repeats",model,1,blended(sampled));
        *speeds[set*2+1]=0;bindings[set]->texture=901+set;bindings[set]->flags=3;
    }
    p.diffuseScrollSpeedX=.5f;p.detailScrollSpeedX=-.5f;p.blendMaskScrollSpeedX=.5f;p.detailTexture.flags=0;
    std::array<std::array<unsigned char,3>,3> shifted{{{20,40,200},{20,40,200},{20,40,200}}};
    PixelCase("Three independently scrolling textures together",model,1,blended(shifted));
    p.diffuseScrollSpeedX=p.detailScrollSpeedX=p.blendMaskScrollSpeedX=0;
    PixelCase("Zero speeds restore original coordinates",model,100,blended(original));
}

void CameraOverlayCases()
{
    auto source = Model(701, 0x32bc21e8, 701);
    auto& material = source.packets[0].material;
    material.textures[1] = {702,3}; material.textures[2] = {703,3};
    material.scalars = {2,1,.5f,0};
    StaticInventory inventory(*glGetCurrentResourcePool(), {source},
        {Texture(701,{80,100,120,255},0,true), Texture(702,{200,80,40,128},8,true),
         Texture(703,{128,64,192,255},0,true), Texture(704,{200,80,40,128},8,true,true),
         Texture(705,{200,80,40,0},8), Texture(706,{80,100,120,0},1),
         Texture(707,{80,100,120,128},8)}, Drain);
    auto& model = *inventory.Model(701); auto& packet = model.packets[0];
    auto& p = *static_cast<GXCameraScrolledOverlayParameters*>(packet.materialParameters);
    nlVector3 camera{.75f,.75f,3};
    auto pixel = [&](const char* name, std::array<unsigned char,3> expected,
                     const GameLighting& light = GameLighting{}, const nlMatrix4* world = nullptr,
                     const nlMatrix4* view = nullptr) {
        PixelCase(name, model, 0, expected, light, world, view, nullptr, {}, 8, {}, &camera);
    };
    // Independent four-stage equation: diffuse*vertex/light +
    // overlay.rgb*overlay.alpha*amount*mask.rgb. Byte tolerance is +/-3.
    pixel("Camera overlay base alpha and mask", {105,105,128});
    p.overlayAmount = 0;
    pixel("Camera overlay zero amount", {80,100,120});
    p.overlayAmount = 1;
    pixel("Camera overlay full amount", {130,110,135});
    p.overlayAmount = .5f;
    camera.x = -.25f;
    pixel("Camera overlay camera X motion", {85,105,195});
    camera.x = .75f;
    pixel("Camera overlay refresh reused camera input", {105,105,128});
    camera.z = -100;
    pixel("Camera overlay ignores camera Z", {105,105,128});
    p.overlayTexture.texture = 704; camera.y = -.25f;
    pixel("Camera overlay camera Y motion", {85,105,195});
    camera.y = .75f; p.overlayTexture.texture = 702;
    p.cameraScroll = -1;
    pixel("Camera overlay reversed camera scroll", {85,105,195});
    p.cameraScroll = 1; p.overlayScale = -2;
    pixel("Camera overlay negative scale", {85,105,195});
    p.overlayScale = 0;
    pixel("Camera overlay zero scale uses unit inverse", {105,105,128});
    p.overlayTexture.flags = 0; camera.x = 1.5f; p.overlayScale = 2;
    pixel("Camera overlay repeating projected coordinates", {85,105,195});
    p.overlayScale = 4;
    pixel("Camera overlay changed projection scale", {105,105,128});
    p.overlayTexture.flags = 3; camera.x = .75f; p.overlayScale = 2;
    auto coordinate = [&](unsigned stream, float u) {
        auto* data = static_cast<float*>(packet.streams[stream].address);
        for (unsigned i = 0; i < packet.numUniqueVertices; ++i) data[i * 2] = u;
    };
    coordinate(2, .875f);
    pixel("Camera overlay independent diffuse UV", {45,45,208});
    coordinate(2, .125f); coordinate(3, .875f);
    pixel("Camera overlay uses position instead of UV1", {105,105,128});
    coordinate(3, .125f); coordinate(4, .875f);
    pixel("Camera overlay independent mask UV", {84,103,128});
    coordinate(4, .125f);
    auto* positions = static_cast<float*>(packet.streams[0].address);
    for (unsigned i = 0; i < packet.numUniqueVertices; ++i) positions[i * 3] += 1;
    nlMatrix4 world; world.SetIdentity(); world.SetTranslation({-1,0,0});
    pixel("Camera overlay retains local vertex coordinates", {85,105,195}, {}, &world);
    for (unsigned i = 0; i < packet.numUniqueVertices; ++i) positions[i * 3] -= 1.25f;
    world.SetTranslation({.25f,0,0}); p.cameraScroll = 0; camera.x = -100;
    pixel("Camera overlay zero camera scroll", {105,105,128}, {}, &world);
    for (unsigned i = 0; i < packet.numUniqueVertices; ++i) positions[i * 3] += .25f;
    p.cameraScroll = 1; camera.x = .75f;
    nlMatrix4 shifted_view; shifted_view.SetIdentity(); shifted_view.SetTranslation({1,0,0});
    world.SetTranslation({-1,0,0});
    pixel("Camera overlay active camera independent of render view", {105,105,128}, {}, &world, &shifted_view);
    p.overlayTexture.texture = 705;
    pixel("Camera overlay transparent overlay contributes nothing", {80,100,120});
    p.overlayTexture.texture = 702;
    auto* colours = static_cast<u8*>(packet.streams[5].address);
    for (unsigned i = 0; i < packet.numUniqueVertices; ++i)
    { colours[i * 4] = 128; colours[i * 4 + 2] = 128; colours[i * 4 + 3] = 0; }
    pixel("Camera overlay vertex colour with texture-only alpha", {65,105,68});
    for (unsigned i = 0; i < packet.numUniqueVertices; ++i)
    { colours[i * 4] = 255; colours[i * 4 + 2] = 255; colours[i * 4 + 3] = 255; }
    GameLighting light; light.enabled = true; light.ambient = {{64,128,192,0}}; p.lightingEnabled = 1;
    pixel("Camera overlay ambient affects diffuse only", {45,55,98}, light);
    light.ambient = {{0,0,0,0}}; light.light_count = 1;
    light.lights[0].useWorldPosition = true; light.lights[0].worldPosition = {0,0,1}; light.lights[0].intensity = .5f;
    pixel("Camera overlay directional diffuse", {65,55,68}, light);
    light.lights[0].worldPosition.z = -1;
    pixel("Camera overlay unlit diffuse retains overlay", {25,5,8}, light);
    p.lightingEnabled = 0; light = {};
    LightingLookup shadow; shadow.LoadTexture(17); light.shadow.lookup = &shadow; light.shadow.texture = 17;
    p.shadowEnabled = 1;
    pixel("Camera overlay original projected shadow", {55,55,67}, light);
    gCameraOverlayShadowsEnabled = false;
    pixel("Camera overlay shadow switch", {105,105,128}, light);
    gCameraOverlayShadowsEnabled = true;
    pixel("Camera overlay restores shadow state", {105,105,128});
    p.overlayAmount = 0; p.diffuseTexture.flags = 0; coordinate(2,-.375f);
    pixel("Camera overlay diffuse initially repeats", {20,40,200});
    p.diffuseWrapEnabled = 1;
    // Original Draw changes the flags after binding; subsequent draws clamp.
    pixel("Camera overlay original wrap flag mutation", {80,100,120});
    Check(p.diffuseTexture.flags == 3, "Overlay wrap mutation did not persist");
    p.diffuseWrapEnabled = 0;
    pixel("Camera overlay preserves mutated binding flags", {80,100,120});
    coordinate(2,.125f);
    auto* program = static_cast<GLMaterialProgram*>(packet.materialProgram);
    p.diffuseTexture.texture = 706; program->Prepare(&packet);
    pixel("Camera overlay diffuse alpha discard", {20,24,30});
    p.diffuseTexture.texture = 707; program->Prepare(&packet);
    pixel("Camera overlay diffuse alpha blend", {50,62,75});
    p.diffuseTexture.texture = 701; program->Prepare(&packet); p.overlayAmount = .5f;
    bool rejected = false;
    try { PixelCase("Missing camera must fail", model, 0, {0,0,0}); }
    catch (const std::logic_error&) { rejected = true; }
    Check(rejected, "Camera overlay silently accepted a missing camera");
    camera.x = std::numeric_limits<float>::max(); p.overlayScale = .5f; rejected = false;
    try { pixel("Overflowing camera matrix must fail", {0,0,0}); }
    catch (const std::invalid_argument&) { rejected = true; }
    Check(rejected, "Camera overlay accepted an overflowing texture matrix");
    camera.x = .75f; p.overlayScale = 2;
    pixel("Camera overlay recovers after rejected camera draws", {105,105,128});
}
void ScrollingCameraOverlayCases()
{
    auto source = Model(1001, 0x845cad59, 1001);
    auto& material = source.packets[0].material;
    material.textures[1] = {1002,3}; material.textures[2] = {1003,3};
    material.scalars = {2,1,.5f,0};
    StaticInventory inventory(*glGetCurrentResourcePool(), {source},
        {Texture(1001,{80,100,120,255},0,true), Texture(1002,{200,80,40,128},8,true),
         Texture(1003,{128,64,192,255},0,true), Texture(1004,{200,80,40,128},8,true,true),
         Texture(1005,{200,80,40,0},8), Texture(1006,{80,100,120,0},1),
         Texture(1007,{80,100,120,128},8), Texture(1008,{80,100,120,255}),
         Texture(1009,{80,100,120,255},0,true,true), Texture(1010,{128,64,192,255},0,true,true)}, Drain);
    auto& model = *inventory.Model(1001); auto& packet = model.packets[0];
    auto& p = *static_cast<GXScrollingCameraOverlayParameters*>(packet.materialParameters);
    nlVector3 camera{.75f,.75f,3};
    float time = 0;
    auto pixel = [&](const char* name, std::array<unsigned char,3> expected,
                     const GameLighting& light = GameLighting{}, const nlMatrix4* world = nullptr,
                     const nlMatrix4* view = nullptr) {
        PixelCase(name, model, time, expected, light, world, view, nullptr, {}, 8, {}, &camera);
    };
    // Independent four-stage equation: diffuse*vertex/light +
    // overlay.rgb*overlay.alpha*amount*mask.rgb. Byte tolerance is +/-3.
    pixel("Scrolling camera overlay base alpha and mask", {105,105,128});
    p.diffuseScrollSpeedX = .5f; time = 1;
    pixel("Scrolling overlay diffuse and mask at t=1 with flag zero", {24,43,208});
    p.maskScrollEnabled = 1;
    pixel("Scrolling overlay address-based mask flag preserves motion", {24,43,208});
    time = 2;
    pixel("Scrolling overlay original wrapped time", {105,105,128});
    time = 1; p.maskScrollEnabled = 0; p.diffuseScrollSpeedX = -.5f;
    pixel("Scrolling overlay negative offsets clamp", {105,105,128});
    p.diffuseTexture.flags = p.overlayMaskTexture.flags = 0;
    pixel("Scrolling overlay negative offsets repeat", {24,43,208});
    p.diffuseTexture.flags = p.overlayMaskTexture.flags = 3; p.diffuseScrollSpeedX = .5f;
    p.diffuseTexture.texture = 1008;
    pixel("Scrolling overlay mask motion independent of diffuse sampling", {84,103,128});
    p.diffuseTexture.texture = 1009; p.diffuseScrollSpeedX = 0; p.diffuseScrollSpeedY = .5f;
    pixel("Scrolling overlay vertical diffuse motion", {45,45,208});
    p.overlayMaskTexture.texture = 1010;
    pixel("Scrolling overlay vertical mask follows diffuse speed", {24,43,208});
    p.diffuseScrollSpeedY = -.5f; p.diffuseTexture.flags = p.overlayMaskTexture.flags = 0;
    pixel("Scrolling overlay negative vertical motion repeats", {24,43,208});
    p.diffuseTexture.flags = p.overlayMaskTexture.flags = 3;
    p.diffuseTexture.texture = 1001; p.overlayMaskTexture.texture = 1003;
    p.diffuseScrollSpeedY = 0; time = 100;
    pixel("Scrolling overlay zero speed ignores time", {105,105,128});
    time = 0;
    p.overlayAmount = 0;
    pixel("Scrolling camera overlay zero amount", {80,100,120});
    p.overlayAmount = 1;
    pixel("Scrolling camera overlay full amount", {130,110,135});
    p.overlayAmount = .5f;
    camera.x = -.25f;
    pixel("Scrolling camera overlay camera X motion", {85,105,195});
    camera.x = .75f;
    pixel("Scrolling camera overlay refresh reused camera input", {105,105,128});
    camera.z = -100;
    pixel("Scrolling camera overlay ignores camera Z", {105,105,128});
    p.overlayTexture.texture = 1004; camera.y = -.25f;
    pixel("Scrolling camera overlay camera Y motion", {85,105,195});
    camera.y = .75f; p.overlayTexture.texture = 1002;
    p.cameraScroll = -1;
    pixel("Scrolling camera overlay reversed camera scroll", {85,105,195});
    p.cameraScroll = 1; p.overlayScale = -2;
    pixel("Scrolling camera overlay negative scale", {85,105,195});
    p.overlayTexture.flags = 0; camera.x = 1.5f; p.overlayScale = 2;
    pixel("Scrolling camera overlay repeating projected coordinates", {85,105,195});
    p.overlayScale = 4;
    pixel("Scrolling camera overlay changed projection scale", {105,105,128});
    p.overlayTexture.flags = 3; camera.x = .75f; p.overlayScale = 2;
    auto coordinate = [&](unsigned stream, float u) {
        auto* data = static_cast<float*>(packet.streams[stream].address);
        for (unsigned i = 0; i < packet.numUniqueVertices; ++i) data[i * 2] = u;
    };
    coordinate(3, .875f);
    pixel("Scrolling camera overlay independent diffuse UV", {45,45,208});
    coordinate(3, .125f); coordinate(2, .875f);
    pixel("Scrolling camera overlay uses position instead of UV0", {105,105,128});
    coordinate(2, .125f); coordinate(4, .875f);
    pixel("Scrolling camera overlay independent mask UV", {84,103,128});
    coordinate(4, .125f);
    auto* positions = static_cast<float*>(packet.streams[0].address);
    for (unsigned i = 0; i < packet.numUniqueVertices; ++i) positions[i * 3] += 1;
    nlMatrix4 world; world.SetIdentity(); world.SetTranslation({-1,0,0});
    pixel("Scrolling camera overlay retains local vertex coordinates", {85,105,195}, {}, &world);
    for (unsigned i = 0; i < packet.numUniqueVertices; ++i) positions[i * 3] -= 1.25f;
    world.SetTranslation({.25f,0,0}); p.cameraScroll = 0; camera.x = -100;
    pixel("Scrolling camera overlay zero camera scroll", {105,105,128}, {}, &world);
    for (unsigned i = 0; i < packet.numUniqueVertices; ++i) positions[i * 3] += .25f;
    p.cameraScroll = 1; camera.x = .75f;
    nlMatrix4 shifted_view; shifted_view.SetIdentity(); shifted_view.SetTranslation({1,0,0});
    world.SetTranslation({-1,0,0});
    pixel("Scrolling camera overlay active camera independent of render view", {105,105,128}, {}, &world, &shifted_view);
    p.overlayTexture.texture = 1005;
    pixel("Scrolling camera overlay transparent overlay contributes nothing", {80,100,120});
    p.overlayTexture.texture = 1002;
    auto* colours = static_cast<u8*>(packet.streams[5].address);
    for (unsigned i = 0; i < packet.numUniqueVertices; ++i)
    { colours[i * 4] = 128; colours[i * 4 + 2] = 128; colours[i * 4 + 3] = 0; }
    pixel("Scrolling camera overlay vertex colour with texture-only alpha", {65,105,68});
    for (unsigned i = 0; i < packet.numUniqueVertices; ++i)
    { colours[i * 4] = 255; colours[i * 4 + 2] = 255; colours[i * 4 + 3] = 255; }
    GameLighting light; light.enabled = true; light.ambient = {{64,128,192,0}}; p.lightingEnabled = 1;
    pixel("Scrolling camera overlay ambient affects diffuse only", {45,55,98}, light);
    light.ambient = {{0,0,0,0}}; light.light_count = 1;
    light.lights[0].useWorldPosition = true; light.lights[0].worldPosition = {0,0,1}; light.lights[0].intensity = .5f;
    pixel("Scrolling camera overlay directional diffuse", {65,55,68}, light);
    light.lights[0].worldPosition.z = -1;
    pixel("Scrolling camera overlay unlit diffuse retains overlay", {25,5,8}, light);
    p.lightingEnabled = 0; light = {};
    LightingLookup shadow; shadow.LoadTexture(17); light.shadow.lookup = &shadow; light.shadow.texture = 17;
    p.shadowEnabled = 1;
    pixel("Scrolling camera overlay original projected shadow", {55,55,67}, light);
    gScrollingCameraOverlayShadowsEnabled = false;
    pixel("Scrolling camera overlay shadow switch", {105,105,128}, light);
    gScrollingCameraOverlayShadowsEnabled = true;
    pixel("Scrolling camera overlay restores shadow state", {105,105,128});
    p.overlayAmount = 0; p.diffuseTexture.flags = 0; coordinate(3,-.375f);
    pixel("Scrolling camera overlay diffuse initially repeats", {20,40,200});
    p.diffuseWrapEnabled = 1;
    // Original Draw changes the flags after binding; subsequent draws clamp.
    pixel("Scrolling camera overlay original wrap flag mutation", {80,100,120});
    Check(p.diffuseTexture.flags == 3, "Overlay wrap mutation did not persist");
    p.diffuseWrapEnabled = 0;
    pixel("Scrolling camera overlay preserves mutated binding flags", {80,100,120});
    coordinate(3,.125f);
    auto* program = static_cast<GLMaterialProgram*>(packet.materialProgram);
    p.diffuseTexture.texture = 1006; program->Prepare(&packet);
    pixel("Scrolling camera overlay diffuse alpha discard", {20,24,30});
    p.diffuseTexture.texture = 1007; program->Prepare(&packet);
    pixel("Scrolling camera overlay diffuse alpha blend", {50,62,75});
    p.diffuseTexture.texture = 1001; program->Prepare(&packet); p.overlayAmount = .5f;
    bool rejected = false;
    try { PixelCase("Missing camera must fail", model, 0, {0,0,0}); }
    catch (const std::logic_error&) { rejected = true; }
    Check(rejected, "Scrolling camera overlay silently accepted a missing camera");
    p.overlayScale = 0; rejected = false;
    try { pixel("Zero scrolling camera scale must fail", {0,0,0}); }
    catch (const std::invalid_argument&) { rejected = true; }
    Check(rejected, "Scrolling camera accepted a zero texture scale");
    camera.x = std::numeric_limits<float>::max(); p.overlayScale = .5f; rejected = false;
    try { pixel("Overflowing camera matrix must fail", {0,0,0}); }
    catch (const std::invalid_argument&) { rejected = true; }
    Check(rejected, "Scrolling camera overlay accepted an overflowing texture matrix");
    camera.x = .75f; p.overlayScale = 2;
    pixel("Scrolling camera overlay recovers after rejected camera draws", {105,105,128});
}
} // namespace
int main(int argc, char **argv)
{
    try
    {
        const auto directory = std::filesystem::path(SDL_GetBasePath()) / "material-test-data";
        std::filesystem::create_directories(directory);
        const auto path = directory.string();
        AuroraConfig cfg{};
        cfg.appName = "Charged material pixel checks";
        cfg.userPath = cfg.cachePath = path.c_str();
        cfg.resourcesPath = SDL_GetBasePath();
        cfg.desiredBackend = BACKEND_VULKAN;
        cfg.enableBackendValidation = true;
        cfg.windowWidth = 640;
        cfg.windowHeight = 480;
        cfg.windowPosX = cfg.windowPosY = -1;
        cfg.vsync = true;
        cfg.logLevel = LOG_WARNING;
        cfg.logCallback = Log;
        cfg.mem1Size = MEM1_DEFAULT_SIZE;
        cfg.mem2Size = 64 * 1024 * 1024;
        Session session;
        const auto info = aurora_initialize(argc, argv, &cfg);
        session.live = true;
        Check(info.backend == BACKEND_VULKAN && info.window, "Material tests require real Vulkan");
        ImGui::GetIO().IniFilename = nullptr;
        ImGui::GetIO().LogFilename = nullptr;
        InitializeStartupOS();
        nlInitMemory();
        const auto free1 = StandardAllocator.TotalFreeMemory(), free2 = VirtualAllocator.TotalFreeMemory();
        glInitResourcePools();
        InitializeOriginalGraphicsMemory();
        InitializeOriginalGraphicsState();
        VIInit();
        VIConfigure(&GXNtsc480IntDf);
        alignas(32) std::array<unsigned char, 65536> fifo{};
        GXInit(fifo.data(), fifo.size());
        session.gx = true;
        AuroraSetViewportPolicy(AURORA_VIEWPORT_FIT);
        SetGraphicsCacheInvalidator(Invalidate);
        MaterialPrograms programs;
        auto unlit = Model(1, 0x21db4385, 10), vertex = Model(2, 0xd3e572da, 10), scroll = Model(3, 0x2169db5c, 11),
             masked = Model(4, 0x32475c7d, 10);
        auto float_colour = Model(20,0x19065bf6,10),constant_colour = Model(21,0xee9d919d,10);
        for(auto& v:float_colour.packets[0].vertices)v.colour={128,255,128,255};
        constant_colour.packets[0].material.specular_colour={.5f,1,.25f,1};
        for (auto &v : vertex.packets[0].vertices)
            v.colour = {128, 255, 128, 255};
        scroll.packets[0].material.scalars[0] = .5f;
        auto &m = masked.packets[0].material;
        m.textures[1] = {12, 3};
        m.textures[2] = {13, 3};
        m.scalars = {.5f, 1, 1, 1};
        auto normal_left = masked, normal_right = masked;
        normal_left.id = 8;
        normal_right.id = 9;
        normal_left.packets[0].material.textures[1].texture = normal_right.packets[0].material.textures[1].texture = 11;
        for (auto &v : normal_left.packets[0].vertices)
            v.normal = {-.75f, 0, std::sqrt(7.f) / 4};
        for (auto &v : normal_right.packets[0].vertices)
            v.normal = {.75f, 0, std::sqrt(7.f) / 4};
        auto discard = Model(5, 0x21db4385, 14), blend = Model(6, 0x21db4385, 15);
        auto palette = Texture(16, {0, 0, 0, 255});
        palette.game_format = 8;
        palette.gx_format = 9;
        palette.bits = {5, 5, 5, 0};
        palette.palette_entries = 2;
        palette.palette = {0xfc, 0, 0x80, 0x1f};
        palette.pixels.assign(32, 0);
        auto shadow_texture = palette;
        shadow_texture.id = 17; shadow_texture.width = 8;
        shadow_texture.palette = {0xc2,0x10,0xff,0xff};
        auto split_shadow = shadow_texture;
        split_shadow.id = 18; split_shadow.palette = {0x80,0,0xff,0xff};
        for (unsigned i = 0; i < 32; ++i) split_shadow.pixels[i] = i % 8 < 4 ? 0 : 1;
        auto ci8 = Model(7, 0x21db4385, 16);
        std::vector<resources::Texture> textures = {Texture(10, {80, 100, 120, 255}),
                                                    Texture(11, {200, 40, 20, 255}, 0, true),
                                                    Texture(12, {200, 100, 40, 255}),
                                                    Texture(13, {128, 128, 128, 255}),
                                                    Texture(glGetTexture("global/fresnel1"), {128, 128, 128, 255}),
                                                    Texture(14, {200, 100, 40, 0}, 1),
                                                    Texture(15, {80, 100, 120, 128}, 8),
                                                    palette, shadow_texture, split_shadow,
                                                    Texture(19, {64, 128, 192, 255})};
        OriginalViews views(GXNtsc480IntDf.fbWidth, GXNtsc480IntDf.efbHeight, Drain);
        StaticInventory inventory(*glGetCurrentResourcePool(),
                                  {unlit, vertex, scroll, masked, discard, blend, ci8, normal_left, normal_right,float_colour,constant_colour},
                                  textures, Drain);
        const bool animation_only = argc == 2 && std::string_view(argv[1]) == "--texture-animation-only";
        const bool effects_only = argc == 2 && std::string_view(argv[1]) == "--effects-only";
        if (effects_only || argc == 1)
        {
            PixelCase("Float UV vertex-colour modulation",*inventory.Model(20),0,{40,100,60});
            PixelCase("Constant-colour modulation",*inventory.Model(21),0,{40,100,30});
            PixelCase("Unlit after effects materials",*inventory.Model(1),0,{80,100,120});
        }
        if (animation_only)
        {
            TextureAnimationCases();
            PixelCase("Unlit after texture animation cleanup", *inventory.Model(1), 0, {80,100,120});
        }
        const bool targets_only = argc == 2 && std::string_view(argv[1]) == "--targets-only";
        const bool specular_only = argc == 2 && std::string_view(argv[1]) == "--specular-only";
        const bool scrolling_specular_only = argc == 2 && std::string_view(argv[1]) == "--scrolling-specular-only";
        const bool camera_overlay_only = argc == 2 && std::string_view(argv[1]) == "--camera-overlay-only";
        const bool masked_detail_only = argc == 2 && std::string_view(argv[1]) == "--masked-detail-only";
        const bool scrolling_masked_detail_only = argc == 2 && std::string_view(argv[1]) == "--scrolling-masked-detail-only";
        if (scrolling_masked_detail_only)
        {
            MaskedDetailCases<GXScrollingMaskedDetailBlendParameters>(0xf2d57ac6, gScrollingMaskedDetailBlendShadowsEnabled);
            ScrollingMaskedDetailCases();
            PixelCase("Unlit after scrolling masked detail", *inventory.Model(1), 0, {80,100,120});
        }
        if (masked_detail_only)
        {
            MaskedDetailCases<GXMaskedDetailBlendParameters>(0x09609a35, gMaskedDetailBlendShadowsEnabled);
            PixelCase("Unlit after masked detail", *inventory.Model(1), 0, {80,100,120});
        }
        const bool scrolling_camera_only = argc == 2 && std::string_view(argv[1]) == "--scrolling-camera-only";
        if (scrolling_camera_only)
        {
            ScrollingCameraOverlayCases();
            PixelCase("Unlit after scrolling camera overlay", *inventory.Model(1), 0, {80,100,120});
        }
        if (camera_overlay_only)
        {
            CameraOverlayCases();
            PixelCase("Unlit after camera overlay", *inventory.Model(1), 0, {80,100,120});
        }
        if (scrolling_specular_only)
        {
            ScrollingSpecularCases();
            PixelCase("Unlit after scrolling specular stages", *inventory.Model(1), 0, {80,100,120});
        }
        if (specular_only)
        {
            SpecularDetailCases();
            PixelCase("Unlit after detail/specular/shadow stages", *inventory.Model(1), 0, {80,100,120});
        }
        if (!targets_only && !specular_only && !scrolling_specular_only && !camera_overlay_only && !masked_detail_only && !scrolling_masked_detail_only && !scrolling_camera_only && !animation_only && !effects_only)
        {
        PixelCase("Unlit diffuse", *inventory.Model(1), 0, {80, 100, 120});
        PixelCase("Vertex colour modulation", *inventory.Model(2), 0, {40, 100, 60});
        PixelCase("Scrolling t=0", *inventory.Model(3), 0, {200, 40, 20});
        PixelCase("Scrolling t=1", *inventory.Model(3), 1, {20, 40, 200});
        PixelCase("Specular mask and Fresnel", *inventory.Model(4), 0, {105, 113, 125});
        PixelCase("Normal lookup left", *inventory.Model(8), 0, {105, 105, 123});
        PixelCase("Normal lookup right", *inventory.Model(9), 0, {83, 105, 145});
        static_cast<GXMaskedSpecularFresnelParameters *>(inventory.Model(4)->packets[0].materialParameters)
            ->specularAmount = 0;
        PixelCase("Specular disabled", *inventory.Model(4), 0, {80, 100, 120});
        PixelCase("Alpha discard", *inventory.Model(5), 0, {20, 24, 30});
        PixelCase("Alpha blend", *inventory.Model(6), 0, {50, 62, 75});
        PixelCase("Big-endian CI8 palette", *inventory.Model(7), 0, {255, 0, 0});
        auto* masked_parameters = static_cast<GXMaskedSpecularFresnelParameters*>(inventory.Model(4)->packets[0].materialParameters);
        auto* scrolling_parameters = static_cast<GXScrollingDiffuseParameters*>(inventory.Model(3)->packets[0].materialParameters);
        masked_parameters->lightingEnabled = 1;
        scrolling_parameters->lightingEnabled = 1;
        GameLighting lighting;
        lighting.enabled = true;
        lighting.ambient = {{64,128,192,0}};
        PixelCase("Ambient only, zero direct lights", *inventory.Model(4), 0, {20,50,90}, lighting);
        PixelCase("Scrolling ambient", *inventory.Model(3), 0, {50,20,15}, lighting);
        lighting.ambient = {{0,0,0,0}};
        lighting.light_count = 1;
        lighting.lights[0].useWorldPosition = true;
        lighting.lights[0].worldPosition = {0,0,1};
        lighting.lights[0].intensity = .5f;
        PixelCase("Directional front", *inventory.Model(4), 0, {40,50,60}, lighting);
        masked_parameters->specularAmount = .5f;
        PixelCase("Directional plus original specular", *inventory.Model(4), 0, {65,63,65}, lighting);
        masked_parameters->specularAmount = 0;
        lighting.lights[0].worldPosition.z = -1;
        PixelCase("Directional back", *inventory.Model(4), 0, {0,0,0}, lighting);
        lighting.lights[0].worldPosition.z = 1;
        lighting.lights[0].intensity = 1;
        lighting.lights[0].useColour = 1;
        lighting.lights[0].colour = {{255,128,0,255}};
        PixelCase("Coloured directional light", *inventory.Model(4), 0, {80,50,0}, lighting);
        lighting.lights[0].useColour = 0;
        nlMatrix4 rotated, camera;
        nlMakeRotationMatrixY(rotated, 3.1415927f / 3);
        rotated.SetTranslation({.2598076f,0,-.15f}); // Keep the tilted triangle centred at z=-.3.
        PixelCase("Directional light follows transformed normals", *inventory.Model(4), 0, {40,50,60}, lighting, &rotated);
        nlMakeRotationMatrixY(rotated, 3.1415927f / 3);
        nlMakeRotationMatrixY(camera, -3.1415927f / 3);
        PixelCase("Directional light follows changed view", *inventory.Model(4), 0, {40,50,60}, lighting, &rotated, &camera);
        lighting.lights[0].isPointLight = 1;
        lighting.lights[0].worldPosition.z = .7f;
        lighting.lights[0].radius = 3;
        // Independent vertex diffuse/steep-attenuation values, interpolated at
        // the centre (weights 1/4, 1/4, 1/2): approximately .488 and .131.
        PixelCase("Point light near", *inventory.Model(4), 0, {39,49,59}, lighting);
        lighting.lights[0].worldPosition.z = 4.7f;
        PixelCase("Point light distance attenuation", *inventory.Model(4), 0, {10,13,16}, lighting);
        lighting.lights[0].isPointLight = 0;
        lighting.lights[0].worldPosition.z = 1;
        lighting.lights[0].intensity = 16.0f/255.0f;
        lighting.light_count = 6;
        for (unsigned i = 1; i < 6; ++i) lighting.lights[i] = lighting.lights[0];
        PixelCase("All six diffuse light slots", *inventory.Model(4), 0, {30,38,45}, lighting);
        lighting.light_count = 0;
        lighting.ambient = {{64,128,192,0}};
        lighting.double_intensity = true;
        masked_parameters->specularAmount = .5f;
        PixelCase("Double diffuse preserves specular", *inventory.Model(4), 0, {65,113,185}, lighting);
        lighting.double_intensity = false;
        lighting.ramp_texture = 19;
        PixelCase("Texture light ramp", *inventory.Model(4), 0, {45,63,95}, lighting);
        PixelCase("Scrolling texture light ramp", *inventory.Model(3), 0, {50,20,15}, lighting);
        lighting.double_intensity = true;
        PixelCase("Double texture light ramp", *inventory.Model(4), 0, {65,113,185}, lighting);
        lighting = {};
        {
            LightingLookup shadow;
            shadow.LoadTexture(17);
            lighting.shadow.lookup = &shadow; lighting.shadow.texture = 17;
            masked_parameters->shadowEnabled = 1;
            PixelCase("Projected CI8 shadow", *inventory.Model(4), 0, {54,58,65}, lighting);
            scrolling_parameters->shadowEnabled = 1;
            PixelCase("Scrolling projected shadow", *inventory.Model(3), 0, {104,21,10}, lighting);
            shadow.LoadTexture(18);
            lighting.shadow.texture = 18;
            lighting.shadow.scale = {1,1};
            lighting.shadow.translation = {-.75f,0};
            PixelCase("Shadow projection dark side", *inventory.Model(4), 0, {0,0,0}, lighting);
            lighting.shadow.translation[0] = .75f;
            PixelCase("Shadow projection light side", *inventory.Model(4), 0, {105,113,125}, lighting);
            lighting.shadow.translation = {0,0};
            nlMatrix4 world, view;
            world.SetIdentity(); view.SetIdentity();
            world.SetTranslation({.5f,0,0}); view.SetTranslation({-.5f,0,0});
            PixelCase("Shadow uses world model matrix", *inventory.Model(4), 0, {105,113,125}, lighting, &world, &view);
            world.SetTranslation({-.5f,0,0}); view.SetTranslation({.5f,0,0});
            PixelCase("Shadow matrix refresh at reused frame address", *inventory.Model(4), 0, {0,0,0}, lighting, &world, &view);
        }
        PixelCase("Material restored after shadow and ramp", *inventory.Model(4), 0, {105,113,125});
        // Switch back to verify TEV/channel/texture state does not leak between programs.
        PixelCase("Unlit after multi-stage materials", *inventory.Model(1), 0, {80, 100, 120});
        }
        if (!specular_only && !scrolling_specular_only && !camera_overlay_only && !masked_detail_only && !scrolling_masked_detail_only && !scrolling_camera_only && !animation_only && !effects_only)
        {
        for (auto format : {GLTargetFormat_RGBA8, GLTargetFormat_RGB565, GLTargetFormat_RGB5A3, GLTargetFormat_A8, GLTargetFormat_IA8})
        {
            GLTargetInfo info;
            info.width = GXNtsc480IntDf.fbWidth; info.height = GXNtsc480IntDf.efbHeight;
            info.format = format; info.clearFlags = 7;
            // Reuse the same name/address with different formats: stale copy-cache state must be evicted.
            auto target = glCreateTarget("pixel/copy", &info);
            auto producer = Model(901, 0x21db4385, 850);
            auto consumer = Model(902, 0x21db4385, glGetTargetTexture(target));
            for (auto& vertex : consumer.packets[0].vertices) vertex.uv = {.5f,.5f};
            const bool alpha = format == GLTargetFormat_A8, intensity = format == GLTargetFormat_IA8;
            const std::array<unsigned char,4> colour = alpha ? std::array<unsigned char,4>{255,255,255,64}
                : intensity ? std::array<unsigned char,4>{255,255,255,255}
                : format == GLTargetFormat_RGB565 ? std::array<unsigned char,4>{0,255,0,255}
                : format == GLTargetFormat_RGB5A3 ? std::array<unsigned char,4>{0,0,255,255}
                : std::array<unsigned char,4>{255,0,0,255};
            StaticInventory copied(*glGetCurrentResourcePool(), {producer, consumer},
                {Texture(850, colour)}, Drain);
            // Observe copied channels directly, independently of material alpha compositing/discard.
            glSetRasterState(copied.Model(902)->packets[0].rasterState, GLS_AlphaBlend, 0);
            glSetRasterState(copied.Model(902)->packets[0].rasterState, GLS_AlphaTest, 0);
            PixelCase(("New target contains no previous GPU copy " + std::to_string(format)).c_str(),
                *copied.Model(902), 0, {0,0,0});
            PixelCase(("Target copy/sample format " + std::to_string(format)).c_str(), *copied.Model(902),
                0, alpha ? std::array<unsigned char,3>{64,64,64}
                         : intensity ? std::array<unsigned char,3>{235,235,235}
                                     : std::array<unsigned char,3>{colour[0],colour[1],colour[2]},
                {}, nullptr, nullptr, copied.Model(901), target);
            copied.Release();
            glDestroyTarget(&target);
        }
        {
            GLTargetInfo info;
            info.width = GXNtsc480IntDf.fbWidth; info.height = GXNtsc480IntDf.efbHeight;
            info.format = GLTargetFormat_A8; info.clearFlags = 7;
            auto first = glCreateTarget("pixel/first-alpha", &info);
            auto second = glCreateTarget("pixel/cleared-alpha", &info);
            auto producer = Model(903, 0x21db4385, 851);
            auto consumer = Model(904, 0x21db4385, glGetTargetTexture(second));
            for (auto& vertex : consumer.packets[0].vertices) vertex.uv = {.5f,.5f};
            StaticInventory copied(*glGetCurrentResourcePool(), {producer, consumer}, {Texture(851,{255,0,0,255})}, Drain);
            glSetRasterState(copied.Model(904)->packets[0].rasterState, GLS_AlphaBlend, 0);
            glSetRasterState(copied.Model(904)->packets[0].rasterState, GLS_AlphaTest, 0);
            // The second view copies the EFB without drawing: distinguish retain, full clear and alpha-only clear.
            PixelCase("Copy mode 8 retains EFB alpha", *copied.Model(904), 0, {255,255,255}, {}, nullptr, nullptr, copied.Model(903), first, 8, second);
            PixelCase("Copy mode 9 clears EFB alpha", *copied.Model(904), 0, {0,0,0}, {}, nullptr, nullptr, copied.Model(903), first, 9, second);
            PixelCase("Copy mode 10 clears EFB alpha", *copied.Model(904), 0, {0,0,0}, {}, nullptr, nullptr, copied.Model(903), first, 10, second);
            copied.Release(); glDestroyTarget(&second); glDestroyTarget(&first);
        }
        }
        views.Release();
        inventory.Release();
        programs.Release();
        glShutdownMemory();
        Check(StandardAllocator.TotalFreeMemory() == free1 && VirtualAllocator.TotalFreeMemory() == free2,
              "Material shutdown leaked original arenas");
        Check(errors == 0, "Material GPU backend errors");
        std::cout << "Original material pipeline pixel and lifetime checks passed\n";
        return 0;
    }
    catch (const std::exception &error)
    {
        std::cerr << "FAILED: " << error.what() << '\n';
        return 1;
    }
}
