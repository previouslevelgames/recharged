#include "frontend_font_fixture.h"
#include "runtime/frontend_text_gx.h"
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
void Case(const resources::FontLayout& layout, std::array<std::uint8_t, 4> modulation,
          std::array<unsigned, 3> expected, float x = 315)
{
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    unsigned draws = 0, quiet_frames = 0; ColourSamples samples{};
    for (unsigned frame = 0;;)
    {
        Check(std::chrono::steady_clock::now() < end, "Font pipeline timed out");
        for (const auto* event = aurora_update(); event->type != AURORA_NONE; ++event)
            Check(event->type != AURORA_EXIT, "Font test window closed");
        if (!aurora_begin_frame()) { SDL_Delay(1); continue; }
        GXSetPixelFmt(GX_PF_RGB8_Z24, GX_ZC_LINEAR); GXSetCopyClear({20, 24, 30, 255}, GX_MAX_Z24);
        DrawFrontendText(layout, x, 237, 640, 480, modulation);
        GXDrawDone();
        // Aurora compiles newly encountered pipelines asynchronously. Wait for
        // its queue to drain, then sample a subsequent frame; never accept a
        // cached run as evidence that the first run rendered successfully.
        const bool sample = frame >= 12 && quiet_frames >= 2;
        if (sample) samples = EndFrameAndReadColours(); else aurora_end_frame();
        draws += aurora_get_stats()->drawCallCount; ++frame;
        const auto pending = std::atomic_ref<const std::uint32_t>(aurora_get_stats()->queuedPipelines).load();
        quiet_frames = pending ? 0 : quiet_frames + 1;
        if (sample) break;
        SDL_Delay(1);
    }
    Check(draws > 0, "Font renderer emitted no GPU draws");
    std::cout << "Sample RGB " << unsigned(samples[4][0]) << ',' << unsigned(samples[4][1]) << ','
        << unsigned(samples[4][2]) << " background " << unsigned(samples[0][0]) << ',' << unsigned(samples[0][1]) << ','
        << unsigned(samples[0][2]) << '\n';
    for (unsigned channel = 0; channel < 3; ++channel)
    {
        Check(std::abs(int(samples[4][channel]) - int(expected[channel])) <= 3, "Font CI8 page colour/modulation mismatch");
        Check(std::abs(int(samples[0][channel]) - int(std::array{20, 24, 30}[channel])) <= 3, "Font renderer changed background pixels");
    }
    std::cout << "Font page " << unsigned(layout.quads.front().page) << " RGB " << unsigned(samples[4][0]) << ','
        << unsigned(samples[4][1]) << ',' << unsigned(samples[4][2]) << "; " << draws << " draws\n";
}
}
int main(int argc, char** argv)
{
    try
    {
        const auto directory = std::filesystem::path(SDL_GetBasePath()) / "frontend-font-test-data";
        std::filesystem::create_directories(directory); const auto path = directory.string();
        AuroraConfig config{}; config.appName = "Charged original font texture checks";
        config.userPath = config.cachePath = path.c_str(); config.resourcesPath = SDL_GetBasePath();
        config.desiredBackend = BACKEND_VULKAN; config.enableBackendValidation = true;
        config.windowWidth = 640; config.windowHeight = 480; config.windowPosX = config.windowPosY = -1;
        config.logLevel = LOG_WARNING; config.logCallback = Log; config.vsync = true;
        Session session; const auto info = aurora_initialize(argc, argv, &config); session.live = true;
        Check(info.window && info.backend == BACKEND_VULKAN, "Font pipeline requires Vulkan");
        ImGui::GetIO().IniFilename = nullptr; ImGui::GetIO().LogFilename = nullptr;
        VIInit(); VIConfigure(&GXNtsc480IntDf);
        alignas(32) std::array<std::uint8_t, 65536> fifo{}; GXInit(fifo.data(), fifo.size());
        AuroraSetViewportPolicy(AURORA_VIEWPORT_FIT);
        const auto font = resources::ReadFrontendFont(font_fixture::Font(), "fe/fonts/fixture", "test");
        const auto first = resources::LayoutFrontendText(font, u"A"), second = resources::LayoutFrontendText(font, u" ");
        Case(first, {255, 255, 255, 255}, {255, 255, 255});
        Case(second, {255, 255, 255, 255}, {255, 0, 0});
        Case(first, {128, 64, 192, 255}, {128, 64, 192});
        Case(first, {255, 255, 255, 128}, {138, 140, 143});
        // Independently derived from original DrawString: A starts at -1,
        // then (advance 10 + forward kern -2) * 1.5 places B at 11.
        // B ends at 26, so the centre sample at local x=27 is background.
        // The previous integer-width pen placed B at 13 and covered it.
        auto spaced = std::make_shared<resources::FrontendFont>(*font);
        spaced->spacing = 1.5f;
        Case(resources::LayoutFrontendText(spaced, u"AB"), {255, 255, 255, 255}, {20, 24, 30}, 293);
        // Original page batching draws white B (page 0) before red A (page 1),
        // even though A appears first in the string. Their quads overlap at
        // the centre sample. String-order submission would leave it white.
        auto pages = std::make_shared<resources::FrontendFont>(*font);
        pages->glyphs.at('A').page = 1;
        pages->glyphs.at('B').offset = -10;
        Case(resources::LayoutFrontendText(pages, u"AB"), {255, 255, 255, 255}, {255, 0, 0});
        AuroraGXSync(); Check(errors == 0, "Aurora reported font rendering errors");
        std::cout << "Frontend font page, colour, alpha and background checks passed\n";
    }
    catch (const std::exception& e) { std::cerr << "FAILED: " << e.what() << '\n'; return 1; }
}
