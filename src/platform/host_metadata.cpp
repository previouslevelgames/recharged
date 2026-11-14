#include "platform/host_metadata.h"

#include <cstddef>
#include <new>

// Compile and link this provider outside the original-game module. Its standard
// operators belong to the host; importing these functions never imports the
// host's C++ allocator or container objects into the game's source graph.

namespace
{
// The game's memory-ownership registry allocates and frees about 5000 small
// nodes here in every match frame. Freed blocks wait in size-class lists for
// the next request instead of going back to the C library.
constexpr std::size_t kHeader = alignof(std::max_align_t);
constexpr std::size_t kGranule = 16;
constexpr std::size_t kClasses = 64; // blocks up to 1 KiB
struct FreeBlock
{
    FreeBlock* next;
};
// Per thread, without a lock; a thread that ends gives its blocks back. A
// list keeps at most kKept blocks, so a thread that frees what others
// allocated does not collect them without end.
constexpr unsigned kKept = 8192;
// Set once the thread's pool is gone (thread or process exit): later blocks
// go straight to the C library.
thread_local bool tPoolGone = false;
struct ThreadPool
{
    FreeBlock* free[kClasses + 1] = {};
    unsigned count[kClasses + 1] = {};
    ~ThreadPool()
    {
        tPoolGone = true;
        for (auto* block : free)
            while (block)
            {
                auto* next = block->next;
                ::operator delete(reinterpret_cast<unsigned char*>(block) - kHeader);
                block = next;
            }
    }
};
thread_local ThreadPool tPool;
}

extern "C" void* ChargedNativeMetadataAllocate(std::size_t bytes)
{
    const std::size_t sizeClass = tPoolGone ? kClasses + 1 : bytes == 0 ? 1 : (bytes + kGranule - 1) / kGranule;
    if (sizeClass <= kClasses)
    {
        if (FreeBlock* block = tPool.free[sizeClass])
        {
            tPool.free[sizeClass] = block->next;
            --tPool.count[sizeClass];
            return block;
        }
    }
    // The header keeps the size class (0: not pooled) in front of the block.
    const std::size_t payload = sizeClass <= kClasses ? sizeClass * kGranule : bytes;
    auto* raw = static_cast<unsigned char*>(::operator new(payload + kHeader));
    *reinterpret_cast<std::size_t*>(raw) = sizeClass <= kClasses ? sizeClass : 0;
    return raw + kHeader;
}

extern "C" void ChargedNativeMetadataRelease(void* pointer) noexcept
{
    if (pointer == nullptr) return;
    auto* raw = static_cast<unsigned char*>(pointer) - kHeader;
    const std::size_t sizeClass = *reinterpret_cast<const std::size_t*>(raw);
    if (sizeClass == 0 || tPoolGone || tPool.count[sizeClass] >= kKept)
    {
        ::operator delete(raw);
        return;
    }
    auto* block = static_cast<FreeBlock*>(pointer);
    block->next = tPool.free[sizeClass];
    tPool.free[sizeClass] = block;
    ++tPool.count[sizeClass];
}
