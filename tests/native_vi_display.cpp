#include "platform/interrupt_controller.h"
#include "platform/interrupts.h"
#include "platform/stm_hardware_abi.h"
#include "platform/video_device.h"
#include <aurora/video.h>
#include <dolphin/os.h>
#include <dolphin/vi.h>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstddef>
#include <stdexcept>
#include <thread>

// CPU request/owner qualification only. The recording endpoint below verifies
// the actual native VI register boundary; it performs no fake successful
// Present, XFB completion or source resource initialization. Real scanout and
// black surface output require the separately compiled provider/GPU gate.
namespace {
unsigned checks;
void Check(bool value, const char* message) {
    ++checks;
    if (!value) throw std::runtime_error(message);
}
template<class Function> void Reject(Function function, const char* message) {
    bool rejected = false;
    try { function(); } catch (const std::exception&) { rejected = true; }
    Check(rejected, message);
}
bool Enabled() {
    bool enabled{};
    Check(aurora_get_video_display_enabled(&enabled), "Missing initialized ENB register");
    return enabled;
}
AuroraVIHardwareState Hardware() {
    AuroraVIHardwareState state{};
    Check(aurora_get_video_hardware_state(&state), "Missing live VI owner");
    return state;
}
GXRenderModeObj Mode() {
    GXRenderModeObj mode{};
    mode.viTVmode = VI_TVMODE_NTSC_INT;
    mode.fbWidth = mode.viWidth = 640;
    mode.efbHeight = mode.xfbHeight = mode.viHeight = 480;
    mode.viXOrigin = 40;
    mode.xFBmode = VI_XFBMODE_SF;
    return mode;
}
struct Requests {
    unsigned prepares{}, commits{}, services{}, shutdowns{}, controls{};
    bool enabled{true}, throw_control{};
    AuroraVIScanout prepared{}, committed{}, controlled{};
};
bool Prepare(void* context, const AuroraVIScanout* scanout) {
    auto& requests = *static_cast<Requests*>(context);
    Check(scanout && scanout->black, "CPU request sink would accept an unqualified colored XFB");
    ++requests.prepares; requests.prepared = *scanout;
    return true;
}
void Commit(void* context, const AuroraVIScanout* scanout) {
    auto& requests = *static_cast<Requests*>(context);
    Check(scanout && scanout->black && scanout->retrace_count == requests.prepared.retrace_count,
          "Source commit lost the prepared black register request");
    ++requests.commits; requests.committed = *scanout;
}
void Service(void* context) { ++static_cast<Requests*>(context)->services; }
void Shutdown(void* context) { ++static_cast<Requests*>(context)->shutdowns; }
void Control(void* context, bool enabled, const AuroraVIScanout* scanout) {
    auto& requests = *static_cast<Requests*>(context);
    ++requests.controls;
    Check(scanout && scanout->black && scanout->active_width == 720 && scanout->active_height == 480,
          "Display operation invented geometry or selected colored CPU data");
    Check(VIGetRetraceCount() == scanout->retrace_count, "Display operation invented a source retrace");
    if (!enabled) {
        Check(!Enabled(), "DCR zero was not latched before the hardware request");
        Reject([] { aurora_disable_video_display(); }, "Display operation reentered its live owner");
    }
    if (requests.throw_control) throw std::runtime_error("Actual request sink fault");
    requests.enabled = enabled; requests.controlled = *scanout;
}
unsigned pre_calls, post_calls;
bool reject_in_callback;
void Pre(u32 count) {
    ++pre_calls;
    Check(count == VIGetRetraceCount() && !mscharged::platform::NativeInterruptsEnabled(),
          "Source pre callback lost its actual count/IRQ exclusion");
    if (reject_in_callback)
        Reject([] { mscharged_stm_disable_video_output(); }, "STM display wait entered a source IRQ");
}
void Post(u32 count) {
    ++post_calls;
    Check(count == VIGetRetraceCount() && !mscharged::platform::NativeInterruptsEnabled(),
          "Source post callback lost its actual count/IRQ exclusion");
}
void WaitRetrace() {
    const auto before = VIGetRetraceCount();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (before == VIGetRetraceCount() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        aurora_service_video_interrupts();
    }
    Check(VIGetRetraceCount() > before, "DCR disable retired the real VI clock/IRQ");
}
}

