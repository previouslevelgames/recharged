// Incremental rollback snapshots (src/platform/rollback_snapshot.cpp) against a
// model: random writes, saves and restores to any kept save, on regions that
// share pages with memory they do not own.
#include "platform/rollback_snapshot.h"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <map>
#include <random>
#include <thread>
#include <vector>

using namespace mscharged::platform;

namespace
{
int failures = 0;

std::size_t PageSize()
{
#if defined(_WIN32)
    SYSTEM_INFO info;
    GetSystemInfo(&info);
    return info.dwPageSize;
#else
    return static_cast<std::size_t>(sysconf(_SC_PAGESIZE));
#endif
}

std::uint8_t* MapPages(std::size_t bytes)
{
#if defined(_WIN32)
    return static_cast<std::uint8_t*>(VirtualAlloc(nullptr, bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
#else
    return static_cast<std::uint8_t*>(mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
#endif
}

void UnmapPages(std::uint8_t* block, std::size_t bytes)
{
#if defined(_WIN32)
    (void)bytes;
    VirtualFree(block, 0, MEM_RELEASE);
#else
    munmap(block, bytes);
#endif
}

void Check(bool passed, const char* message)
{
    if (passed) return;
    std::cerr << message << '\n';
    ++failures;
}

struct Fixture
{
    std::size_t page = PageSize();
    std::size_t bytes = 64 * page;
    std::uint8_t* block = nullptr;
    std::vector<RollbackRegion> regions;
    std::vector<std::uint8_t> owned; // 1 where a region owns the byte

    Fixture()
    {
        block = MapPages(bytes);
        // Starts and ends inside pages; a gap of whole pages; a hole inside a page.
        regions.push_back({block + page + 100, 19 * page + 200});
        regions.push_back({block + 30 * page, 10 * page + 1000});
        regions.push_back({block + 40 * page + 2000, 20 * page - 2000 + 64});
        owned.assign(bytes, 0);
        for (const auto& region : regions)
        {
            const auto offset = static_cast<std::size_t>(static_cast<std::uint8_t*>(region.base) - block);
            std::fill(owned.begin() + static_cast<long>(offset),
                      owned.begin() + static_cast<long>(offset + region.bytes), 1);
        }
    }
    ~Fixture() { UnmapPages(block, bytes); }

    // Addresses of the owned bytes that differ from saved (an Owned() state).
    std::vector<std::uintptr_t> Changed(const std::vector<std::uint8_t>& saved) const
    {
        std::vector<std::uintptr_t> changed;
        std::size_t k = 0;
        for (std::size_t i = 0; i < bytes; ++i)
            if (owned[i] && block[i] != saved[k++]) changed.push_back(reinterpret_cast<std::uintptr_t>(block + i));
        return changed;
    }

    std::vector<std::uint8_t> Owned() const
    {
        std::vector<std::uint8_t> state;
        for (std::size_t i = 0; i < bytes; ++i)
            if (owned[i]) state.push_back(block[i]);
        return state;
    }
    std::vector<std::uint8_t> Foreign() const
    {
        std::vector<std::uint8_t> state;
        for (std::size_t i = 0; i < bytes; ++i)
            if (!owned[i]) state.push_back(block[i]);
        return state;
    }
};

// A hot set (a few pages written at every step) and scattered writes anywhere,
// the regions' bytes and the rest alike.
void Step(Fixture& f, std::mt19937& random)
{
    for (std::size_t p : {2u, 3u, 31u, 45u})
        f.block[p * f.page + random() % f.page] = static_cast<std::uint8_t>(random());
    const int scattered = static_cast<int>(random() % 6);
    for (int i = 0; i < scattered; ++i)
        f.block[random() % f.bytes] = static_cast<std::uint8_t>(random());
    if (random() % 8 == 0) std::memset(f.block + 50 * f.page + 10, static_cast<int>(random()), 3 * f.page);
}

void RandomHistory(std::size_t historyPages, int depth, int steps, unsigned seed)
{
    Fixture f;
    std::mt19937 random(seed);
    for (std::size_t i = 0; i < f.bytes; ++i) f.block[i] = static_cast<std::uint8_t>(random());
    Check(ConfigureRollbackSnapshots(f.regions.data(), static_cast<int>(f.regions.size()), depth, historyPages),
          "configure");

    for (int round = 0; round < 3; ++round) // End and Begin again keep the hot pages
    {
        std::map<std::uint64_t, std::vector<std::uint8_t>> model;
        std::uint64_t latest = BeginRollbackSnapshots();
        Check(latest != 0, "begin");
        model[latest] = f.Owned();
        for (int s = 0; s < steps; ++s)
        {
            Step(f, random);
            if (random() % 5 == 0 && OldestRollbackSnapshot() <= latest)
            {
                const std::uint64_t oldest = OldestRollbackSnapshot();
                const std::uint64_t target = oldest + random() % (latest - oldest + 1);
                const auto foreign = f.Foreign();
                Check(RestoreRollbackSnapshot(target), "restore a kept save");
                Check(f.Owned() == model[target], "restored bytes differ from the save");
                Check(f.Foreign() == foreign, "a restore wrote bytes the regions do not own");
                Check(!RestoreRollbackSnapshot(target + 1), "a save after the restored one is gone");
                latest = target;
                if (oldest > 1) Check(!RestoreRollbackSnapshot(oldest - 1), "a dropped save cannot be restored");
                continue;
            }
            std::uintptr_t first = 0;
            Check(FindUntrackedRollbackWrites(&first) == 0, "a write escaped the tracking");
            std::vector<std::uintptr_t> visited;
            VisitRollbackChanges(
                [](std::uintptr_t address, std::size_t length, void* context) {
                    auto& list = *static_cast<std::vector<std::uintptr_t>*>(context);
                    for (std::size_t i = 0; i < length; ++i) list.push_back(address + i);
                },
                &visited);
            std::sort(visited.begin(), visited.end());
            Check(visited == f.Changed(model[latest]), "the changes visited differ from the bytes written");
            latest = SaveRollbackSnapshot();
            model[latest] = f.Owned();
        }
        EndRollbackSnapshots();
    }
    const auto& stats = RollbackSnapshotStatistics();
    std::cout << "history " << historyPages << " pages, depth " << depth << ": saves " << stats.saves << ", restores "
              << stats.restores << ", faults " << stats.faults << ", promoted " << stats.promoted << ", demoted "
              << stats.demoted << ", evicted " << stats.evicted << '\n';
    Check(stats.demoted > 0, "idle hot pages turn cold");
    // Saves past the depth go as planned; only a full history drops them early.
    Check((stats.evicted == 0) == (historyPages >= 1024), "saves dropped early only for room");
}

// Another thread writes bytes the regions do not own on pages they share,
// while the main thread saves and restores.
void ForeignThread()
{
    Fixture f;
    Check(ConfigureRollbackSnapshots(f.regions.data(), static_cast<int>(f.regions.size()), 8, 256), "configure");
    std::map<std::uint64_t, std::vector<std::uint8_t>> model;
    model[BeginRollbackSnapshots()] = f.Owned();
    std::atomic<bool> stop{false};
    // Page 40 holds bytes [0, 1000) of the second region, a hole, then the third.
    std::thread writer([&] {
        unsigned value = 0;
        while (!stop.load(std::memory_order_relaxed))
            for (std::size_t i = 1000; i < 2000; i += 64) f.block[40 * f.page + i] = static_cast<std::uint8_t>(++value);
    });
    std::mt19937 random(7);
    for (int s = 0; s < 200; ++s)
    {
        f.block[40 * f.page + random() % 1000] = static_cast<std::uint8_t>(random());
        if (s % 20 == 19)
        {
            const std::uint64_t target = OldestRollbackSnapshot();
            Check(RestoreRollbackSnapshot(target), "restore with a foreign writer");
            Check(f.Owned() == model[target], "restore with a foreign writer differs");
        }
        else
            model[SaveRollbackSnapshot()] = f.Owned();
    }
    stop = true;
    writer.join();
    EndRollbackSnapshots();
}
}

// A page holds at most 255 regions: one more is refused, not miscounted.
void RegionsInOnePage()
{
    static std::uint8_t block[1 << 16];
    const std::size_t page = PageSize();
    auto* start = reinterpret_cast<std::uint8_t*>((reinterpret_cast<std::uintptr_t>(block) + page - 1) & ~(page - 1));
    std::vector<RollbackRegion> regions;
    for (int i = 0; i < 256; ++i) regions.push_back({start + 2 * i, 1});
    Check(ConfigureRollbackSnapshots(regions.data(), 255, 2, 16), "255 regions in a page");
    Check(!ConfigureRollbackSnapshots(regions.data(), 256, 2, 16), "256 regions in a page are refused");
}

int main()
{
    RandomHistory(1024, 8, 400, 1);  // room for everything
    RandomHistory(1024, 3, 400, 2);  // short history: the oldest saves drop
    RandomHistory(24, 8, 400, 3);    // little room: saves drop for room, some overflow
    ForeignThread();
    RegionsInOnePage();
    if (failures) std::cerr << failures << " failures\n";
    return failures ? 1 : 0;
}
