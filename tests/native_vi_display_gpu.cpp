// Real GX/VI hardware qualification, with generated geometry and no game data.
// No successful Present, copy completion, retrace or input is supplied by this
// fixture. The worker barrier only delays a real queued presentation.
#include "platform/graphics_backend.h"
#include "platform/interrupt_controller.h"
#include "platform/interrupts.h"
#include "platform/path.h"
#include "platform/stm_hardware_abi.h"
#include "platform/system.h"
#include "platform/video_device.h"
#include "platform/video_output_device.h"
#include <aurora/aurora.h>
#include <aurora/event.h>
#include <aurora/gfx.h>
#include <aurora/video.h>
#include <dolphin/gx.h>
#include <dolphin/mtx.h>
#include <dolphin/os.h>
#include <dolphin/os/OSNativeMemory.h>
#include <dolphin/vi.h>
#include "gfx/render_worker.hpp"
#include "gfx/scanout.hpp"
#include "gfx/xfb.hpp"
#include "webgpu/gpu.hpp"
#include "window.hpp"
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;
unsigned checks, pre_calls, post_calls;
bool callback_fault;
std::atomic_uint errors{};

void Check(bool value, const char* reason) {
    ++checks;
    if (!value) throw std::runtime_error(reason);
}
void Log(AuroraLogLevel level, const char* module, const char* message, unsigned bytes) {
    if (level >= LOG_ERROR) ++errors;
    std::fprintf(stderr, "[%s] %.*s\n", module, int(bytes), message);
}
void Pre(u32 count) {
    ++pre_calls;
    callback_fault |= count != VIGetRetraceCount() ||
        mscharged::platform::NativeInterruptsEnabled();
}
void Post(u32 count) {
    ++post_calls;
    callback_fault |= count != VIGetRetraceCount() ||
        mscharged::platform::NativeInterruptsEnabled();
}
bool Enabled() {
    bool enabled{};
    Check(aurora_get_video_display_enabled(&enabled), "The actual VI ENB register is unavailable");
    return enabled;
}
AuroraVIHardwareState Hardware() {
    AuroraVIHardwareState state{};
    Check(aurora_get_video_hardware_state(&state), "The actual VI owner is unavailable");
    return state;
}
AuroraVIPresentedState Presented() {
    AuroraVIPresentedState state{};
    Check(aurora_get_presented_video_output_state(&state), "The real presentation endpoint is unavailable");
    return state;
}
void PollWindow() {
    for (const auto* event = aurora_update(); event->type != AURORA_NONE; ++event)
        Check(event->type != AURORA_EXIT, "The window closed before display qualification");
}
AuroraVIPresentedState WaitPresented(std::uint64_t after, bool black) {
    const auto deadline = Clock::now() + 8s;
    do {
        PollWindow();
        aurora::gfx::render_worker::synchronize();
        const auto state = Presented();
        if (state.presentation_sequence > after && state.black == black) {
            Check(aurora::window::is_presentable(), "The completed output no longer has a presentable surface");
            return state;
        }
        std::this_thread::sleep_for(1ms);
    } while (Clock::now() < deadline);
    throw std::runtime_error("No new actual surface Present completed; hidden or unavailable surfaces cannot pass");
}
void WaitInterrupt() {
    const auto before = VIGetRetraceCount();
    const auto deadline = Clock::now() + 1s;
    while (VIGetRetraceCount() == before && Clock::now() < deadline) {
        std::this_thread::sleep_for(1ms);
        aurora_service_video_interrupts();
    }
    Check(VIGetRetraceCount() > before, "Display control stopped the genuine VI retrace owner");
}

