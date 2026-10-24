// The audio plays on through rollbacks, outside them: everything it makes
// lives in a heap of its own carved from MEM2 (the game enters the audio
// through MSCHARGED_AUDIO_SCOPE, tweak patch 0031), which no rollback
// restores, with its globals and the disc reads' manager (its streams read
// while rollbacks happen); a replayed frame starts, stops, pauses and resumes
// no sound. RECHARGED_ROLLBACK_AUDIO_AUDIT=1 lists the call stacks of the
// allocations made outside the audio's scope (<file>.summary.allocs): one
// through audio code is audio state the rollback would restore.
#include "platform/rollback_state.h"

namespace mscharged::rollback
{

// The audio's heap: everything the audio makes, from its creation on, lives
// here (the game enters the audio through MSCHARGED_AUDIO_SCOPE), outside
// the rollback: the sound the audio describes plays on through rollbacks. It
// is carved from MEM2, as the audio's samples must be where the DSP reads.
MemoryAllocator sAudioAllocator;
void* sAudioHeap;

namespace
{
thread_local int tAudioDepth;


// RECHARGED_ROLLBACK_AUDIO_AUDIT=1: the call stacks (module offsets) of the
// allocations made outside the audio's scope while tracking, each distinct one
// once, in <file>.allocs at the end: a stack through audio code is audio state
// in memory the rollback restores.
constexpr int kAuditStacks = 4096;
constexpr int kAuditFrames = 14;
struct AuditStack
{
    std::uintptr_t frames[kAuditFrames];
    int count;
    std::uint64_t hits;
};
AuditStack* sAuditStacks;
int sAuditCount;
bool sAuditing;
thread_local bool tInAudit;

void AuditAllocation()
{
    if (tInAudit) return;
    tInAudit = true;
    void* frames[kAuditFrames + 2];
    const int depth = mscharged::platform::RollbackCallStack(frames, kAuditFrames + 2);
    AuditStack stack{};
    const std::uintptr_t self = mscharged::platform::RollbackModuleBase(reinterpret_cast<void*>(&AuditAllocation));
    for (int i = 2; i < depth && stack.count < kAuditFrames; ++i)
        stack.frames[stack.count++] = reinterpret_cast<std::uintptr_t>(frames[i]) - self - 1;
    for (int i = 0; i < sAuditCount; ++i)
        if (sAuditStacks[i].count == stack.count
            && !std::memcmp(sAuditStacks[i].frames, stack.frames, sizeof(std::uintptr_t) * stack.count))
        {
            ++sAuditStacks[i].hits;
            tInAudit = false;
            return;
        }
    if (sAuditCount < kAuditStacks)
    {
        stack.hits = 1;
        sAuditStacks[sAuditCount++] = stack;
    }
    tInAudit = false;
}
} // namespace

void StartAudioAudit()
{
    sAuditStacks = static_cast<AuditStack*>(std::calloc(kAuditStacks, sizeof(AuditStack)));
    sAuditing = sAuditStacks != nullptr;
}

void WriteAudit(const char* summaryPath)
{
    if (!sAuditStacks) return;
    char path[4096];
    std::snprintf(path, sizeof(path), "%.4000s.allocs", summaryPath);
    FILE* file = std::fopen(path, "w");
    if (!file) return;
    for (int i = 0; i < sAuditCount; ++i)
    {
        std::fprintf(file, "%llu", static_cast<unsigned long long>(sAuditStacks[i].hits));
        for (int f = 0; f < sAuditStacks[i].count; ++f)
            std::fprintf(file, " 0x%lx", static_cast<unsigned long>(sAuditStacks[i].frames[f]));
        std::fprintf(file, "\n");
    }
    std::fclose(file);
}
} // namespace mscharged::rollback

using namespace mscharged::rollback;

extern "C" void mscharged_rollback_audio_enter(void)
{
    ++tAudioDepth;
}

extern "C" void mscharged_rollback_audio_leave(void)
{
    --tAudioDepth;
}

extern "C" void* mscharged_rollback_audio_allocator(void)
{
    if (!tAudioDepth)
    {
        if (sAuditing && sState) AuditAllocation();
        return nullptr;
    }
    if (!sState) return nullptr;
    if (!sAudioHeap)
    {
        std::fprintf(stderr, "rollback: MEM2 %u bytes, %u free (largest %u) before the audio heap\n",
                     VirtualAllocator.m_memory_size, VirtualAllocator.TotalFreeMemory(),
                     VirtualAllocator.LargestFreeBlock());
        void* heap = VirtualAllocator.Allocate(kAudioHeapBytes, 32, false);
        if (!heap) return nullptr;
        sAudioAllocator.Initialize(heap, static_cast<unsigned int>(kAudioHeapBytes));
        mscharged::platform::IgnoreGameByteProducersIn(heap, kAudioHeapBytes);
        sAudioHeap = heap;
    }
    return &sAudioAllocator;
}
