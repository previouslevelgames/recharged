#include "platform/interrupt_controller.h"
#include "platform/interrupts.h"
#include "platform/video_device.h"
#include <aurora/video.h>
#include <dolphin/os.h>
#include <dolphin/vi.h>
#include <chrono>
#include <cstdio>
#include <limits>
#include <stdexcept>
#include <thread>

namespace {
unsigned checks;
void Check(bool result, const char* message) {
    ++checks;
    if (!result) throw std::runtime_error(message);
}
AuroraVIDimmingRequestState Request() {
    AuroraVIDimmingRequestState state{};
    Check(aurora_get_video_dimming_request_state(&state), "Initialized VI request state missing");
    return state;
}
template<class Function> void Reject(Function function, const char* message) {
    bool rejected = false;
    try { function(); } catch (const std::logic_error&) { rejected = true; }
    Check(rejected, message);
}
void WaitFormat(unsigned format) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (VIGetTvFormat() != format && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        aurora_service_video_interrupts();
    }
    Check(VIGetTvFormat() == format, "True VI flush did not commit source TV format");
}
GXRenderModeObj Mode(unsigned value) {
    GXRenderModeObj mode{};
    mode.viTVmode = VITVMode(value);
    mode.fbWidth = mode.viWidth = 640;
    mode.efbHeight = mode.xfbHeight = mode.viHeight = value == VI_TVMODE_PAL_INT ? 574 : 480;
    mode.xFBmode = VI_XFBMODE_SF;
    return mode;
}
unsigned pre_calls, post_calls;
void Pre(u32 count) {
    ++pre_calls;
    Check(count == VIGetRetraceCount(), "Dimming callback preceded real source count");
    Check(!mscharged::platform::NativeInterruptsEnabled(), "Dimming pre callback lost IRQ exclusion");
    Check(VISetTimeToDimming(VI_DM_10M) == VI_DM_15M, "Pre callback lost prior source request");
}
void Post(u32 count) {
    ++post_calls;
    Check(count == VIGetRetraceCount(), "Dimming post callback lost real source count");
    Check(!mscharged::platform::NativeInterruptsEnabled(), "Dimming post callback lost IRQ exclusion");
    Check(VISetTimeToDimming(VI_DM_15M) == VI_DM_10M, "Post callback lost prior source request");
}
}