int main() {
    try {
        bool enabled = true;
        Check(!aurora_get_video_display_enabled(&enabled) && !enabled,
              "Uninitialized VI fabricated enabled output");
        Check(!aurora_get_video_display_enabled(nullptr), "Null ENB observer accepted");
        Reject([] { mscharged_stm_disable_video_output(); }, "Cold STM request fabricated VI ownership");
        mscharged::platform::InitializeNativeInterruptController();

        // No output endpoint or GPU exists in this branch. DCR zero changes
        // only its actual live CPU register; no physical Present is claimed.
        mscharged::platform::ConfigureNativeVideoHardware(VI_TVMODE_NTSC_INT, false);
        Reject([] { mscharged_stm_disable_video_output(); }, "Configuration invented VIInit");
        VIInit();
        Check(Enabled(), "VIInit lost the original cached ENB=1");
        const auto handler = __OSGetInterruptHandler(__OS_INTERRUPT_PI_VI);
        const auto mask = OSDisableInterrupts();
        const auto before = Hardware();
        mscharged_stm_disable_video_output();
        const auto after = Hardware();
        Check(!Enabled() && !mscharged::platform::NativeInterruptsEnabled(),
              "DCR zero changed caller mask or left output enabled");
        Check(before.black == after.black && before.pending_flush == after.pending_flush &&
              before.current_framebuffer == after.current_framebuffer &&
              before.next_framebuffer == after.next_framebuffer && before.retrace_count == after.retrace_count,
              "DCR zero changed source black, flush, buffers or callback count");
        Check(handler && __OSGetInterruptHandler(__OS_INTERRUPT_PI_VI) == handler,
              "DCR zero retired the source PI_VI handler");
        OSRestoreInterrupts(mask);
        VIInit(); Check(!Enabled(), "Idempotent original VIInit re-enabled DCR without a register write");
        WaitRetrace(); Check(!Enabled(), "An ordinary retrace re-enabled disabled display");
        auto mode = Mode(); VIConfigure(&mode);
        Check(!Enabled(), "Unflushed cached VI mode re-enabled display early");
        VIFlush(); WaitRetrace(); Check(Enabled(), "Actual cached ENB=1 Mode flush failed to re-enable display");
        aurora_shutdown_video_hardware();

        // Existing endpoints remain usable, but STM must reject one without
        // a display-control operation before changing any register/source state.
        Requests unsupported{};
        mscharged::platform::ConfigureNativeVideoHardware(VI_TVMODE_NTSC_INT, false);
        const AuroraVIOutputDevice legacy{&unsupported, Prepare, Commit, Service, Shutdown};
        aurora_attach_video_output_device(&legacy); VIInit();
        Reject([] { mscharged_stm_disable_video_output(); }, "Missing output register operation accepted");
        Check(Enabled() && unsupported.controls == 0 && unsupported.commits == 0,
              "Unsupported endpoint changed ENB or fabricated a commit");
        aurora_shutdown_video_hardware(); Check(unsupported.shutdowns == 1, "Actual owner retirement lost its endpoint");

        Requests requests{};
        mscharged::platform::ConfigureNativeVideoHardware(VI_TVMODE_NTSC_INT, false);
        const AuroraVIOutputDevice output{&requests, Prepare, Commit, Service, Shutdown, Control};
        aurora_attach_video_output_device(&output); VIInit();
        Check(Enabled(), "New VI incarnation retained disabled register");
        bool foreign_rejected{};
        std::thread foreign([&] {
            try { mscharged_stm_disable_video_output(); }
            catch (const std::logic_error&) { foreign_rejected = true; }
        }); foreign.join();
        Check(foreign_rejected && Enabled() && !requests.controls, "Foreign DCR writer changed the live owner");
        {
            mscharged::platform::NativeInterruptGuard guard;
            Reject([] { mscharged_stm_disable_video_output(); }, "Retained native guard entered display wait");
            Check(Enabled() && !requests.controls, "Rejected retained guard mutated DCR");
        }
        std::array<std::byte, 64> borrowed{};
        VISetNextFrameBuffer(borrowed.data()); VISetBlack(false); VIFlush();
        const auto pending = Hardware();
        requests.throw_control = true;
        Reject([] { mscharged_stm_disable_video_output(); }, "Actual output failure returned successful disable");
        Check(!Enabled() && requests.controls == 1 && requests.shutdowns == 0,
              "Output fault rolled back the genuine register or retired owners");
        requests.throw_control = false;
        mscharged_stm_disable_video_output();
        Check(requests.controls == 2 && !requests.enabled && !requests.commits && !requests.services,
              "Failed display operation was not retried through its own endpoint");
        const auto disabled = Hardware();
        Check(pending.black == disabled.black && pending.pending_flush == disabled.pending_flush &&
              pending.next_framebuffer == disabled.next_framebuffer && disabled.pending_flush,
              "DCR zero consumed the original pending source flush");
        mscharged_stm_disable_video_output(); Check(requests.controls == 2, "Repeated completed DCR zero duplicated hardware work");
        Check(!VISetPreRetraceCallback(Pre) && !VISetPostRetraceCallback(Post), "Unexpected source callbacks");
        reject_in_callback = true;
        WaitRetrace();
        const auto flushed = Hardware();
        Check(!Enabled() && !flushed.black && !flushed.pending_flush &&
              flushed.current_framebuffer == borrowed.data() && flushed.next_framebuffer == borrowed.data(),
              "Real source flush lost black/request/framebuffer independence from DCR");
        Check(requests.commits && requests.committed.black && requests.committed.framebuffer == borrowed.data() &&
              !requests.services && !requests.shutdowns && pre_calls && pre_calls == post_calls,
              "Disabled display changed source callbacks/requests or invoked rendering/retirement inside IRQ");
        const auto priorFields = flushed.elapsed_fields;
        WaitRetrace(); Check(Hardware().elapsed_fields > priorFields && !Enabled(), "Live disabled VI stopped its actual fields");
        VISetBlack(true); VIConfigure(&mode); VIFlush();
        Check(!Enabled(), "Mode request re-enabled display before real source flush");
        WaitRetrace();
        Check(Enabled() && requests.enabled && requests.controls == 3 && requests.committed.black,
              "Original cached DCR Mode write did not reach the existing output endpoint");
        VISetPreRetraceCallback(nullptr); VISetPostRetraceCallback(nullptr);
        aurora_shutdown_video_hardware();
        Check(requests.shutdowns == 1, "DCR operation prematurely retired the true output owner");
        enabled = true;
        Check(!aurora_get_video_display_enabled(&enabled) && !enabled, "Retired ENB observer retained stale state");
        Reject([] { mscharged_stm_disable_video_output(); }, "Retired STM request reused old VI incarnation");
        mscharged::platform::ShutdownNativeInterruptController();
        std::printf("Native VI DCR/owner requests: %u checks; no GPU/physical Present or ResetTask acceptance.\n", checks);
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "Native VI display failure: %s\n", error.what());
        return 1;
    }
}
