// The host's worker threads for the game module (runtime/parallel_jobs.h): every
// job of a batch runs once, Finish returns only after all of them, batches
// that follow each other at once never mix (a worker still leaving one does
// not take a job of the next), with workers asleep or spinning, through the
// module's C table too.
#include "runtime/parallel_jobs.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using mscharged::runtime::ParallelJobs;

namespace
{
int sChecks = 0;
void Require(bool condition, const std::string& message)
{
    ++sChecks;
    if (!condition) throw std::runtime_error(message);
}

struct Batch
{
    std::vector<std::atomic<unsigned>> runs;
    explicit Batch(unsigned count) : runs(count) {}
};

void Count(void* context, unsigned index)
{
    auto* batch = static_cast<Batch*>(context);
    batch->runs.at(index).fetch_add(1);
    // A little work, so the workers take part.
    volatile unsigned spin = 0;
    for (unsigned i = 0; i < 200; ++i) spin += i;
}

void Check(ParallelJobs& pool, unsigned count, const std::string& what)
{
    Batch batch(count);
    pool.Start(Count, &batch, count);
    pool.Finish();
    for (unsigned i = 0; i < count; ++i)
        Require(batch.runs[i].load() == 1, what + ": job " + std::to_string(i) + " ran "
                                               + std::to_string(batch.runs[i].load()) + " times");
}
} // namespace

int main()
{
    try
    {
        Require(ParallelJobs::DefaultWorkers(1) == 0, "one hardware thread: no workers");
        Require(ParallelJobs::DefaultWorkers(2) == 0, "two hardware threads: no workers");
        Require(ParallelJobs::DefaultWorkers(4) == 2, "four hardware threads: two workers");
        Require(ParallelJobs::DefaultWorkers(16) == 3, "at most three workers");
        for (unsigned workers : {0u, 1u, 3u})
        {
            ParallelJobs pool(workers);
            Require(pool.Workers() == workers, "the workers asked for");
            Require(pool.Table()->workers == workers, "the table's workers");
            Check(pool, 0, "an empty batch");
            Check(pool, 1, "one job");
            // Batches right after each other (workers spinning), many of them.
            for (unsigned round = 0; round < 20000; ++round) Check(pool, 1 + round % 12, "back to back");
            // After a pause the workers sleep; the next batch wakes them.
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            Check(pool, 10, "after the workers slept");
            // Through the C table the module gets.
            Batch batch(10);
            pool.Table()->start(Count, &batch, 10);
            pool.Table()->finish();
            for (unsigned i = 0; i < 10; ++i) Require(batch.runs[i].load() == 1, "the table's batch");
        }
        // Finish waits for a job a worker still runs.
        {
            ParallelJobs pool(2);
            std::atomic<int> finished{0};
            struct Slow
            {
                std::atomic<int>* finished;
            } slow{&finished};
            pool.Start(
                [](void* context, unsigned) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(30));
                    static_cast<Slow*>(context)->finished->fetch_add(1);
                },
                &slow, 3);
            pool.Finish();
            Require(finished.load() == 3, "Finish returns after every job");
        }
        std::printf("parallel_jobs: %d checks passed\n", sChecks);
        return 0;
    }
    catch (const std::exception& error)
    {
        std::fprintf(stderr, "parallel_jobs: FAILED: %s (after %d checks)\n", error.what(), sChecks);
        return 1;
    }
}
