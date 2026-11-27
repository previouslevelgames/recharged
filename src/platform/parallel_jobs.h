#pragma once

// Worker threads the host lends the game module (src/runtime/parallel_jobs.cpp):
// the module's own code must not make threads, as everything it allocates with
// new comes from the game's heap. The module's parallel posing
// (src/platform/parallel_pose.cpp) runs its jobs on them.
#ifdef __cplusplus
extern "C" {
#endif

typedef void (*ChargedParallelJob)(void* context, unsigned index);

struct ChargedParallelJobs
{
    // Threads beside the caller's.
    unsigned workers;
    // Starts job(context, i) for every i below count on the workers, while the
    // caller goes on. One batch at a time, always from the same thread.
    void (*start)(ChargedParallelJob job, void* context, unsigned count);
    // Runs the batch's jobs no worker took yet on the calling thread, and
    // returns once every job of it has run.
    void (*finish)(void);
};

// Before original main; null for none (the module then runs everything on
// its own thread).
typedef void (*ChargedConfigureParallelJobsFunction)(const ChargedParallelJobs* jobs);

#ifdef __cplusplus
}
#endif