// This is the same actual CopyTextureToBuffer/MapAsync readback used by the
// existing original-loading fixture. It reads the successful Present's VI
// desktop signal, not an inferred SDL screenshot or source request buffer.
std::vector<unsigned char> ReadRGB(const aurora::gfx::TextureHandle& signal) {
    using namespace aurora::webgpu;
    if (!signal || signal->format != wgpu::TextureFormat::RGBA8Unorm)
        throw std::runtime_error("The real VI signal is absent or not RGBA8");
    const auto width = signal->size.width, height = signal->size.height;
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
    const auto result = std::make_shared<Result>();
    const auto future = buffer.MapAsync(wgpu::MapMode::Read, 0, descriptor.size,
        wgpu::CallbackMode::WaitAnyOnly,
        [result](wgpu::MapAsyncStatus status, wgpu::StringView) { result->status = status; });
    if (g_instance.WaitAny(future, 5'000'000'000) != wgpu::WaitStatus::Success ||
        result->status != wgpu::MapAsyncStatus::Success)
        throw std::runtime_error("Actual VI signal readback did not complete");
    if (g_instance.WaitAny(g_device.GetLostFuture(), 0) != wgpu::WaitStatus::TimedOut)
        throw std::runtime_error("A lost device cannot qualify display output");
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
bool Black(const std::vector<unsigned char>& rgb) {
    if (rgb.empty()) return false;
    for (const auto byte : rgb) if (byte) return false;
    return true;
}
void CheckLiveBankCopy(const aurora::gfx::xfb::Snapshot& copy, void* xfb) {
    u32 physical{};
    const auto pin = OSNativePinAddress(xfb, copy.bytes, TRUE, &physical);
    if (pin) OSNativeUnpinAddress(pin);
    // Canonical OSNativePinAddress validates the full writable extent and
    // returns zero for real SDK MEM1/MEM2 banks. Nonzero tokens retain a
    // separately registered static owner; this fixture uses the real arena.
    Check(!pin && !copy.backingPin && copy.physical == physical &&
              OSPhysicalToCached(physical) == xfb,
          "The completed XFB lost its exact live writable SDK-bank backing");
}
void DrawColoredCopy(void* xfb) {
    const auto& mode = GXNtsc480IntDf;
    GXSetViewport(0, 0, mode.fbWidth, mode.efbHeight, 0, 1);
    GXSetScissor(0, 0, mode.fbWidth, mode.efbHeight);
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
    GXSetColorUpdate(GX_TRUE); GXSetAlphaUpdate(GX_TRUE);
    GXSetPixelFmt(GX_PF_RGB8_Z24, GX_ZC_LINEAR);
    GXSetNumChans(1);
    GXSetChanCtrl(GX_COLOR0A0, GX_DISABLE, GX_SRC_REG, GX_SRC_VTX,
                  GX_LIGHT_NULL, GX_DF_NONE, GX_AF_NONE);
    GXSetNumTevStages(1); GXSetNumTexGens(0);
    GXSetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD_NULL, GX_TEXMAP_NULL, GX_COLOR0A0);
    GXSetTevOp(GX_TEVSTAGE0, GX_PASSCLR);
    GXClearVtxDesc();
    GXSetVtxDesc(GX_VA_POS, GX_DIRECT); GXSetVtxDesc(GX_VA_CLR0, GX_DIRECT);
    GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
    GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0);
    GXBegin(GX_QUADS, GX_VTXFMT0, 4);
    GXPosition3f32(-1,-1,-0.3f); GXColor4u8(240,65,50,255);
    GXPosition3f32( 1,-1,-0.3f); GXColor4u8(50,220,100,255);
    GXPosition3f32( 1, 1,-0.3f); GXColor4u8(60,100,245,255);
    GXPosition3f32(-1, 1,-0.3f); GXColor4u8(230,170,40,255);
    GXEnd();
    GXSetCopyClear({0,0,0,255}, GX_MAX_Z24);
    GXCopyDisp(xfb, GX_TRUE);
    GXDrawDone();
}

struct WorkerBarrier {
    std::mutex mutex;
    std::condition_variable cv;
    bool started{}, released{};
    void Release() {
        { std::lock_guard lock(mutex); released = true; }
        cv.notify_all();
    }
};
struct QueuedObservation {
    bool completed{}, pixels_black{};
    AuroraVIPresentedState presented{};
    std::string failure;
};
}

