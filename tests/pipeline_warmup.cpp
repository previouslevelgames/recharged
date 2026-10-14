#include "runtime/pipeline_warmup_loop.h"

#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <vector>

using namespace mscharged::runtime::detail;
namespace {
struct Status {
    std::uint64_t pendingPipelines = 0;
    std::uint64_t failedPipelines = 0;
    bool ready = false;
};
struct Step { PipelineWarmupEvents events; Status status; };
struct Host {
    std::vector<Step> steps;
    unsigned index = 0, prepares = 0, draws = 0, pauses = 0, reads = 0;
    PipelineWarmupEvents PollEvents() {
        if (index >= steps.size()) throw std::runtime_error("Polled beyond the supplied worker/event sequence");
        return steps[index].events;
    }
    void Prepare() { ++prepares; }
    ::Status Status() { ++reads; return steps[index].status; }
    void Draw(const ::Status&) { ++draws; }
    void Pause() { ++pauses; ++index; }
};
void Check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
}

int main()
{
    try {
        Host empty{{{{}, {0, 0, true}}}};
        Check(RunPipelineWarmup(empty) == PipelineWarmupResult::Ready && empty.prepares == 1 && !empty.draws,
              "An empty/warm cache must enter the game without an artificial progress frame or delay");

        // Removing the last queued job does not mean the worker has published
        // its pipeline or finished pruning its binary cache. Only ready admits.
        Host compiling{{{{}, {1, 0, false}}, {{}, {0, 0, false}}, {{}, {0, 0, true}}}};
        Check(RunPipelineWarmup(compiling) == PipelineWarmupResult::Ready && compiling.draws == 2 &&
                  compiling.pauses == 2 && compiling.reads == 3,
              "The game must stay outside source entry through the final compile/publication interval");

        Host initialClose{{{{true, false}, {0, 0, true}}}};
        Check(RunPipelineWarmup(initialClose) == PipelineWarmupResult::Cancelled && !initialClose.prepares &&
                  !initialClose.reads && !initialClose.draws,
              "Already-queued close wins even when the cache is ready");

        Host duringClose{{{{}, {10, 0, false}}, {{true, false}, {0, 0, true}}}};
        Check(RunPipelineWarmup(duringClose) == PipelineWarmupResult::Cancelled && duringClose.draws == 1 &&
                  duringClose.reads == 1,
              "Close during background work wins over simultaneous ready publication");

        Host resized{{{{}, {10, 0, false}}, {{false, true}, {2, 0, false}}, {{}, {0, 0, true}}}};
        Check(RunPipelineWarmup(resized) == PipelineWarmupResult::Ready && resized.prepares == 2 && resized.draws == 2,
              "A resized target must be reconciled before checking readiness");

        Host failed{{{{}, {0, 1, true}}}};
        Check(RunPipelineWarmup(failed) == PipelineWarmupResult::Failed && !failed.draws,
              "A failed pipeline must prevent entry even if no jobs remain");
        std::cout << "6 startup graphics preparation ordering checks passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
