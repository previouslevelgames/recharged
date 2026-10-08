#include "platform/graphics_stats.h"
#include "frontend_font_fixture.h"
#include "frontend_layout_fixture.h"
#include "runtime/frontend_layout_gx.h"
#include "runtime/gpu_readback.h"
#include <aurora/aurora.h>
#include <aurora/event.h>
#include <aurora/gfx.h>
#include <dolphin/gx.h>
#include <dolphin/gx/GXAurora.h>
#include <dolphin/vi.h>
#include <SDL3/SDL.h>
#include <imgui.h>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <iostream>

using namespace mscharged;
namespace
{
std::atomic_uint errors = 0;
void Log(AuroraLogLevel level, const char*, const char* message, unsigned size)
{ if (level >= LOG_ERROR) ++errors; std::cerr.write(message, size); std::cerr << '\n'; }
void Check(bool good, const char* message) { if (!good) throw std::runtime_error(message); }
struct Session { bool live = false; ~Session() { if (live) { AuroraGXSync(); aurora_shutdown(); } } };
using RGB = std::array<unsigned,3>;
void Case(const resources::FrontendLayoutFrame& layout, const std::array<RGB,9>& expected)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    unsigned draws = 0, quiet = 0;
    ColourSamples samples{};
    for (unsigned frame = 0;;)
    {
        Check(std::chrono::steady_clock::now() < deadline, "Frontend frame pipeline timed out");
        for (const auto* event = aurora_update(); event->type != AURORA_NONE; ++event)
            Check(event->type != AURORA_EXIT, "Frontend test window closed");
        if (!aurora_begin_frame()) { SDL_Delay(1); continue; }
        GXSetPixelFmt(GX_PF_RGB8_Z24, GX_ZC_LINEAR);
        GXSetCopyClear({20,24,30,255}, GX_MAX_Z24);
        DrawFrontendLayout(layout, 640, 480);
        GXDrawDone();
        const bool sample = frame >= 12 && quiet >= 2;
        if (sample) samples = EndFrameAndReadColours(); else aurora_end_frame();
        draws += aurora_get_stats()->drawCallCount; ++frame;
        const auto pending = mscharged::platform::GetQueuedPipelineCount();
        quiet = pending ? 0 : quiet + 1;
        if (sample) break;
        SDL_Delay(1);
    }
    Check(layout.entries.empty() || draws > 0, "Frontend frame emitted no GPU draws");
    for (unsigned pixel = 0; pixel < samples.size(); ++pixel)
    {
        std::cout << "Pixel " << pixel << ": " << unsigned(samples[pixel][0]) << ','
            << unsigned(samples[pixel][1]) << ',' << unsigned(samples[pixel][2]) << '\n';
        for (unsigned channel = 0; channel < 3; ++channel)
            Check(std::abs(int(samples[pixel][channel]) - int(expected[pixel][channel])) <= 3,
                "Authored frontend placement, visibility, order or colour mismatch");
    }
}
}
int main(int argc, char** argv)
{
    try
    {
        const auto directory = std::filesystem::path(SDL_GetBasePath()) / "frontend-frame-test-data";
        std::filesystem::create_directories(directory); const auto path = directory.string();
        AuroraConfig config{}; config.appName = "Charged authored frontend frame checks";
        config.userPath = config.cachePath = path.c_str(); config.resourcesPath = SDL_GetBasePath();
        config.desiredBackend = BACKEND_VULKAN; config.enableBackendValidation = true;
        config.windowWidth = 640; config.windowHeight = 480; config.windowPosX = config.windowPosY = -1;
        config.logLevel = LOG_WARNING; config.logCallback = Log; config.vsync = true;
        Session session; const auto info = aurora_initialize(argc, argv, &config); session.live = true;
        Check(info.window && info.backend == BACKEND_VULKAN, "Frontend frame pipeline requires Vulkan");
        ImGui::GetIO().IniFilename = nullptr; ImGui::GetIO().LogFilename = nullptr;
        VIInit(); VIConfigure(&GXNtsc480IntDf);
        alignas(32) std::array<std::uint8_t,65536> fifo{}; GXInit(fifo.data(), fifo.size());
        AuroraSetViewportPolicy(AURORA_VIEWPORT_FIT);
        const auto font = resources::ReadFrontendFont(font_fixture::Font(), "fe/fonts/fixture", "test");
        const std::array fonts{font};
        auto graph = frontend_layout_fixture::Scene(font->alias);
        resources::Localization localization;
        auto& parent = graph.instances[0];
        // Parent translation is deliberately large enough that ignoring it
        // misses all three quarter-grid samples. Inherited red attenuation
        // additionally distinguishes parent state from per-instance colour.
        parent.overload_flags = 1 | 16;
        parent.attributes.position = {50,-30,0};
        parent.attributes.colour = {128,255,255,255};
        for (unsigned i = 0; i < 3; ++i)
        {
            auto& label = graph.instances[i+1];
            label.text = u"A";
            label.attributes.position = {float(-215+160*int(i)),float(153-120*int(i)),0};
            label.attributes.colour = {0,0,0,255}; label.attributes.colour[i] = 255;
        }
        // Last submitted yellow overlaps green. Anark reverses submissions,
        // so green must cover yellow regardless of the stored priority values.
        auto overlap = graph.instances[2]; overlap.offset = 600; overlap.priority = 65535;
        overlap.attributes.colour = {255,255,0,255};
        graph.instances[0].children.push_back(overlap.offset); graph.instances.push_back(overlap);
        auto hidden = graph.instances[1]; hidden.offset = 700; hidden.visible = false;
        hidden.attributes.position = {105,153,0};
        graph.instances[0].children.push_back(hidden.offset); graph.instances.push_back(hidden);
        constexpr RGB background{20,24,30};
        const auto layout = resources::BuildFrontendLayout(graph, localization, fonts);
        Check(layout.TextCount() == 4 && layout.hidden == 1 && layout.unavailable.empty(), "Unexpected fixture selection");
        Case(layout, {{{128,0,0},background,background,background,{0,255,0},background,background,background,{0,0,255}}});
        // Hiding the green component exposes the previously occluded yellow
        // component. Hiding the parent suppresses its complete subtree.
        graph.instances[2].visible = false;
        Case(resources::BuildFrontendLayout(graph, localization, fonts),
            {{{128,0,0},background,background,background,{128,255,0},background,background,background,{0,0,255}}});
        graph.instances[0].visible = false;
        Case(resources::BuildFrontendLayout(graph, localization, fonts),
            {{background,background,background,background,background,background,background,background,background}});
        AuroraGXSync(); Check(errors == 0, "Aurora reported frontend rendering errors");
        std::cout << "Authored frontend placement, hierarchy, visibility and draw order checks passed\n";
    }
    catch (const std::exception& e) { std::cerr << "FAILED: " << e.what() << '\n'; return 1; }
}
