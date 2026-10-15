#include "platform/interrupt_controller.h"
#include "platform/interrupts.h"
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

// Host frame pacing at the native VI (aurora_configure_video_pacing and
// aurora_paced_video_swap): CPU qualification with a recording endpoint. It
// accepts only black scanouts, so no XFB completion or physical Present is
// claimed; the output's skip of a repeated copy needs the GPU scanout.
namespace {
using Clock = std::chrono::steady_clock;
using std::chrono::milliseconds;
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
struct Requests {
    // Host commits run outside the retrace's IRQ exclusion; retrace commits inside it.
    unsigned host_commits{}, retrace_commits{}, services{}, pacing_calls{};
    bool paced{};
    AuroraVIScanout prepared{}, host_committed{};
};
bool Prepare(void* context, const AuroraVIScanout* scanout) {
    auto& requests = *static_cast<Requests*>(context);
    Check(scanout && scanout->black, "CPU request sink would accept an unqualified colored XFB");
    requests.prepared = *scanout;
    return true;
}
void Commit(void* context, const AuroraVIScanout* scanout) {
    auto& requests = *static_cast<Requests*>(context);
    Check(scanout && scanout->retrace_count == requests.prepared.retrace_count, "Commit lost its prepared request");
    if (mscharged::platform::NativeInterruptsEnabled()) { ++requests.host_commits; requests.host_committed = *scanout; }
    else ++requests.retrace_commits;
}
void Service(void* context) { ++static_cast<Requests*>(context)->services; }
void Shutdown(void*) {}
void Pacing(void* context, bool paced) {
    auto& requests = *static_cast<Requests*>(context);
    ++requests.pacing_calls; requests.paced = paced;
}
GXRenderModeObj Mode() {
    GXRenderModeObj mode{};
    mode.viTVmode = VI_TVMODE_NTSC_PROG;
    mode.fbWidth = mode.viWidth = 640;
    mode.efbHeight = mode.xfbHeight = mode.viHeight = 480;
    mode.viXOrigin = 40;
    mode.xFBmode = VI_XFBMODE_SF;
    return mode;
}
}

int main() {
    try {
        mscharged::platform::InitializeNativeInterruptController();
        mscharged::platform::ConfigureNativeVideoHardware(VI_TVMODE_NTSC_INT, false);
        AuroraVideoPacing pacing{123};
        Check(!aurora_get_video_pacing(&pacing) && pacing.interval_ns == 0, "Host pacing on before it was configured");
        Check(!aurora_get_video_pacing(nullptr), "Null pacing observer accepted");

        // Pacing set before the output attaches reaches it at attachment.
        const AuroraVideoPacing slot{20'000'000};
        aurora_configure_video_pacing(&slot);
        Requests requests{};
        const AuroraVIOutputDevice output{&requests, Prepare, Commit, Service, Shutdown, nullptr, Pacing};
        aurora_attach_video_output_device(&output);
        Check(requests.pacing_calls == 1 && requests.paced, "Output did not hear about host pacing");
        bool foreign_rejected{};
        std::thread foreign([&] {
            try { aurora_configure_video_pacing(nullptr); }
            catch (const std::logic_error&) { foreign_rejected = true; }
        }); foreign.join();
        Check(foreign_rejected && requests.paced, "A foreign thread changed host pacing");
        VIInit();
        Check(aurora_get_video_pacing(&pacing) && pacing.interval_ns == slot.interval_ns, "VIInit lost host pacing");

        // A flushed frame is committed at once, with the current retrace count:
        // the swap fabricates no retrace.
        std::array<std::byte, 64> frame{};
        VISetNextFrameBuffer(frame.data()); VISetBlack(true); VIFlush();
        const auto count = VIGetRetraceCount();
        aurora_paced_video_swap();
        const auto first = Clock::now();
        Check(requests.host_commits == 1 && requests.host_committed.framebuffer == frame.data() &&
              requests.host_committed.black && requests.host_committed.retrace_count == count,
              "Flushed frame not committed at the swap");
        Check(requests.services >= 1, "Committed frame not handed to presentation");

        // The next swap waits for its slot; the VI fields keep running.
        VIFlush(); aurora_paced_video_swap();
        const auto second = Clock::now();
        Check(second - first >= milliseconds(19) && second - first < milliseconds(500), "Swap did not wait for its slot");
        Check(requests.host_commits == 2 && requests.retrace_commits >= 1 && VIGetRetraceCount() > count,
              "Retraces stopped or the second frame was not committed");

        // A late frame starts the cadence again instead of rushing to catch up.
        std::this_thread::sleep_for(milliseconds(60));
        VIFlush();
        const auto late = Clock::now();
        aurora_paced_video_swap();
        const auto after_late = Clock::now();
        Check(after_late - late < milliseconds(15), "Late frame waited for a missed slot");
        VIFlush(); aurora_paced_video_swap();
        Check(Clock::now() - after_late >= milliseconds(19), "Cadence lost after a late frame");

        // No flush, no new commit; a flush that also changes the TV mode keeps
        // the genuine retrace boundary.
        auto commits = requests.host_commits;
        aurora_paced_video_swap();
        Check(requests.host_commits == commits, "Swap without a flush committed a frame");
        auto mode = Mode(); VIConfigure(&mode); VIFlush();
        aurora_paced_video_swap();
        Check(requests.host_commits == commits, "Mode-changing flush presented before its retrace");

        // A window that presents nothing keeps at least the VI field period.
        const AuroraVideoPacing unlimited{0};
        aurora_configure_video_pacing(&unlimited);
        Check(aurora_get_video_pacing(&pacing) && pacing.interval_ns == 0 && requests.paced, "Unlimited pacing");
        VIFlush(); aurora_paced_video_swap();
        const auto unbounded = Clock::now();
        VIFlush(); aurora_paced_video_swap();
        Check(Clock::now() - unbounded >= milliseconds(15), "Hidden window ran without the field-period floor");

        // Off: the output hears it, and the swap is refused.
        aurora_configure_video_pacing(nullptr);
        Check(!aurora_get_video_pacing(&pacing) && pacing.interval_ns == 0 && !requests.paced && requests.pacing_calls == 3,
              "Host pacing not turned off");
        Reject([] { aurora_paced_video_swap(); }, "Paced swap ran without host pacing");
        aurora_configure_video_pacing(&slot);
        aurora_shutdown_video_hardware();
        Check(!aurora_get_video_pacing(&pacing), "Retired VI kept host pacing");
        mscharged::platform::ShutdownNativeInterruptController();
        std::printf("Native VI host pacing: %u checks; no GPU/physical Present acceptance.\n", checks);
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "Native VI pacing failure: %s\n", error.what());
        return 1;
    }
}