int main() {
    try {
        static_assert(sizeof(int) == 4);
        static_assert(VI_DM_DEFAULT == 0 && VI_DM_10M == 1 && VI_DM_15M == 2);
        AuroraVIDimmingRequestState empty{};
        Check(!aurora_get_video_dimming_request_state(&empty), "Uninitialized VI invented dimming state");
        Check(!aurora_get_video_dimming_request_state(nullptr), "Null dimming observer output accepted");
        Reject([] { VISetTimeToDimming(VI_DM_15M); }, "Uninitialized VI accepted a game request");
        mscharged::platform::InitializeNativeInterruptController();

        // Independent constants from the retained RVL vi.c request tables.
        // EURGB60 and MPAL use the non-PAL table; native presentations do not
        // enter these field-count decisions.
        struct Case { unsigned mode, default_fields, ten_fields, fifteen_fields; };
        const Case cases[] = {
            {VI_TVMODE_NTSC_INT, 18000, 36000, 54000},
            {VI_TVMODE_PAL_INT, 15000, 30000, 45000},
            {VI_TVMODE_MPAL_INT, 18000, 36000, 54000},
            {VI_TVMODE_EURGB60_INT, 18000, 36000, 54000},
            {VI_TVMODE_NTSC_PROG, 18000, 36000, 54000},
        };
        for (const auto& value : cases) {
            mscharged::platform::ConfigureNativeVideoHardware(value.mode, false);
            Check(!aurora_get_video_dimming_request_state(&empty), "Configuration invented VIInit completion");
            VIInit();
            auto request = Request();
            Check(request.time == VI_DM_DEFAULT && request.pending_field_threshold == value.default_fields &&
                  request.threshold_tv_format == value.mode >> 2, "VIInit request defaults differ from retail");
            AuroraVIHardwareState before{};
            Check(aurora_get_video_hardware_state(&before), "Real VI hardware state missing");
            Check(VISetTimeToDimming(VI_DM_15M) == VI_DM_DEFAULT, "Title request lost default predecessor");
            request = Request();
            Check(request.time == VI_DM_15M && request.pending_field_threshold == value.fifteen_fields,
                  "Title request changed original fifteen-minute threshold");
            VIInit();
            Check(Request().time == VI_DM_15M, "Idempotent VIInit reset a live game request");
            Check(VISetTimeToDimming(VI_DM_10M) == VI_DM_15M, "Ten-minute request lost predecessor");
            Check(Request().pending_field_threshold == value.ten_fields, "Ten-minute threshold differs from retail");
            for (int unknown : {-1, 3, std::numeric_limits<int>::max()}) {
                const auto prior = Request().time;
                Check(VISetTimeToDimming(unknown) == prior, "Unknown request predecessor was normalized");
                request = Request();
                Check(request.time == unknown && request.pending_field_threshold == value.default_fields,
                      "Unknown original request/default branch changed");
            }
            Check(VISetTimeToDimming(VI_DM_DEFAULT) == std::numeric_limits<int>::max(),
                  "Default reset lost the actual unknown predecessor");
            AuroraVIHardwareState after{};
            Check(aurora_get_video_hardware_state(&after), "Real VI state disappeared after request");
            Check(before.black == after.black && before.pending_flush == after.pending_flush &&
                  before.current_framebuffer == after.current_framebuffer &&
                  before.next_framebuffer == after.next_framebuffer && before.retrace_count == after.retrace_count,
                  "Dimming setter fabricated framebuffer, flush, black or source count changes");
            bool foreign_rejected{};
            std::thread foreign([&] {
                try { VISetTimeToDimming(VI_DM_15M); }
                catch (const std::logic_error&) { foreign_rejected = true; }
            });
            foreign.join();
            Check(foreign_rejected && Request().time == VI_DM_DEFAULT,
                  "Foreign setter changed the source owner request");
            const auto level = OSDisableInterrupts();
            Check(VISetTimeToDimming(VI_DM_10M) == VI_DM_DEFAULT, "Excluded setter lost source predecessor");
            Check(!mscharged::platform::NativeInterruptsEnabled(), "Setter changed caller interrupt exclusion");
            OSRestoreInterrupts(level);
            aurora_shutdown_video_hardware();
            Check(!aurora_get_video_dimming_request_state(&empty), "Retired request survived VI owner release");
            Reject([] { VISetTimeToDimming(VI_DM_DEFAULT); }, "Retired VI owner accepted a source request");
        }

        mscharged::platform::ConfigureNativeVideoHardware(VI_TVMODE_EURGB60_INT, false);
        VIInit();
        Check(VISetTimeToDimming(VI_DM_15M) == VI_DM_DEFAULT, "Restart retained stale Title request");
        auto mode = Mode(VI_TVMODE_PAL_INT);
        VIConfigure(&mode);
        Check(Request().pending_field_threshold == 54000, "Unflushed PAL mode changed dimming table early");
        VIFlush(); WaitFormat(VI_PAL);
        Check(Request().pending_field_threshold == 45000 && Request().time == VI_DM_15M,
              "True source TV change lost pending dimming request/table");
        Check(!VISetPreRetraceCallback(Pre) && !VISetPostRetraceCallback(Post), "Unexpected existing source callback");
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
        while (!post_calls && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            aurora_service_video_interrupts();
        }
        Check(pre_calls && post_calls && Request().time == VI_DM_15M &&
              Request().pending_field_threshold == 45000, "True pre/post requests failed without owner reentry");
        VISetPreRetraceCallback(nullptr); VISetPostRetraceCallback(nullptr);
        aurora_shutdown_video_hardware();
        mscharged::platform::ShutdownNativeInterruptController();
        std::printf("Native VI request/owner/source-format checks: %u; no idle/GPU dimming acceptance.\n", checks);
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "Native VI dimming request failure: %s\n", error.what());
        return 1;
    }
}
