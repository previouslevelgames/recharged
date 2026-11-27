#pragma once

#include "platform/parallel_jobs.h"

#include <memory>

namespace mscharged::runtime {

// The host's worker threads for the game module (platform/parallel_jobs.h).
// Between batches a worker waits spinning for a moment (a replay catching up
// takes a step every few tenths of a millisecond), then asleep. One pool per
// process: Table() hands it to the module.
class ParallelJobs {
public:
    explicit ParallelJobs(unsigned workers);
    ~ParallelJobs();
    ParallelJobs(const ParallelJobs&) = delete;
    ParallelJobs& operator=(const ParallelJobs&) = delete;

    // The workers for a computer with this many hardware threads: none below
    // three, at most three (the game, its drawing and its sound have theirs).
    static unsigned DefaultWorkers(unsigned hardware_threads);

    unsigned Workers() const;
    // The module's table, valid while this pool lives.
    const ChargedParallelJobs* Table() const;
    // A batch (ChargedParallelJobs::start and finish), from one thread.
    void Start(ChargedParallelJob job, void* context, unsigned count);
    void Finish();

private:
    struct State;
    std::unique_ptr<State> state_;
};

} // namespace mscharged::runtime
