// Independent native GX check with generated geometry and tiled RGBA8 pixels.
#include "mscharged/build_version.h"
#include "platform/path.h"
#include "platform/graphics_backend.h"
#include <aurora/aurora.h>
#include <aurora/event.h>
#include <aurora/gfx.h>
#include <dolphin/gx.h>
#include <dolphin/gx/GXAurora.h>
#include <dolphin/mtx.h>
#include <dolphin/os.h>
#include <dolphin/vi.h>
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#ifdef WEBGPU_DAWN
#include <dawn/native/DawnNative.h>
#include <webgpu/gpu.hpp>
#endif
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace
{
std::atomic_uint errors{0};
void Log(AuroraLogLevel level, const char* module, const char* message, unsigned length)
{
    if (level >= LOG_ERROR) ++errors;
    std::cerr << '[' << module << "] " << std::string_view(message, length) << '\n';
}
struct Session
{
    bool live = false;
    ~Session() { if (live) aurora_shutdown(); }
};
alignas(32) std::array<std::uint8_t, 65536> fifo{};
alignas(32) std::array<std::uint8_t, 16*16*4> pixels{};

void MakeTexture()
{
    // GX RGBA8 stores each 4x4 tile's AR plane followed by its GB plane.
    std::size_t block = 0;
    for (unsigned by = 0; by < 16; by += 4)
        for (unsigned bx = 0; bx < 16; bx += 4, block += 64)
            for (unsigned y = 0; y < 4; ++y)
                for (unsigned x = 0; x < 4; ++x)
                {
                    const auto offset = block + 2*(4*y+x);
                    const bool bright = ((bx/4+by/4)%2) == 0;
                    pixels[offset] = 255;
                    pixels[offset+1] = bright ? 250 : 25;
                    pixels[offset+32] = bright ? 200 : 90;
                    pixels[offset+33] = bright ? 30 : 180;
                }
}

void Vertex(float x, float y, float z, unsigned char r, unsigned char g, unsigned char b)
{
    GXPosition3f32(x, y, z);
    GXColor4u8(r, g, b, 255);
}

void Draw(GXTexObj& texture, unsigned width, unsigned height)
{
    GXSetCopyClear({20, 24, 30, 255}, GX_MAX_Z24);
    GXSetViewport(0, 0, width, height, 0, 1);
    GXSetScissor(0, 0, width, height);
    Mtx44 projection{};
    C_MTXOrtho(projection, 1, -1, -1, 1, 0, 1);
    Mtx model = {{1,0,0,0}, {0,1,0,0}, {0,0,1,0}};
    GXSetProjection(projection, GX_ORTHOGRAPHIC);
    GXLoadPosMtxImm(model, GX_PNMTX0);
    GXSetCurrentMtx(GX_PNMTX0);
    GXSetCullMode(GX_CULL_NONE);
    GXSetZMode(GX_TRUE, GX_LEQUAL, GX_TRUE);
    GXSetBlendMode(GX_BM_NONE, GX_BL_ONE, GX_BL_ZERO, GX_LO_CLEAR);
    GXSetAlphaCompare(GX_ALWAYS, 0, GX_AOP_AND, GX_ALWAYS, 0);
    GXSetColorUpdate(GX_TRUE);
    GXSetAlphaUpdate(GX_TRUE);
    GXSetNumChans(1);
    GXSetChanCtrl(GX_COLOR0A0, GX_DISABLE, GX_SRC_REG, GX_SRC_VTX, GX_LIGHT_NULL, GX_DF_NONE, GX_AF_NONE);
    GXSetNumTevStages(1);
    GXSetNumTexGens(0);
    GXSetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD_NULL, GX_TEXMAP_NULL, GX_COLOR0A0);
    GXSetTevOp(GX_TEVSTAGE0, GX_PASSCLR);
    GXClearVtxDesc();
    GXSetVtxDesc(GX_VA_POS, GX_DIRECT);
    GXSetVtxDesc(GX_VA_CLR0, GX_DIRECT);
    GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
    GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0);
    GXBegin(GX_TRIANGLES, GX_VTXFMT0, 3);
    Vertex(-0.9f,-0.75f,-0.3f, 240,65,50);
    Vertex(-0.35f,0.75f,-0.3f, 50,220,100);
    Vertex(0.2f,-0.75f,-0.3f, 60,100,245);
    GXEnd();

    GXSetNumTexGens(1);
    GXSetTexCoordGen(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_IDENTITY);
    GXSetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR0A0);
    GXSetTevOp(GX_TEVSTAGE0, GX_MODULATE);
    GXLoadTexObj(&texture, GX_TEXMAP0);
    GXSetVtxDesc(GX_VA_TEX0, GX_DIRECT);
    GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_TEX0, GX_TEX_ST, GX_F32, 0);
    GXBegin(GX_QUADS, GX_VTXFMT0, 4);
    Vertex(0.3f,-0.65f,-0.4f,255,255,255); GXTexCoord2f32(0,1);
    Vertex(0.9f,-0.65f,-0.4f,255,255,255); GXTexCoord2f32(1,1);
    Vertex(0.9f, 0.65f,-0.4f,255,255,255); GXTexCoord2f32(1,0);
    Vertex(0.3f, 0.65f,-0.4f,255,255,255); GXTexCoord2f32(0,0);
    GXEnd();
    GXDrawDone();
}
}

