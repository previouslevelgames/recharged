#include "runtime/pipeline_warmup.h"
#include "runtime/pipeline_warmup_loop.h"
#include "runtime/pipeline_catalog_layout.h"
#include "runtime/pipeline_demand_log.h"
#include "runtime/graphics_loading_view.h"

#include "platform/session_log.h"

#include <aurora/aurora.h>
#include <aurora/event.h>
#include <aurora/gfx.h>
#include <aurora/gfx.hpp>
#include <aurora/pipeline_cache.h>
#include <imgui.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <thread>

namespace mscharged::runtime {
namespace {
enum class Finish { Done, Closed, TargetsChanged };

struct WarmupHost {
    GraphicsLoadingView view;
    std::chrono::steady_clock::time_point next{};
    explicit WarmupHost(const std::filesystem::path& font) : view(font) {}
    detail::PipelineWarmupEvents PollEvents()
    {
        detail::PipelineWarmupEvents events;
        for (const auto* event = aurora_update(); event->type != AURORA_NONE; ++event) {
            events.closeRequested |= event->type == AURORA_EXIT;
            events.targetsChanged |= event->type == AURORA_WINDOW_RESIZED ||
                                     event->type == AURORA_DISPLAY_SCALE_CHANGED;
        }
        return events;
    }

    void Prepare() { aurora_prepare_pipelines(); }
    AuroraPipelineWarmupStatus Status() { return aurora_pipeline_warmup_status(); }
    void Draw(const AuroraPipelineWarmupStatus& status) { view.Draw(status); }

    // About 60 frames per second; the compiler worker keeps the rest.
    void Pause()
    {
        const auto now = std::chrono::steady_clock::now();
        next = std::max(now, next + std::chrono::microseconds(16667));
        std::this_thread::sleep_until(next);
    }

    // Completes the bar and fades out, ending on the black the game starts
    // from: a single black frame when nothing was shown.
    Finish FinishView()
    {
        for (;;) {
            const auto events = PollEvents();
            if (events.closeRequested) return Finish::Closed;
            if (events.targetsChanged) return Finish::TargetsChanged;
            if (!view.DrawFinished()) return Finish::Done;
            Pause();
        }
    }
};

// Runs on the drawing thread; a log failure must not reach the renderer.
void LogDemandMiss(const AuroraPipelineDemandMiss* miss, void*) noexcept
{
    try {
        platform::SessionLogLine(PipelineDemandMissLine(*miss));
    } catch (...) {
    }
}
} // namespace

bool PrepareGraphicsForGame(const std::filesystem::path& font)
{
    platform::SessionLogLine("Startup: preparing graphics");
    const auto started = std::chrono::steady_clock::now();
    // Draw-list-only UI has no settings or cursor/input ownership. Restore the
    // context before the game's later FPS overlay chooses its own fonts.
    auto& io = ImGui::GetIO();
    struct RestoreImGui {
        ImGuiIO& io;
        const char* ini;
        ImGuiConfigFlags flags;
        ~RestoreImGui() { io.IniFilename = ini; io.ConfigFlags = flags; }
    } restore{io, io.IniFilename, io.ConfigFlags};
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;

    WarmupHost host(font);
    auto result = detail::RunPipelineWarmup(host);
    auto ready = std::chrono::steady_clock::now();
    // A resize during the fade-out prepares the new targets first.
    while (result == detail::PipelineWarmupResult::Ready) {
        const auto finish = host.FinishView();
        if (finish == Finish::Done) break;
        result = finish == Finish::Closed ? detail::PipelineWarmupResult::Cancelled
                                          : detail::RunPipelineWarmup(host);
        ready = std::chrono::steady_clock::now();
    }
    // Retire all startup draws before original GX acquires its FIFO and EFB.
    aurora::gfx::synchronize();
    if (result == detail::PipelineWarmupResult::Cancelled) {
        platform::SessionLogLine("Startup: graphics preparation cancelled");
        return false;
    }
    if (result == detail::PipelineWarmupResult::Failed)
        throw std::runtime_error("Graphics preparation failed; see the session log for the graphics error");
    const auto status = host.Status();
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(ready - started).count();
    platform::SessionLogLine("Startup: graphics ready, " + std::to_string(status.readyPipelines) +
        " pipelines from " + std::to_string(status.knownConfigs) + " configurations in " +
        std::to_string(elapsed) + " ms");
    // Everything known is prepared: a draw that still waits for a compile is a
    // stutter in play. Log it; a new configuration's line completes the catalogue.
    aurora_set_pipeline_demand_miss_callback(LogDemandMiss, nullptr);
    return true;
}

void LogGraphicsPipelineStats()
{
    const auto status = aurora_pipeline_warmup_status();
    platform::SessionLogLine("Graphics pipelines: " + std::to_string(status.knownConfigs) +
        " configurations, " + std::to_string(status.readyPipelines) + " ready, " +
        std::to_string(status.demandHits) + " demand hits, " +
        std::to_string(status.demandMisses) + " demand misses, " +
        std::to_string(status.demandWaitNanoseconds / 1000000) + " ms waiting for them");
}

} // namespace mscharged::runtime
