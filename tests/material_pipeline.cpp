// Independent synthetic pixel expectations for the original material TEV recipes.
#include "runtime/materials.h"
#include "runtime/gpu_readback.h"
#include "runtime/material_environment.h"
#include "runtime/static_inventory.h"
#include "runtime/graphics_memory.h"
#include "runtime/graphics_state.h"
#include "runtime/startup.h"
#include "runtime/startup_files.h"
#include "Game/GraphicsMemoryStartup.h"
#include "NL/gl/glMemoryInit.h"
#include "NL/gl/glMatrix.h"
#include "NL/gl/glState.h"
#include "NL/glx/GXMaskedSpecularFresnelMaterialProgram.h"
#include "NL/glx/GXScrollingDiffuseMaterialProgram.h"
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
#include <stdexcept>

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
resources::Texture Texture(unsigned id, std::array<unsigned char, 4> c, unsigned alpha_bits = 0, bool stripe = false)
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
        const auto pixel = stripe && i % 4 >= 2 ? std::array<unsigned char, 4>{20, 40, 200, 255} : c;
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
               const GameLighting& lighting = {}, const nlMatrix4* world = nullptr, const nlMatrix4* view = nullptr)
{
    unsigned draws = 0;
    std::array<unsigned char, 3> pixel{};
    nlMatrix4 identity;
    identity.SetIdentity();
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
        Mtx44 projection;
        C_MTXOrtho(projection, 1, -1, -1, 1, 0, 1);
        Mtx matrix = {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}};
        GXSetCopyClear({20, 24, 30, 255}, GX_MAX_Z24);
        GXSetViewport(0, 0, GXNtsc480IntDf.fbWidth, GXNtsc480IntDf.efbHeight, 0, 1);
        GXSetScissor(0, 0, GXNtsc480IntDf.fbWidth, GXNtsc480IntDf.efbHeight);
        GXSetProjection(projection, GX_ORTHOGRAPHIC);
        GXLoadPosMtxImm(matrix, GX_PNMTX0);
        GXSetCurrentMtx(GX_PNMTX0);
        {
            MaterialPreviewScope environment(view ? *view : identity, time, lighting);
            for (unsigned i = 0; i < model.numPackets; ++i)
                DrawMaterial(model.packets[i]);
        }
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
        StaticInventory inventory(*glGetCurrentResourcePool(),
                                  {unlit, vertex, scroll, masked, discard, blend, ci8, normal_left, normal_right},
                                  textures, Drain);
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
        lighting.lights[0].enabled = true;
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
        lighting.lights[0].unknown01 = 1;
        lighting.lights[0].colour = {{255,128,0,255}};
        PixelCase("Coloured directional light", *inventory.Model(4), 0, {80,50,0}, lighting);
        lighting.lights[0].unknown01 = 0;
        nlMatrix4 rotated, camera;
        nlMakeRotationMatrixY(rotated, 3.1415927f / 3);
        rotated.SetTranslation({.2598076f,0,-.15f}); // Keep the tilted triangle centred at z=-.3.
        PixelCase("Directional light follows transformed normals", *inventory.Model(4), 0, {40,50,60}, lighting, &rotated);
        nlMakeRotationMatrixY(rotated, 3.1415927f / 3);
        nlMakeRotationMatrixY(camera, -3.1415927f / 3);
        PixelCase("Directional light follows changed view", *inventory.Model(4), 0, {40,50,60}, lighting, &rotated, &camera);
        lighting.lights[0].unknown02 = 1;
        lighting.lights[0].worldPosition.z = .7f;
        lighting.lights[0].unknown20 = 3;
        // Independent vertex diffuse/steep-attenuation values, interpolated at
        // the centre (weights 1/4, 1/4, 1/2): approximately .488 and .131.
        PixelCase("Point light near", *inventory.Model(4), 0, {39,49,59}, lighting);
        lighting.lights[0].worldPosition.z = 4.7f;
        PixelCase("Point light distance attenuation", *inventory.Model(4), 0, {10,13,16}, lighting);
        lighting.lights[0].unknown02 = 0;
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