int main(int argc, char** argv)
{
    try
    {
        unsigned frames = 180;
        bool interactive = false, resize = false, optimized_device = false;
        for (int i = 1; i < argc; ++i)
        {
            const std::string arg = argv[i];
            if (arg == "--window") interactive = true;
            else if (arg == "--resize-test") resize = true;
            else if (arg == "--optimized-device") optimized_device = true;
            else if (arg == "--frames" && i+1 < argc)
            {
                std::size_t used = 0;
                const auto value = std::stoul(argv[++i], &used);
                if (used != std::string(argv[i]).size() || value == 0 || value > 10000)
                    throw std::runtime_error("Frame count must be between 1 and 10000");
                frames = value;
            }
            else if (arg == "--version") { std::cout << "mscharged-gx-check " << mscharged::build::version << '\n'; return 0; }
            else if (arg == "--help") { std::cout << "mscharged-gx-check [--frames N] [--resize-test] [--window] [--optimized-device]\n"; return 0; }
            else throw std::runtime_error("Unknown or incomplete argument: " + arg);
        }
        const char* base = SDL_GetBasePath();
        if (!base) throw std::runtime_error("Cannot locate the executable directory");
        const auto directory = mscharged::PathFromUtf8(base) / "gx-check-data";
        std::filesystem::create_directories(directory);
        const auto path = mscharged::PathUtf8(directory);
        AuroraConfig config{};
        config.appName = "Mario Strikers Charged | native GX check";
        config.userPath = config.cachePath = path.c_str();
        config.resourcesPath = base;
        config.desiredBackend = mscharged::platform::NativeGraphicsBackend;
        config.windowWidth = 800; config.windowHeight = 600;
        config.windowPosX = config.windowPosY = -1;
        config.logLevel = LOG_INFO; config.logCallback = Log;
        config.vsync = true; config.enableBackendValidation = !optimized_device;
        config.mem1Size = MEM1_DEFAULT_SIZE;
        Session session;
        const auto info = aurora_initialize(argc, argv, &config);
        session.live = true;
        if (!info.window || info.backend != mscharged::platform::NativeGraphicsBackend)
            throw std::runtime_error(std::string("Requested ") + mscharged::platform::NativeGraphicsBackendName
                + " is unavailable; the diagnostic cannot pass on a fallback backend");
#ifdef WEBGPU_DAWN
        // Exercise the actual requested device in both debug-layer modes.
        // Disabling backend debug layers must retain WebGPU validation and
        // robust buffer/shader access in Release as well as Debug builds.
        for (const char* toggle : dawn::native::GetTogglesUsed(aurora::webgpu::g_device.Get())) {
            const std::string_view name(toggle);
            if (name == "skip_validation" || name == "disable_robustness")
                throw std::runtime_error("Unsafe graphics device policy: " + std::string(name));
        }
#endif
        OSInit();
        VIInit();
        VIConfigure(&GXNtsc480IntDf);
        GXInit(fifo.data(), fifo.size());
        AuroraSetViewportPolicy(AURORA_VIEWPORT_FIT);
        MakeTexture();
        GXTexObj texture{};
        GXInitTexObj(&texture, pixels.data(), 16, 16, GX_TF_RGBA8, GX_CLAMP, GX_CLAMP, GX_FALSE);
        GXInitTexObjLOD(&texture, GX_NEAR, GX_NEAR, 0, 0, 0, GX_FALSE, GX_FALSE, GX_ANISO_1);
        unsigned rendered = 0, draws = 0, resize_events = 0;
        bool resized = false;
        std::uint32_t depth = 0;
        bool exiting = false;
        const auto start = std::chrono::steady_clock::now();
        while (!exiting && (interactive || rendered < frames))
        {
            for (const auto* event = aurora_update(); event->type != AURORA_NONE; ++event)
            {
                if (event->type == AURORA_EXIT) exiting = true;
                if (event->type == AURORA_WINDOW_RESIZED) ++resize_events;
            }
            if (exiting) break;
            if (!interactive && std::chrono::steady_clock::now()-start > std::chrono::seconds(30))
                throw std::runtime_error("Timed out waiting for the native GX diagnostic frames");
            if (!aurora_begin_frame()) { SDL_Delay(1); continue; }
            Draw(texture, GXNtsc480IntDf.fbWidth, GXNtsc480IntDf.efbHeight);
            // GXPeekZ uses the configured console EFB coordinates even when
            // native rendering scales that framebuffer to a larger window.
            GXPeekZ(GXNtsc480IntDf.fbWidth/3, GXNtsc480IntDf.efbHeight/2, &depth);
            aurora_end_frame();
            draws += aurora_get_stats()->drawCallCount;
            ++rendered;
            if (resize && rendered == 60 && !SDL_SetWindowSize(info.window, 960, 640))
                throw std::runtime_error(std::string("Window resize failed: ") + SDL_GetError());
            if (resize && rendered > 60)
                resized = resized || (aurora_get_window_size().width == 960 && aurora_get_window_size().height == 640);
        }
        aurora_shutdown();
        session.live = false;
        constexpr std::uint32_t expected_depth = static_cast<std::uint32_t>(0.3*GX_MAX_Z24+0.5);
        if (errors || !draws || depth < expected_depth-32 || depth > expected_depth+32 || (!interactive && rendered < frames)
            || (resize && (!resize_events || !resized)))
            throw std::runtime_error("GX check incomplete: frames=" + std::to_string(rendered)
                + ", draws=" + std::to_string(draws) + ", depth=" + std::to_string(depth)
                + ", resize=" + std::to_string(resized) + ", errors=" + std::to_string(errors.load()));
        std::cout << mscharged::platform::NativeGraphicsBackendName << " GX check passed: " << rendered << " frames, " << draws
                  << " draw calls, geometry depth " << depth << ", resize events " << resize_events << '\n';
        return 0;
    }
    catch (const std::exception& error) { std::cerr << "GX check failed: " << error.what() << '\n'; return 1; }
}
