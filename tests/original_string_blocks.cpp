#include "NL/nlString.h"
#include <array>
#include <cstddef>
#include <cstring>
#include <iostream>
#include <set>
#include <stdexcept>
#include <vector>

namespace
{
unsigned checks;
void Check(bool condition, const char* message)
{
    ++checks;
    if (!condition) throw std::runtime_error(message);
}
}

int main()
{
    try
    {
        // Retained test backing, with actual original allocators. This is not
        // production OS/pre-main initialization or the original global owner.
        std::vector<std::byte> mem1(256 * 1024), mem2(128 * 1024);
        StandardAllocator.Initialize(mem1.data(), mem1.size());
        VirtualAllocator.Initialize(mem2.data(), mem2.size());
        CurrentAllocator = &StandardAllocator;
        AllocatorStack[0] = CurrentAllocator;
        AllocatorStackDepth = 1;
        gMemoryInitialized = 1;
        const auto total = StandardAllocator.TotalFreeMemory();
        const auto largest = StandardAllocator.LargestFreeBlock();

        alignas(Detail::StringBlockAllocator)
            std::array<std::byte, sizeof(Detail::StringBlockAllocator)> ownerStorage;
        auto* owner = ::new (ownerStorage.data()) Detail::StringBlockAllocator;
        auto* canary = static_cast<unsigned char*>(nlMalloc(1024));
        std::memset(canary, 0xA7, 1024);
        std::array<Detail::StringBlock*, 64> blocks;
        std::set<void*> seen;
        for (unsigned i = 0; i < blocks.size(); ++i)
        {
            blocks[i] = static_cast<Detail::StringBlock*>(owner->Allocate(1));
            Check(blocks[i] != nullptr, "Original sixty-four-block pool exhausted early");
            Check(seen.insert(blocks[i]).second, "Original block allocator returned a live alias");
            if (i) Check(blocks[i] == blocks[0] + i, "Native storage changed original block order");
            for (unsigned j = 0; j < sizeof(blocks[i]->storage); ++j)
                blocks[i]->storage[j] = static_cast<unsigned char>((i * 37 + j * 13) ^ 0x5A);
        }
        Check(owner->Allocate(1) == nullptr, "Original pool gained an extra block");
        for (unsigned i = 0; i < 64; ++i)
            for (unsigned j = 0; j < sizeof(blocks[i]->storage); ++j)
                Check(blocks[i]->storage[j] == static_cast<unsigned char>((i * 37 + j * 13) ^ 0x5A),
                    "Full native block payload overlapped another allocation");
        for (unsigned i = 0; i < 1024; ++i)
            Check(canary[i] == 0xA7, "Final native string block crossed its owned allocation");

        for (unsigned i = 0; i < 64; ++i) owner->Free(blocks[(i * 17) % 64]);
        for (unsigned i = 0; i < 64; ++i)
            Check(owner->Allocate(1) == blocks[((63 - i) * 17) % 64],
                "Native storage changed original free-list reuse order");
        Check(owner->Allocate(1) == nullptr, "Reused original pool gained a block");

        // Retire only this fixture's backing. The original global destructor's
        // null branch is preserved and deliberately not exercised by this test.
        nlFree(canary);
        nlFree(blocks[0]);
        Check(StandardAllocator.TotalFreeMemory() == total, "String backing leaked arena capacity");
        Check(StandardAllocator.LargestFreeBlock() == largest, "String backing fragmented its arena");
        std::cout << "Original string block constructor/storage/free-list: " << checks
                  << " checks; global startup/CRT teardown remain unqualified\n";
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
