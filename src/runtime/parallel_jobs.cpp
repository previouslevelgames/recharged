#include "runtime/parallel_jobs.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__)
#include <immintrin.h>
#endif

namespace mscharged::runtime {
namespace {

void Pause()
{
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__)
    _mm_pause();
#elif defined(__aarch64__)
    asm volatile("yield");
#endif
}

// How long a worker spins for the next batch before it sleeps.
constexpr auto kSpin = std::chrono::microseconds(400);

ParallelJobs* sPool = nullptr;

} // namespace

// The hand-over: `generation` is even while a batch is published and odd
// while the caller writes the next one, which it does only once no worker is
// `active` in the last one. A worker counts itself active before it looks at
// the generation, so either the caller sees it and waits until it has left,
// or it sees the odd generation and stays out (both sides use sequentially
// consistent operations for this).
struct ParallelJobs::State {
    std::vector<std::thread> threads;
    std::atomic<unsigned> generation{0};
    std::atomic<unsigned> active{0};
    std::atomic<unsigned> next{0};
    std::atomic<unsigned> done{0};
    std::atomic<unsigned> sleeping{0};
    std::atomic<bool> stop{false};
    ChargedParallelJob job = nullptr;
    void* context = nullptr;
    unsigned count = 0;
    std::mutex mutex;
    std::condition_variable wake;
    ChargedParallelJobs table{};

    void RunJobs(ChargedParallelJob batchJob, void* batchContext, unsigned batchCount)
    {
        for (;;) {
            const unsigned index = next.fetch_add(1);
            if (index >= batchCount) return;
            batchJob(batchContext, index);
            done.fetch_add(1, std::memory_order_release);
        }
    }

    void Work()
    {
        unsigned seen = 0;
        for (;;) {
            // The next batch: spinning for a moment, then asleep.
            const auto ready = [&] {
                const unsigned g = generation.load();
                return stop.load() || ((g & 1) == 0 && g != seen);
            };
            const auto spinUntil = std::chrono::steady_clock::now() + kSpin;
            for (unsigned spins = 0; !ready(); ++spins) {
                Pause();
                if ((spins & 255) != 255 || std::chrono::steady_clock::now() < spinUntil) continue;
                std::unique_lock<std::mutex> lock(mutex);
                sleeping.fetch_add(1);
                wake.wait(lock, ready);
                sleeping.fetch_sub(1);
                break;
            }
            if (stop.load()) return;
            active.fetch_add(1);
            const unsigned g = generation.load();
            if ((g & 1) != 0 || g == seen) {
                active.fetch_sub(1);
                continue;
            }
            seen = g;
            RunJobs(job, context, count);
            active.fetch_sub(1);
        }
    }
};

ParallelJobs::ParallelJobs(unsigned workers) : state_(std::make_unique<State>())
{
    state_->table.workers = workers;
    state_->table.start = [](ChargedParallelJob job, void* context, unsigned count) {
        if (sPool) sPool->Start(job, context, count);
    };
    state_->table.finish = [] {
        if (sPool) sPool->Finish();
    };
    for (unsigned n = 0; n < workers; ++n) state_->threads.emplace_back([state = state_.get()] { state->Work(); });
    sPool = this;
}

ParallelJobs::~ParallelJobs()
{
    if (sPool == this) sPool = nullptr;
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        state_->stop.store(true);
    }
    state_->wake.notify_all();
    for (auto& thread : state_->threads) thread.join();
}

unsigned ParallelJobs::DefaultWorkers(unsigned hardware_threads)
{
    if (hardware_threads < 3) return 0;
    return std::min(3u, hardware_threads / 2);
}

unsigned ParallelJobs::Workers() const { return static_cast<unsigned>(state_->threads.size()); }

const ChargedParallelJobs* ParallelJobs::Table() const { return &state_->table; }

void ParallelJobs::Start(ChargedParallelJob job, void* context, unsigned count)
{
    State& s = *state_;
    s.generation.fetch_add(1); // odd: no worker enters now
    while (s.active.load() != 0) Pause();
    s.job = job;
    s.context = context;
    s.count = count;
    s.next.store(0);
    s.done.store(0);
    s.generation.fetch_add(1); // even: published
    if (s.sleeping.load() != 0) {
        { std::lock_guard<std::mutex> lock(s.mutex); }
        s.wake.notify_all();
    }
}

void ParallelJobs::Finish()
{
    State& s = *state_;
    s.RunJobs(s.job, s.context, s.count);
    while (s.done.load(std::memory_order_acquire) < s.count) Pause();
}

} // namespace mscharged::runtime
