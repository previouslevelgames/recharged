#pragma once

namespace mscharged::runtime::detail {

enum class PipelineWarmupResult { Ready, Cancelled, Failed };
struct PipelineWarmupEvents {
    bool closeRequested = false;
    bool targetsChanged = false;
};

// The small startup loop is shared with the ordering tests. The production
// adapter below it owns events, rendering and pacing; the renderer alone
// decides readiness (including the final in-flight compilation/cache work).
template <class Host>
PipelineWarmupResult RunPipelineWarmup(Host& host)
{
    bool prepared = false;
    for (;;) {
        const auto events = host.PollEvents();
        if (events.closeRequested) return PipelineWarmupResult::Cancelled;
        if (!prepared || events.targetsChanged) {
            host.Prepare();
            prepared = true;
        }
        const auto status = host.Status();
        if (status.failedPipelines) return PipelineWarmupResult::Failed;
        if (status.ready) return PipelineWarmupResult::Ready;
        host.Draw(status);
        host.Pause();
    }
}

} // namespace mscharged::runtime::detail