int main(int argc, char** argv) {
    if (argc == 2 && std::string(argv[1]) == "--help") {
        std::puts("native_vi_display_gpu_tests\nReal GPU: colored Present, VI_DCR disable/black queued output, original Mode flush restore. No game data or keyboard focus required.");
        return 0;
    }
    bool host_live{}, controller_live{}, video_live{};
    try {
        Check(argc == 1, "Unknown argument; use --help");
        const auto* base = SDL_GetBasePath();
        Check(base, "Cannot locate the display fixture executable");
        const auto directory = mscharged::PathFromUtf8(base) / "vi-display-gpu-data";
        std::filesystem::create_directories(directory);
        const auto path = mscharged::PathUtf8(directory);
        AuroraConfig config{};
        config.appName = "Mario Strikers Charged | VI display hardware test";
        config.userPath = config.cachePath = path.c_str(); config.resourcesPath = base;
        config.desiredBackend = mscharged::platform::NativeGraphicsBackend;
        config.windowWidth = 800; config.windowHeight = 600;
        config.windowPosX = config.windowPosY = -1;
        config.logLevel = LOG_INFO; config.logCallback = Log;
        config.vsync = true; config.enableBackendValidation = true;
        config.mem1Size = MEM1_DEFAULT_SIZE;
        const auto host = aurora_initialize(argc, argv, &config); host_live = true;
        Check(host.window && host.backend == mscharged::platform::NativeGraphicsBackend,
              "The selected native GPU window is unavailable; fallback cannot pass");
        Check(aurora::webgpu::g_adapterInfo.adapterType != wgpu::AdapterType::CPU,
              "A CPU adapter cannot qualify actual display output");
        OSInit();
        mscharged::platform::InitializeNativeInterruptController(); controller_live = true;
        mscharged::platform::ConfigureNativeVideoHardware(VI_TVMODE_NTSC_INT, false); video_live = true;
        const mscharged::NativeSystemSettings settings{1,0,0,0,1};
        mscharged::platform::ConfigureNativeVideoOutputHardware(settings);
        aurora_configure_native_gx_hardware();
        VIInit(); VIConfigure(&GXNtsc480IntDf);
        constexpr unsigned fifo_bytes = 65536, xfb_bytes = 640 * 480 * 2;
        auto* fifo = OSAllocFromArenaLo(fifo_bytes, 32);
        auto* xfb = OSAllocFromArenaLo(xfb_bytes, 32);
        Check(fifo && xfb, "The real MEM1 arena cannot back the FIFO/XFB");
        GXInit(fifo, fifo_bytes);
        Check(!VISetPreRetraceCallback(Pre) && !VISetPostRetraceCallback(Post),
              "Unexpected preexisting VI callbacks");
        DrawColoredCopy(xfb);
        auto copy = aurora::gfx::xfb::find(xfb);
        std::fprintf(stderr,
            "Actual XFB: present=%d complete=%d physical=%08x static-pin=%llu "
            "width=%u height=%u stride=%u bytes=%u; expected640x480/1280/%u.\n",
            bool(copy), copy ? copy->gpuComplete.load(std::memory_order_acquire) : false,
            copy ? copy->physical : 0u,
            copy ? static_cast<unsigned long long>(copy->backingPin) : 0ull,
            copy ? copy->width : 0u, copy ? copy->height : 0u,
            copy ? copy->stride : 0u, copy ? copy->bytes : 0u, xfb_bytes);
        Check(copy && copy->gpuComplete.load(std::memory_order_acquire) &&
                  copy->bytes == xfb_bytes && copy->stride == 1280 &&
                  copy->width == 640 && copy->height == 480,
              "The generated source GX copy lacks real completion/pinned MEM1 backing");
        CheckLiveBankCopy(*copy, xfb);
        const auto revision = copy->revision, pin = copy->backingPin;
        VISetNextFrameBuffer(xfb); VISetBlack(false); VIFlush();
        const auto first = WaitPresented(0, false);
        Check(first.framebuffer == xfb && first.copy_revision == revision,
              "Colored Present did not use the source-selected completed XFB");
        auto signal = aurora::gfx::scanout::rendered_desktop_signal();
        const auto colored = ReadRGB(signal);
        Check(!Black(colored), "The actual colored VI output is entirely black");
        const auto handler = __OSGetInterruptHandler(__OS_INTERRUPT_PI_VI);
        const auto controller = mscharged::platform::GetNativeInterruptControllerStatus();
        const auto before = Hardware();
        Check(!before.black && before.current_framebuffer == xfb && Enabled(),
              "The source VI state is not the expected visible XFB");

        // Synchronize one real work item, then queue a colored retrace behind a
        // deterministic worker barrier. This proves stale queued color cannot
        // leak after DCR=0 rather than only checking a later black request.
        aurora::gfx::render_worker::synchronize();
        const auto before_queue = Presented();
        const auto barrier = std::make_shared<WorkerBarrier>();
        aurora::gfx::render_worker::enqueue_work([barrier] {
            std::unique_lock lock(barrier->mutex);
            barrier->started = true; barrier->cv.notify_all();
            barrier->cv.wait(lock, [&] { return barrier->released; });
        });
        struct Release { std::shared_ptr<WorkerBarrier> barrier; ~Release() { barrier->Release(); } } release{barrier};
        {
            std::unique_lock lock(barrier->mutex);
            Check(barrier->cv.wait_until(lock, Clock::now() + 2s, [&] { return barrier->started; }),
                  "The actual render worker did not reach the fixture barrier");
        }
        WaitInterrupt(); aurora_service_video_hardware();
        AuroraVIOutputState queued{};
        Check(aurora_get_video_output_state(&queued) && !queued.pending && !queued.black &&
                  Presented().presentation_sequence == before_queue.presentation_sequence,
              "A genuine colored output work item was not retained behind the barrier");
        const auto observation = std::make_shared<QueuedObservation>();
        aurora::gfx::render_worker::enqueue_work([observation, signal] {
            // This marker is ordered after the stale item and before the new
            // disable item. Exceptions become observations, never worker exits.
            try {
                if (!aurora_get_presented_video_output_state(&observation->presented))
                    throw std::runtime_error("Queued presentation observer lost its real endpoint");
                observation->pixels_black = Black(ReadRGB(signal));
                observation->completed = true;
            } catch (const std::exception& error) { observation->failure = error.what(); }
        });
        std::atomic_bool latched{};
        std::thread unblock([barrier, &latched] {
            const auto deadline = Clock::now() + 3s;
            while (Clock::now() < deadline) {
                bool enabled{}; AuroraVIOutputState output{};
                if (aurora_get_video_display_enabled(&enabled) && !enabled &&
                    aurora_get_video_output_state(&output) && output.pending) {
                    latched.store(true, std::memory_order_release); break;
                }
                std::this_thread::sleep_for(1ms);
            }
            // Always release the fixture delay, including an explicit failed
            // latch, so a failed implementation cannot strand the real worker.
            barrier->Release();
        });
        // __OSShutdownDevices leaves the original caller masked before the
        // terminal STM DCR=0 write. Keep that real mask lock held across both
        // actual worker drains; no interrupt-enable accommodation is supplied.
        const auto* caller_context = OSGetCurrentContext();
        const auto masked_pre_calls = pre_calls, masked_post_calls = post_calls;
        const BOOL saved_interrupts = OSDisableInterrupts();
        try {
            Check(saved_interrupts && !mscharged::platform::NativeInterruptsEnabled() &&
                      mscharged::platform::NativeInterruptWaitAllowed(),
                  "The original plain interrupt-mask caller was not established");
            mscharged_stm_disable_video_output();
            Check(!mscharged::platform::NativeInterruptsEnabled() &&
                      OSGetCurrentContext() == caller_context &&
                      pre_calls == masked_pre_calls && post_calls == masked_post_calls,
                  "Display drain changed the caller mask/context or delivered a masked VI callback");
        } catch (...) {
            OSRestoreInterrupts(saved_interrupts);
            barrier->Release(); unblock.join(); throw;
        }
        OSRestoreInterrupts(saved_interrupts);
        unblock.join();
        Check(mscharged::platform::NativeInterruptsEnabled() == bool(saved_interrupts) &&
                  OSGetCurrentContext() == caller_context,
              "The saved original caller interrupt mask/context did not restore");
        Check(latched.load(std::memory_order_acquire), "No actual DCR/output-disable latch was observed");
        Check(observation->failure.empty() && observation->completed && observation->pixels_black &&
                  observation->presented.black &&
                  observation->presented.presentation_sequence > before_queue.presentation_sequence,
              "The stale colored work item did not complete as a genuine black Present/pixels");
        const auto disabled = Presented();
        Check(disabled.black && disabled.presentation_sequence > observation->presented.presentation_sequence,
              "DCR disable returned without its own new successful black surface Present");
        Check(Black(ReadRGB(signal)) && !Enabled(), "Actual disabled output pixels/register are not black/disabled");
        const auto after = Hardware();
        Check(!after.black && after.current_framebuffer == before.current_framebuffer &&
                  after.next_framebuffer == before.next_framebuffer && !after.pending_flush &&
                  copy->backingPin == pin && copy->gpuComplete.load(std::memory_order_acquire) &&
                  aurora::gfx::xfb::find(xfb) == copy,
              "DCR disable altered source black/framebuffer state or retired its real XFB lease");
        CheckLiveBankCopy(*copy, xfb);
        Check(handler && __OSGetInterruptHandler(__OS_INTERRUPT_PI_VI) == handler &&
                  mscharged::platform::GetNativeInterruptControllerStatus().generation == controller.generation &&
                  mscharged::platform::GetNativeInterruptControllerStatus().user_mask == controller.user_mask,
              "DCR disable retired/remasked the original VI handler/controller");
        const auto disabled_callbacks = post_calls;
        VIInit(); WaitInterrupt();
        Check(!Enabled() && post_calls > disabled_callbacks && !callback_fault && pre_calls == post_calls,
              "Disabled VI lost source callbacks or re-enabled without an original register flush");
        VIConfigure(&GXNtsc480IntDf);
        Check(!Enabled(), "The original cached mode request enabled output before VIFlush");
        VISetBlack(false); VIFlush();
        const auto restored = WaitPresented(disabled.presentation_sequence, false);
        Check(Enabled() && restored.framebuffer == xfb && restored.copy_revision == revision &&
                  ReadRGB(signal) == colored && copy->backingPin == pin,
              "The original Mode flush failed to restore the same retained colored XFB/pixels");
        CheckLiveBankCopy(*copy, xfb);
        Check(post_calls > disabled_callbacks && pre_calls == post_calls && !callback_fault && errors == 0,
              "Real VI callbacks or GPU validation failed during display control");
        const auto total_callbacks = post_calls;
        VISetPreRetraceCallback(nullptr); VISetPostRetraceCallback(nullptr);
        // Final fixture shutdown is distinct from DCR disable. Drop observer
        // references before real GX/OS resource retirement, after actual drain.
        signal.reset(); copy.reset();
        aurora_shutdown_video_hardware(); video_live = false;
        mscharged::platform::ShutdownNativeInterruptController(); controller_live = false;
        aurora_shutdown(); host_live = false;
        Check(errors == 0, "Real display shutdown emitted a GPU/host error");
        std::printf("Native VI display GPU passed: %u checks; %s; XFB revision%llu pin%llu; "
            "colored%llu queued-black%llu disabled-black%llu restored%llu; %u paired VI callbacks; plain-masked drain restored.\n",
            checks, mscharged::platform::NativeGraphicsBackendName,
            static_cast<unsigned long long>(revision), static_cast<unsigned long long>(pin),
            static_cast<unsigned long long>(first.presentation_sequence),
            static_cast<unsigned long long>(observation->presented.presentation_sequence),
            static_cast<unsigned long long>(disabled.presentation_sequence),
            static_cast<unsigned long long>(restored.presentation_sequence), total_callbacks);
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "Native VI display GPU failure: %s\n", error.what());
        try {
            if (video_live) aurora_shutdown_video_hardware();
            if (controller_live) mscharged::platform::ShutdownNativeInterruptController();
            if (host_live) aurora_shutdown();
        } catch (const std::exception& cleanup) {
            std::fprintf(stderr, "Actual fixture cleanup also failed: %s\n", cleanup.what());
        }
        return 1;
    }
}
