#include "frontend_packets_fixture.h"
#include "runtime/frontend_layout_gx.h"
#include "runtime/frontend_pointer_display.h"
#include "runtime/gpu_readback.h"
#include <aurora/event.h>
#include <aurora/gfx.h>
#include <aurora/gfx.hpp>
#include <dolphin/gx.h>
#include <dolphin/gx/GXAurora.h>
#include <dolphin/vi.h>
#include <SDL3/SDL.h>
#include <imgui.h>
#include <atomic>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <iostream>

using namespace mscharged;
namespace
{
unsigned checks = 0;
bool live_input = false;
float sample_x = 0, sample_y = 0;
std::uint64_t sample_sequence = 0;
std::atomic_uint errors = 0;
void Check(bool value, const char* message)
{ ++checks; if (!value) throw std::runtime_error(message); }
template<class F> void Reject(F action)
{ ++checks; try { action(); } catch (const std::exception&) { return; } throw std::runtime_error("Invalid displayed pointer operation accepted"); }
void Log(AuroraLogLevel level, const char*, const char* message, unsigned length)
{ if (level >= LOG_ERROR) ++errors; std::cerr.write(message, length); std::cerr << '\n'; }
void Pump()
{
    for (const auto* event = aurora_update(); event->type != AURORA_NONE; ++event)
        Check(event->type != AURORA_EXIT, "Pointer test window closed");
}
struct Session
{
    bool live = false;
    ~Session() { if (live) { AuroraGXSync(); aurora::gfx::synchronize(); aurora_shutdown(); } }
};
std::shared_ptr<FrontendSessionFrame> Frame()
{
    auto frame = frontend_packet_fixture::Frame();
    frame->layout.entries.erase(frame->layout.entries.begin());
    auto& image = std::get<resources::FrontendLayoutImage>(frame->layout.entries.front()); image.instance = 100;
    resources::FrontendLibraryObject library{}; library.offset = 10; library.type = 1;
    library.attributes.scale = {.4f, .4f, 1}; frame->graph.library.push_back(library);
    resources::FrontendInstance instance{}; instance.offset = 100; instance.type = 2; instance.library = 10;
    instance.name = "Button"; frame->graph.instances.push_back(instance);
    return frame;
}
AuroraPresentation Draw(const FrontendSession::Handle& frame, unsigned pixel, bool present = true)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    unsigned count = 0, quiet = 0;
    for (;;)
    {
        Check(std::chrono::steady_clock::now() < deadline, "Displayed pointer render timed out"); Pump();
        if (!aurora_begin_frame()) { SDL_Delay(1); continue; }
        GXSetPixelFmt(GX_PF_RGB8_Z24, GX_ZC_LINEAR); GXSetCopyClear({20,24,30,255}, GX_MAX_Z24);
        DrawFrontendLayout(frame->layout, 640, 480); GXDrawDone();
        const auto pending = std::atomic_ref<const std::uint32_t>(aurora_get_stats()->queuedPipelines).load();
        quiet = pending ? 0 : quiet + 1;
        if (++count < 16 || quiet < 2) { if (present) aurora_end_frame(); else aurora_end_frame_no_present(); }
        else
        {
            const auto pixels = EndFrameAndReadColours([&] {
                if (present) aurora_end_frame(); else aurora_end_frame_no_present();
            });
            AuroraGXSync(); aurora::gfx::synchronize();
            Check(pixels[pixel][0] < 4 && pixels[pixel][1] < 4 && pixels[pixel][2] > 250,
                "Actual displayed component does not occupy its independently expected EFB point");
            return aurora_get_last_presentation();
        }
        SDL_Delay(1);
    }
}
void Input(FrontendInput& input, bool pressed = false)
{
    std::array<FrontendPadSample,4> pads{}; pads[0].connected = true;
    pads[0].buttons = pressed ? 0x100 : 0; input.Update(pads, 1.f/60);
}
void Warp(SDL_Window* window, const AuroraPresentation& present, float asset_x)
{
    const float x = (present.x + (asset_x/640.f + .5f)*present.width)
        * present.size.width / present.size.native_fb_width;
    const float y = (present.y + .5f*present.height) * present.size.height / present.size.native_fb_height;
    sample_x = x; sample_y = y;
    if (live_input) SDL_WarpMouseInWindow(window, x, y);
    Pump();
}
FrontendPointerDispatch Sample(FrontendPointerDisplay& pointer, SDL_Window* window, bool capture = false)
{
    if (live_input) return pointer.Poll(window, capture);
    const auto& v = pointer.Current()->Viewport();
    // Deterministic input-provider fixture; physical SDL focus is qualified
    // separately with --live-input. Window and rectangle are real GPU metadata.
    FrontendPointerDesktopSample sample{v.window,v.window_width,v.window_height,v.pixel_width,v.pixel_height,
        ++sample_sequence,1,sample_x,sample_y,true,true,capture};
    return pointer.Route(sample);
}
}
int main(int argc, char** argv)
{
    try
    {
        if (argc == 2 && std::string_view(argv[1]) == "--live-input") live_input = true;
        else Check(argc == 1, "Usage: frontend_pointer_display_pipeline_tests [--live-input]");
        Check(!aurora_get_last_presentation().sequence, "Presentation exists before initialization");
        const auto directory = std::filesystem::path(SDL_GetBasePath()) / "frontend-pointer-display-test-data";
        std::filesystem::create_directories(directory); const auto path = directory.string();
        AuroraConfig config{}; config.appName = "Charged displayed frontend pointer checks";
        config.userPath = config.cachePath = path.c_str(); config.resourcesPath = SDL_GetBasePath();
        config.desiredBackend = BACKEND_VULKAN; config.enableBackendValidation = true;
        config.windowWidth = 960; config.windowHeight = 720; config.windowPosX = config.windowPosY = -1;
        config.logLevel = LOG_WARNING; config.logCallback = Log; config.vsync = true;
        Session session; const auto info = aurora_initialize(argc, argv, &config); session.live = true;
        std::cout << "Pointer display stage: initialized " << SDL_GetCurrentVideoDriver() << std::endl;
        Check(info.window && info.backend == BACKEND_VULKAN, "Displayed pointer requires Vulkan");
        Check(!aurora_get_last_presentation().sequence, "Initialization invented a presentation");
        ImGui::GetIO().IniFilename = nullptr; ImGui::GetIO().LogFilename = nullptr;
        VIInit(); VIConfigure(&GXNtsc480IntDf); alignas(32) std::array<std::uint8_t,65536> fifo{};
        GXInit(fifo.data(), fifo.size()); AuroraSetViewportPolicy(AURORA_VIEWPORT_FIT);
        auto first = Frame(); unsigned enters = 0, presses = 0;
        FrontendSession::Handle observed;
        {
            FrontendInput input; Input(input);
            FrontendPointerDisplay pointer(input, [&](auto event, unsigned index, const auto& frame) {
                Check(index == 0, "Displayed pointer callback changed index"); observed = frame;
                if (event == FrontendPointerCallback::Enter) ++enters;
                if (event == FrontendPointerCallback::Press) ++presses;
            });
            Reject([&] { pointer.Acknowledge({}, first, {100}); });
            auto present = Draw(first, 4); Check(present.sequence && present.window_id == SDL_GetWindowID(info.window), "Successful present has no identity");
            std::cout << "Pointer display stage: first frame presented" << std::endl;
            pointer.Acknowledge(present, first, {100});
            Check(pointer.Current()->Frame() == first, "Displayed pointer retained a different frame");
            if (live_input) Check(SDL_RaiseWindow(info.window), "Cannot request pointer test window focus");
            std::cout << "Pointer display stage: raised" << std::endl;
            Warp(info.window, present, 0);
            std::cout << "Pointer display stage: warped" << std::endl;
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            FrontendPointerDispatch result;
            do
            {
                Check(std::chrono::steady_clock::now() < deadline, "SDL window never acquired actual mouse/input focus");
                Pump(); Input(input); result = Sample(pointer, info.window); SDL_Delay(5);
            } while (!result.active);
            std::cout << "Pointer display stage: " << (live_input ? "real SDL pointer" : "fixture snapshot") << " active" << std::endl;
            Check(std::abs(result.event.position[0]) < 1 && std::abs(result.event.position[1]) < 1,
                "Actual SDL mouse does not map to the displayed component center");
            Check(enters == 1 && observed == first, "Original pointer Enter lacks exact displayed frame");
            Input(input, true); result = Sample(pointer, info.window);
            Check(result.event.pressed && presses == 1, "Original action30 did not press displayed component");
            Sample(pointer, info.window); Check(presses == 1, "Repeated sampling duplicated action30");
            Check(!Sample(pointer, info.window, true).active, "UI mouse capture admitted a component press");
            Check(!Sample(pointer, info.window).active, "Held input bypassed capture neutral gate");
            Input(input); Check(!Sample(pointer, info.window).active, "Neutral capture observation activated immediately");
            Check(Sample(pointer, info.window).active, "Capture neutral gate did not reopen");
            Input(input, true); Sample(pointer, info.window); Check(presses == 2, "Fresh press after capture failed"); Input(input);
            const auto discarded = Draw(first, 4, false);
            std::cout << "Pointer display stage: discarded frames complete" << std::endl;
            Check(discarded.sequence == present.sequence, "Discarded frames advanced successful presentation");
            Reject([&] { pointer.Acknowledge(discarded, first, {100}); });
            Check(pointer.Current()->Frame() == first, "Rejected discarded presentation changed pointer frame");
            Check(SDL_SetWindowSize(info.window, 1100, 700), "Cannot resize displayed pointer window"); Pump();
            Check(!pointer.Poll(info.window).active, "Resized stale presentation still accepted input");
            auto shifted = std::make_shared<FrontendSessionFrame>(*first);
            shifted->graph.instances.front().attributes.position[0] = 160;
            auto& image = std::get<resources::FrontendLayoutImage>(shifted->layout.entries.front()); image.transform[12] = 480;
            present = Draw(shifted, 5);
            std::cout << "Pointer display stage: resized frame presented" << std::endl;
            Check(present.x > 0 && present.width < present.size.native_fb_width,
                "Actual resized presentation did not expose its pillarbox");
            pointer.Acknowledge(present, shifted, {100}); Warp(info.window, present, 160); Input(input);
            Check(!Sample(pointer, info.window).active, "Resize did not require neutral pointer input");
            result = Sample(pointer, info.window);
            Check(result.active && std::abs(result.event.position[0]-160) < 1 && std::abs(result.event.position[1]) < 1,
                "SDL pointer missed the actual shifted/pillarboxed rendered component");
            Input(input, true); Sample(pointer, info.window);
            Check(presses == 3 && observed == shifted && observed != first, "New displayed generation routed an old frame");
            pointer.Release(); Reject([&] { pointer.Poll(info.window); }); observed.reset();
        }
        first.reset(); AuroraGXSync(); aurora::gfx::synchronize(); aurora_shutdown(); session.live = false;
        Check(!aurora_get_last_presentation().sequence, "Shutdown retained presentation identity");
        Check(errors == 0, "Aurora reported displayed pointer errors");
        std::cout << checks << " displayed pointer checks passed: real Vulkan pixels, "
            << (live_input ? "actual SDL focus/cursor" : "explicit fixture input snapshots")
            << ", original action30, capture/resize/discard and exact frames\n";
    }
    catch (const std::exception& e) { std::cerr << "FAILED: " << e.what() << '\n'; return 1; }
}
