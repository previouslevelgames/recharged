// The registry's rollback checkpoint (src/platform/game_allocation_ownership.cpp)
// against snapshots of its answers: random allocations, frees, completed byte
// writes, graphics suballocations from a pool that marks and releases like
// GLXResourcePool, native publications and a frame block left out of the
// rollback, with a mark after every step, rollbacks to any kept mark and a
// trimmed journal. After a rollback the arena (the original allocator's
// memory, with the pool's cursor and marks in it) and the registry must answer
// as they did at the mark, the frame block as it did just before, and the
// game must be able to allocate over what the rollback gave back.
#include "NL/MemAlloc.h"
#include "platform/game_allocation_ownership.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <exception>
#include <new>
#include <random>
#include <vector>

extern "C" void* ChargedNativeMetadataAllocate(std::size_t bytes) { return ::operator new(bytes); }
extern "C" void ChargedNativeMetadataRelease(void* p) noexcept { ::operator delete(p); }

using namespace mscharged::platform;

namespace
{
int failures = 0;
void Check(bool passed, const char* message)
{
    if (passed) return;
    std::fprintf(stderr, "%s\n", message);
    ++failures;
}

constexpr std::size_t kArena = 1 << 20;
constexpr std::size_t kPool = 64 << 10, kData = 32 << 10, kFrame = 16 << 10;
constexpr int kSlots = 48, kMarks = 16, kDepth = 6;

// The game's own state, in the arena: a rollback restores it with the bytes.
struct Control
{
    std::size_t cursor;          // the pool's top
    int depth;                   // the pool's marks
    std::size_t marks[kMarks];
    unsigned char* slots[kSlots]; // small allocations
};

struct World
{
    alignas(64) std::array<unsigned char, kArena> arena{};
    MemoryAllocator heap{};
    Control* control = nullptr;
    unsigned char* pool = nullptr;
    unsigned char* data = nullptr;
    unsigned char* frame = nullptr; // left out of the rollback, like the frame memory
    std::size_t frameCursor = 0;    // host-side, like glxMemory's statics
};
World* sWorld;

bool KeepFrame(std::uintptr_t address)
{
    const auto base = reinterpret_cast<std::uintptr_t>(sWorld->frame);
    return address >= base && address - base < kFrame;
}

// The registry's answers over a region, every 16 bytes.
void Digest(std::vector<std::uint64_t>& out, const unsigned char* region, std::size_t bytes)
{
    for (std::size_t offset = 0; offset < bytes; offset += 16)
    {
        const void* p = region + offset;
        GameGraphicsStorageSpan graphics{};
        const bool hasGraphics = FindGameGraphicsStorage(p, 1, graphics);
        out.push_back(hasGraphics);
        if (hasGraphics)
        {
            out.push_back(reinterpret_cast<std::uintptr_t>(graphics.base));
            out.push_back(graphics.bytes);
            out.push_back(graphics.incarnation);
        }
        GameCompletedSpan completed{};
        const bool hasCompleted = FindGameCompletedSpan(p, 1, completed);
        out.push_back(hasCompleted);
        if (hasCompleted)
        {
            out.push_back(reinterpret_cast<std::uintptr_t>(completed.base));
            out.push_back(completed.bytes);
            out.push_back(static_cast<std::uint64_t>(FindGameByteDomain(p, 1)));
        }
    }
}

std::vector<std::uint64_t> Restored(const World& w)
{
    std::vector<std::uint64_t> out;
    Digest(out, w.pool, kPool);
    Digest(out, w.data, kData);
    for (auto* slot : w.control->slots)
    {
        GameAllocationSpan span{};
        const bool live = slot && FindGameAllocationSpan(slot, 1, span);
        out.push_back(live);
        if (live)
        {
            out.push_back(span.bytes);
            out.push_back(span.incarnation);
        }
        if (slot) Digest(out, slot, 64);
    }
    return out;
}

std::vector<std::uint64_t> FrameDigest(const World& w)
{
    std::vector<std::uint64_t> out;
    Digest(out, w.frame, kFrame);
    return out;
}

void Step(World& w, std::mt19937& random)
{
    Control& c = *w.control;
    for (int op = 0; op < 6; ++op)
    {
        switch (random() % 7)
        {
        case 0: // pool allocation, often published as native vertices
        {
            const std::size_t bytes = 16 * (1 + random() % 16);
            if (c.cursor + bytes > kPool) break;
            unsigned char* at = w.pool + c.cursor;
            GameGraphicsStorageReservation storage(bytes);
            storage.Commit(at);
            c.cursor += bytes;
            if (random() % 2)
            {
                std::memset(at, static_cast<int>(random()), bytes);
                PublishGameGraphicsNativeBytes(at, at + bytes - 16 * (random() % 2));
            }
            break;
        }
        case 1: // a pool mark
            if (c.depth < kMarks) c.marks[c.depth++] = c.cursor;
            break;
        case 2: // a pool release to its last mark, as ReleaseResource does
            if (c.depth)
            {
                c.cursor = c.marks[--c.depth];
                GameGraphicsRetirementReservation retire;
                retire.Commit(w.pool + c.cursor, kPool - c.cursor);
            }
            break;
        case 3: // a completed write in the data block
        {
            const std::size_t bytes = 8 * (1 + random() % 64);
            const std::size_t at = 8 * (random() % ((kData - bytes) / 8));
            GameByteWriteReservation write(w.data + at, bytes);
            std::memset(w.data + at, static_cast<int>(random()), bytes);
            write.Complete(random() % 2 ? GameByteDomain::WiiSerialized : GameByteDomain::NativePayload);
            break;
        }
        case 4: // a small allocation, its bytes completed
        {
            auto& slot = c.slots[random() % kSlots];
            if (slot) break;
            slot = static_cast<unsigned char*>(w.heap.Allocate(64, 32, false));
            GameByteWriteReservation write(slot, 64);
            std::memset(slot, static_cast<int>(random()), 64);
            write.Complete(GameByteDomain::WiiSerialized);
            break;
        }
        case 5: // a small free, with the facts it holds
        {
            auto& slot = c.slots[random() % kSlots];
            if (!slot) break;
            w.heap.Free(slot);
            slot = nullptr;
            break;
        }
        default: // the frame block: suballocations, then all of it retired at a new frame
            if (random() % 4 == 0)
            {
                GameGraphicsRetirementReservation retire;
                retire.Commit(w.frame, kFrame);
                w.frameCursor = 0;
            }
            else if (w.frameCursor + 64 <= kFrame)
            {
                GameGraphicsStorageReservation storage(64);
                storage.Commit(w.frame + w.frameCursor);
                w.frameCursor += 64;
            }
            break;
        }
    }
}

struct Saved
{
    GameAllocationMark mark;
    std::vector<unsigned char> arena;
    std::vector<unsigned char> heap; // the allocator object, outside the arena
    std::vector<std::uint64_t> digest;
};

void Run(unsigned seed, int steps)
{
    auto* w = new World;
    sWorld = w;
    w->heap.Initialize(w->arena.data(), static_cast<unsigned int>(w->arena.size()));
    w->control = static_cast<Control*>(w->heap.Allocate(sizeof(Control), 32, false));
    std::memset(w->control, 0, sizeof(Control));
    w->pool = static_cast<unsigned char*>(w->heap.Allocate(kPool, 32, false));
    w->data = static_cast<unsigned char*>(w->heap.Allocate(kData, 32, false));
    w->frame = static_cast<unsigned char*>(w->heap.Allocate(kFrame, 32, false));
    std::mt19937 random(seed);

    BeginGameAllocationCheckpoint(KeepFrame);
    std::deque<Saved> saved;
    auto save = [&] {
        Saved s;
        s.mark = MarkGameAllocationCheckpoint();
        s.arena.assign(w->arena.begin(), w->arena.end());
        const auto* heap = reinterpret_cast<const unsigned char*>(&w->heap);
        s.heap.assign(heap, heap + sizeof(MemoryAllocator));
        s.digest = Restored(*w);
        saved.push_back(std::move(s));
        while (saved.size() > kDepth) saved.pop_front();
        TrimGameAllocationCheckpoint(saved.front().mark);
    };
    save();
    int rollbacks = 0;
    for (int s = 0; s < steps; ++s)
    {
        try
        {
            Step(*w, random);
        }
        catch (const std::exception& e)
        {
            std::fprintf(stderr, "step %d: %s\n", s, e.what());
            ++failures;
            break;
        }
        if (random() % 4 == 0)
        {
            const std::size_t back = random() % saved.size();
            const Saved& to = saved[saved.size() - 1 - back];
            const auto frameBefore = FrameDigest(*w);
            std::memcpy(w->arena.data(), to.arena.data(), kArena);
            std::memcpy(static_cast<void*>(&w->heap), to.heap.data(), sizeof(MemoryAllocator));
            Check(RollbackGameAllocationCheckpointTo(to.mark, KeepFrame) == 0, "a free could not be undone");
            Check(Restored(*w) == to.digest, "the registry does not answer as it did at the mark");
            Check(FrameDigest(*w) == frameBefore, "the rollback changed the block it leaves alone");
            saved.resize(saved.size() - back);
            ++rollbacks;
            continue; // the next step allocates over what the rollback gave back
        }
        save();
    }
    const auto stats = CurrentGameAllocationCheckpointStats();
    std::printf("seed %u: %d steps, %d rollbacks, %llu copies, %zu journal entries held, %llu untracked, %llu "
                "failed copies\n",
                seed, steps, rollbacks, static_cast<unsigned long long>(stats.copies), stats.entries,
                static_cast<unsigned long long>(stats.untracked), static_cast<unsigned long long>(stats.failedCopies));
    Check(stats.copies > 0, "no copies were journaled");
    Check(stats.untracked == 0 && stats.failedCopies == 0, "changes escaped the journal");
    Check(stats.entries < 2000, "the trimmed journal keeps growing");
    EndGameAllocationCheckpoint();
    DiscardGameAllocatorRecords(w->heap);
    delete w;
}
}

int main()
{
    for (unsigned seed = 1; seed <= 8; ++seed) Run(seed, 600);
    if (failures) std::fprintf(stderr, "%d failures\n", failures);
    return failures ? 1 : 0;
}
