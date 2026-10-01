#include "NL/MemAlloc.h"
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <new>
#include <random>
#include <stdexcept>
#include <vector>

namespace
{
void Require(bool value, const char* message)
{ if (!value) throw std::runtime_error(message); }
template<class Exception, class Action> void Reject(Action action)
{
    try { action(); }
    catch (const Exception&) { return; }
    throw std::runtime_error("Invalid allocation operation was accepted");
}
struct Live { void* pointer; unsigned size; unsigned char pattern; };
void Verify(const std::vector<Live>& live)
{
    for (const auto& block : live)
        for (unsigned i = 0; i < block.size; ++i)
            Require(static_cast<unsigned char*>(block.pointer)[i] == block.pattern,
                    "Allocation or free corrupted a surviving payload");
}
}

int main()
{
    try
    {
        alignas(64) std::array<std::byte, 65536> arena{};
        MemoryAllocator allocator{};
        Reject<std::bad_alloc>([&] { allocator.Allocate(16, 8, false); });
        Reject<std::invalid_argument>([&] { allocator.Initialize(arena.data()+1, 1024); });
        Reject<std::invalid_argument>([&] { allocator.Initialize(arena.data(), 15); });
        allocator.Initialize(arena.data(), arena.size());
        Reject<std::invalid_argument>([&] { allocator.Allocate(16, 3, false); });
        Reject<std::invalid_argument>([&] { allocator.Allocate(16, 0, true); });
        Reject<std::bad_alloc>([&] { allocator.Allocate(1UL << 30, 8, false); });
        Reject<std::bad_alloc>([&] { allocator.Allocate(arena.size(), 8, true); });
        Require(allocator.TotalFreeMemory() == arena.size(), "Rejected requests consumed memory");

        // Exhaust the free list completely, then reconstruct it from one free.
        void* whole = allocator.Allocate(arena.size()-alignof(FreeBlockList), 8, false);
        Require(allocator.TotalFreeMemory() == 0 && allocator.LargestFreeBlock() == 0,
                "A full allocation left free blocks");
        Reject<std::bad_alloc>([&] { allocator.Allocate(1, 8, false); });
        allocator.Free(whole);
        Require(allocator.TotalFreeMemory() == arena.size(), "Full-arena allocation did not recover");
        Reject<std::invalid_argument>([&] { allocator.Free(whole); });
        int foreign = 0;
        Reject<std::invalid_argument>([&] { allocator.Free(&foreign); });
        allocator.Free(nullptr);

        for (bool end : {false, true})
            for (unsigned alignment : {1u,2u,4u,8u,16u,32u,64u,128u,256u,1024u})
                for (unsigned size : {0u,1u,3u,12u,23u,24u,25u,31u,33u,127u})
                {
                    void* pointer = allocator.Allocate(size, alignment, end);
                    Require(reinterpret_cast<std::uintptr_t>(pointer) % std::max<unsigned>(alignment, alignof(FreeBlockList)) == 0,
                            "Game payload alignment is incorrect");
                    std::memset(pointer, 0xAB, size);
                    allocator.Free(pointer);
                    Require(allocator.TotalFreeMemory() == arena.size()
                        && allocator.LargestFreeBlock() == arena.size(), "Split blocks did not coalesce");
                }

        std::mt19937 random(0x43485247);
        std::vector<Live> live;
        unsigned exhausted = 0;
        for (unsigned step = 0; step < 6000; ++step)
        {
            Verify(live);
            if (live.empty() || random()%100 < 60)
            {
                const unsigned size = 1 + random()%1024;
                const unsigned alignment = 8u << (random()%6);
                try
                {
                    void* pointer = allocator.Allocate(size, alignment, random()%2);
                    const auto address = reinterpret_cast<std::uintptr_t>(pointer);
                    const auto base = reinterpret_cast<std::uintptr_t>(arena.data());
                    Require(address >= base && address+size <= base+arena.size(), "Payload left its arena");
                    Require(address%alignment == 0, "Mixed allocation lost alignment");
                    for (const auto& previous : live)
                    {
                        const auto other = reinterpret_cast<std::uintptr_t>(previous.pointer);
                        Require(address+size <= other || other+previous.size <= address, "Live payloads overlap");
                    }
                    const auto pattern = static_cast<unsigned char>(1 + step%254);
                    std::memset(pointer, pattern, size);
                    live.push_back({pointer, size, pattern});
                }
                catch (const std::bad_alloc&) { ++exhausted; }
            }
            else
            {
                const auto index = random()%live.size();
                allocator.Free(live[index].pointer);
                live.erase(live.begin()+index);
            }
        }
        Verify(live);
        std::shuffle(live.begin(), live.end(), random);
        for (const auto& block : live) allocator.Free(block.pointer);
        Require(exhausted > 0, "Mixed workload did not exercise exhaustion");
        Require(allocator.TotalFreeMemory() == arena.size()
            && allocator.LargestFreeBlock() == arena.size(), "Mixed workload leaked or fragmented memory");
        std::cout << "Original native allocator: alignment, exhaustion, payload preservation, and coalescing passed; address above 4 GiB: "
                  << (reinterpret_cast<std::uintptr_t>(arena.data()) > UINT32_MAX ? "yes" : "no") << '\n';
        return 0;
    }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
