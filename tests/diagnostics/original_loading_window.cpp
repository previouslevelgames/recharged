// Temporary source diagnostic: actual original main and LidOpenMessage own
// every loading request. This host supplies native hardware before entry.
#include <aurora/aurora.h>
#include <aurora/event.h>
#include <aurora/gfx.h>
#include <aurora/video.h>
#include <aurora/dvd.h>
#include <dolphin/gx/GXAurora.h>
#include <dolphin/os.h>
#include <dolphin/vi.h>
#include "platform/interrupt_controller.h"
#include "platform/stm_device.h"
#include "platform/system.h"
#include "platform/video_device.h"
#include "platform/video_output_device.h"
#include "gfx/scanout.hpp"
#include "gfx/xfb.hpp"
#include "webgpu/gpu.hpp"
#include <SDL3/SDL_video.h>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <dlfcn.h>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

extern "C" bool __OSInitSTM();
namespace {
unsigned checks;
std::atomic_uint errors;
void Check(bool value, const char* reason) {
    ++checks;
    if (!value) throw std::runtime_error(reason);
}
void Log(AuroraLogLevel level, const char* module, const char* message, unsigned bytes) {
    if (level >= LOG_ERROR) ++errors;
    std::fprintf(stderr, "[%s] %.*s\n", module, int(bytes), message);
}
std::vector<unsigned char> ReadSignal(unsigned& width, unsigned& height) {
    using namespace aurora::webgpu;
    const auto signal = aurora::gfx::scanout::rendered_signal();
    Check(bool(signal), "Actual source-selected VI signal texture is unavailable");
    width = signal->size.width;
    height = signal->size.height;
    Check(signal->format == wgpu::TextureFormat::RGBA8Unorm,
          "Actual native VI signal format is unsupported by this readback");
    const auto pitch = (width * 4 + 255) & ~255u;
    const wgpu::BufferDescriptor descriptor{
        .usage = wgpu::BufferUsage::CopyDst | wgpu::BufferUsage::MapRead,
        .size = std::uint64_t(pitch) * height};
    const auto buffer = g_device.CreateBuffer(&descriptor);
    const auto encoder = g_device.CreateCommandEncoder();
    const wgpu::TexelCopyTextureInfo source{.texture = signal->texture};
    const wgpu::TexelCopyBufferInfo destination{
        .layout = {.bytesPerRow = pitch, .rowsPerImage = height}, .buffer = buffer};
    encoder.CopyTextureToBuffer(&source, &destination, &signal->size);
    const auto commands = encoder.Finish();
    g_queue.Submit(1, &commands);
    struct Result { wgpu::MapAsyncStatus status = wgpu::MapAsyncStatus::Error; };
    auto result = std::make_shared<Result>();
    const auto future = buffer.MapAsync(wgpu::MapMode::Read, 0, descriptor.size,
        wgpu::CallbackMode::WaitAnyOnly,
        [result](wgpu::MapAsyncStatus status, wgpu::StringView) { result->status = status; });
    Check(g_instance.WaitAny(future, 5'000'000'000) == wgpu::WaitStatus::Success &&
              result->status == wgpu::MapAsyncStatus::Success,
          "Actual source-selected VI signal readback did not complete");
    Check(g_instance.WaitAny(g_device.GetLostFuture(), 0) == wgpu::WaitStatus::TimedOut,
          "A lost device cannot qualify original loading output");
    const auto* mapped = static_cast<const unsigned char*>(buffer.GetConstMappedRange());
    std::vector<unsigned char> rgb;
    rgb.reserve(std::size_t(width) * height * 3);
    for (unsigned y = 0; y < height; ++y) for (unsigned x = 0; x < width; ++x) {
        const auto* pixel = mapped + std::size_t(y) * pitch + x * 4;
        rgb.insert(rgb.end(), pixel, pixel + 3);
    }
    buffer.Unmap();
    return rgb;
}
}

int main(int argc, char** argv) {
    try {
        bool interactive = false;
        std::filesystem::path disc;
        for (int i = 1; i < argc; ++i) {
            const std::string argument = argv[i];
            if (argument == "--help") {
                std::puts("Usage: mscharged-original-loading-check --disc FILE [--window]\n"
                          "Run the original loading image from the original main entry.\n"
                          "Experimental USA/English diagnostic; full startup remains incomplete.\n"
                          "--window keeps the loading image visible until the window closes.");
                return 0;
            }
            if (argument == "--window") interactive = true;
            else if (argument == "--disc" && i + 1 < argc) disc = argv[++i];
            else throw std::runtime_error("Unknown or incomplete argument; use --help");
        }
        const auto executable = std::filesystem::absolute(argv[0]);
        const auto modulePath = executable.parent_path() /
            MSCHARGED_ORIGINAL_LOADING_MODULE_FILENAME;
        disc = std::filesystem::absolute(disc);
        Check(std::filesystem::is_regular_file(modulePath) &&
                  std::filesystem::is_regular_file(disc),
              "Supply the actual original-source module and an owned disc");
        const auto dataDirectory = executable.parent_path() / "original-loading-data";
        std::filesystem::create_directories(dataDirectory);
        const auto dataPath = dataDirectory.string();
        AuroraConfig config{};
        config.appName = "Mario Strikers Charged | original loading diagnostic";
        config.userPath = config.cachePath = config.resourcesPath = dataPath.c_str();
        config.windowWidth = 640; config.windowHeight = 448;
        config.windowPosX = config.windowPosY = -1;
        config.desiredBackend = BACKEND_VULKAN;
        config.enableBackendValidation = true;
        config.logLevel = LOG_INFO; config.logCallback = Log;
        config.mem1Size = MEM1_DEFAULT_SIZE;
        config.mem2Size = 64u * 1024u * 1024u;
        config.vsync = true;
        const auto host = aurora_initialize(argc, argv, &config);
        Check(host.window && host.backend == BACKEND_VULKAN,
              "An actual Vulkan window is required; no fallback acceptance");
        Check(aurora::webgpu::g_adapterInfo.adapterType != wgpu::AdapterType::CPU,
              "Original loading output requires a physical GPU");
        // Establish actual native physical backing before original VI/GX
        // commands and module constructors. No game geometry or mode rewrite.
        Check(SDL_SetWindowMinimumSize(host.window, 640, 448),
              "Native 1:1 window minimum request failed");
        Check(SDL_SetWindowSize(host.window, 640, 448),
              "Native 1:1 window resize request failed");
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        bool sized = false;
        while (std::chrono::steady_clock::now() < deadline) {
            for (const auto* event = aurora_update(); event->type != AURORA_NONE; ++event)
                Check(event->type != AURORA_EXIT, "Window closed before original entry");
            aurora::gfx::synchronize();
            const auto size = aurora_get_window_size();
            if (size.fb_width == 640 && size.fb_height == 448 &&
                size.native_fb_width == 640 && size.native_fb_height == 448) {
                sized = true; break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        Check(sized, "Original PE loading requires actual 1:1 physical640x448 backing");
        OSInit();
        Check(OSGetArenaLo() && OSGetMEM2ArenaLo(), "Actual SDK arenas are unavailable");
        mscharged::platform::InitializeNativeInterruptController();
        const mscharged::NativeSystemSettings settings{1, 0, 0, 0, 1};
        mscharged::ConfigureNativeSystemSettings(settings);
        mscharged::platform::ConfigureNativeVideoHardware(VI_TVMODE_NTSC_INT, false);
        mscharged::platform::ConfigureNativeVideoOutputHardware(settings);
        aurora_configure_native_gx_hardware();
        mscharged::platform::InitializeNativeSTMDevice();
        Check(__OSInitSTM(), "Actual original STM initialization failed");
        Check(aurora_dvd_open(disc.c_str()), "The owned Wii data partition did not mount");
        auto* module = dlopen(modulePath.c_str(), RTLD_LAZY | RTLD_LOCAL);
        if (!module) throw std::runtime_error(dlerror());
        const auto entry = reinterpret_cast<int(*)()>(dlsym(module, "charged_original_entry"));
        Check(entry, "Actual original main export is unavailable");
        std::fprintf(stderr,
            "Entering original main→Initialize→glStartup→DisplayLoadingMessageFast. "
            "Only the named loading checkpoint is enabled; full flow remains incomplete.\n");
        std::fflush(nullptr);
        const int result = entry();
        Check(result == 86, "Actual source loading did not reach diagnostic checkpoint86");
        // Observe genuine elapsed hardware fields after source final selection;
        // no host framebuffer selection, synthetic retrace or game transition.
        VIWaitForRetrace(); VIWaitForRetrace();
        aurora::gfx::synchronize();
        AuroraVIOutputState output{};
        Check(aurora_get_video_output_state(&output), "The native VI output endpoint is unavailable");
        Check(output.framebuffer && !output.black && !output.pending &&
                  output.copy_revision && output.presentations,
              "Original loading did not select a completed visible XFB");
        const auto copy = aurora::gfx::xfb::find(output.framebuffer);
        Check(copy && copy->gpuComplete.load() && copy->revision == output.copy_revision,
              "Source-selected XFB lacks an actual finished GPU copy");
        Check(copy->width == 640 && copy->height == 448 && copy->stride == 1280,
              "Actual original loading copy geometry differs from source requests");
        unsigned width = 0, height = 0;
        const auto rgb = ReadSignal(width, height);
        unsigned lit = 0;
        for (std::size_t i = 0; i < rgb.size(); i += 3)
            if (rgb[i] > 32 || rgb[i+1] > 32 || rgb[i+2] > 32) ++lit;
        Check(lit != 0, "Actual original loading VI signal is black; image remains unqualified");
        const auto image = dataDirectory / "original-loading.ppm";
        auto* file = std::fopen(image.c_str(), "wb");
        Check(file, "The actual loading signal snapshot could not be created");
        std::fprintf(file, "P6\n%u %u\n255\n", width, height);
        const auto written = std::fwrite(rgb.data(), 1, rgb.size(), file);
        const auto closed = std::fclose(file);
        Check(written == rgb.size() && !closed, "Actual loading snapshot write failed");
        Check(errors == 0, "Actual native loading hardware emitted an error");
        std::printf(
            "Original loading source gate passed: %u checks; checkpoint86, "
            "finished chosen XFB revision%llu, %llu presentations, %ux%u signal, "
            "%u lit pixels; saved %s. CPU-written XFB coherence, full "
            "tasks/input/audio/reset and source CRT shutdown remain held.\n",
            checks, static_cast<unsigned long long>(output.copy_revision),
            static_cast<unsigned long long>(output.presentations), width, height,
            lit, image.c_str());
        if (interactive) {
            std::puts("Original loading image reached checkpoint86. Close the window to end this diagnostic.");
            bool running = true;
            while (running) {
                for (const auto* event = aurora_update(); event->type != AURORA_NONE; ++event)
                    if (event->type == AURORA_EXIT) running = false;
                if (running) VIWaitForRetrace();
            }
        }
        // Existing selected-source lifetime boundary: do not pretend live game
        // statics/arenas have completed the full original shutdown sequence.
        std::fflush(nullptr); std::_Exit(0);
    } catch (const std::exception& error) {
        std::fprintf(stderr, "Original loading source gate stopped: %s\n", error.what());
        std::fflush(nullptr); std::_Exit(1);
    }
}
