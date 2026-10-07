#pragma once

// Source pool/lifetime fixture; not a network/factory initialization qualifier.
#include "NL/nlSlotPool.h"
#include "NL/nlSmallBlockAllocator.h"
#include "NL/nlMemory.h"
#include "NL/MemAlloc.h"
#include "Game/DetermDataEvent.h"
#include "platform/game_allocation_ownership.h"
#include <cstdint>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace mscharged::testing::slot_stride {
inline unsigned checks;
inline void Check(bool ok, const char* message) {
    ++checks;
    if (!ok) throw std::runtime_error(message);
}
inline SlotPoolFreeFunc sourceFree;
inline void* expectedFree[3];
inline unsigned releasedBlocks;
inline void ObserveGrowingBlockFree(void* data) {
    Check(releasedBlocks < 3 && data == expectedFree[releasedBlocks],
          "Source initial/delta blocks did not retire newest to oldest");
    ++releasedBlocks;
    sourceFree(data);
}

inline void QualifyOriginalEventPool() {
    using namespace mscharged::platform;
    static_assert(sizeof(DetermDataEvent) == 0x24);
    static_assert(sizeof(SlotPoolEntry) == 8);
    constexpr unsigned stride = 40;
    Check(gDetermDataEventPool.m_Initial == 10 && gDetermDataEventPool.m_Delta == 0,
          "Original eager event pool counts changed");
    auto* block = gDetermDataEventPool.m_BlockList;
    GameAllocationSpan span{};
    Check(block && !block->next && FindGameAllocationSpan(block, sizeof(*block), span) &&
          span.bytes == 408 && span.owner == &VirtualAllocator,
          "Original ten-event pool does not have its true native backing");
    Check(reinterpret_cast<unsigned char*>(block) == static_cast<unsigned char*>(span.base) + 400,
          "Actual event block footer lies outside its ten physical slots");
    auto* free = gDetermDataEventPool.m_FreeList;
    for (unsigned i = 0; i < 10; ++i) {
        Check(free && reinterpret_cast<unsigned char*>(free) ==
              static_cast<unsigned char*>(span.base) + (9-i)*stride &&
              (reinterpret_cast<std::uintptr_t>(free) & 7) == 0,
              "Original source free-list order or native slot alignment changed");
        free = free->next;
    }
    Check(!free, "Native transport changed the source ten-event capacity");
    DetermDataEvent* event[10]{};
    unsigned char payload[32];
    for (unsigned i = 0; i < 10; ++i) {
        std::memset(payload, 0x20+i, sizeof(payload));
        event[i] = new DetermDataEvent(payload, sizeof(payload));
        Check(event[i] && event[i]->mSize == 32,
              "Original event class allocator/constructor changed payload size");
        for (unsigned j = 0; j < 32; ++j)
            Check(event[i]->mData[j] == 0x20+i,
                  "Original event payload crossed another native free-list slot");
    }
    Check(!gDetermDataEventPool.Allocate() && gDetermDataEventPool.m_BlockList == block,
          "Original zero-delta exhaustion grew the event pool");
    for (auto* p : event) delete p;
    Check(gDetermDataEventPool.Allocate() == event[9],
          "Original event delete/free-list reuse order changed");
    gDetermDataEventPool.Free(event[9]);
    gDetermDataEventPool.FreeBlocks();
    Check(!gDetermDataEventPool.m_BlockList && !gDetermDataEventPool.m_FreeList &&
          !FindGameAllocationOwner(span.base),
          "Original event pool block cleanup lost native allocation retirement");
    const auto freeBefore = VirtualAllocator.TotalFreeMemory();
    const auto largestBefore = VirtualAllocator.LargestFreeBlock();
    GameAllocationSpan growing[3]{};
    {
        SlotPool<DetermDataEvent> grow(3, 2);
        sourceFree = grow.m_FreeFn;
        releasedBlocks = 0;
        grow.m_FreeFn = ObserveGrowingBlockFree;
        DetermDataEvent* slots[7]{};
        for (unsigned i = 0; i < 7; ++i) {
            slots[i] = grow.Allocate();
            Check(slots[i] && (reinterpret_cast<std::uintptr_t>(slots[i]) & 7) == 0,
                  "Actual delta block slot alignment is invalid");
            std::memset(payload, 0x70+i, sizeof(payload));
            ::new (static_cast<void*>(slots[i])) DetermDataEvent(payload, sizeof(payload));
        }
        auto* node = grow.m_BlockList;
        for (unsigned i = 0; i < 3; ++i) {
            Check(node && FindGameAllocationSpan(node, sizeof(*node), growing[i]) &&
                  growing[i].bytes == (i == 2 ? 128u : 88u) &&
                  growing[i].owner == &VirtualAllocator,
                  "Original initial/delta block allocation count or native extent changed");
            expectedFree[i] = growing[i].base;
            node = node->next;
        }
        Check(!node, "Original growing pool created extra blocks");
        for (unsigned i = 0; i < 7; ++i)
            for (unsigned j = 0; j < 32; ++j)
                Check(slots[i]->mData[j] == 0x70+i,
                      "Actual growing slot payloads overlap");
        for (auto* slot : slots) grow.Delete(slot);
    }
    Check(releasedBlocks == 3, "Source block cleanup omitted its exact callbacks");
    for (const auto& r : growing)
        Check(!FindGameAllocationOwner(r.base), "Source pool destructor retained a native block");
    Check(VirtualAllocator.TotalFreeMemory() == freeBefore &&
          VirtualAllocator.LargestFreeBlock() == largestBefore,
          "Source pool destructor did not restore actual owner/free geometry");
}

inline unsigned allocationAttempts;
inline unsigned long attemptedBytes;
inline void* FailAllocation(unsigned long bytes) {
    ++allocationAttempts;
    attemptedBytes = bytes;
    return nullptr; // Failure oracle only; never supplies successful storage.
}
inline void QualifyBoundaryRejection(unsigned payload, unsigned count) {
    SlotPoolBase pool;
    pool.m_Initial = count;
    pool.m_AllocFn = FailAllocation;
    allocationAttempts = 0;
    auto* selected = CurrentAllocator;
    const auto depth = AllocatorStackDepth;
    bool rejected = false;
    try { SlotPoolBase::BaseAddNewBlock(&pool, payload); }
    catch (const std::length_error&) { rejected = true; }
    Check(rejected && allocationAttempts == 0 && !pool.m_BlockList && !pool.m_FreeList &&
          pool.m_Initial == count && !pool.m_Delta &&
          CurrentAllocator == selected && AllocatorStackDepth == depth,
          "Invalid native physical slot/count mutated source state or invoked allocation");
}
inline void QualifyBoundaries() {
    QualifyBoundaryRejection(7, 1); // Existing sub-pointer-payload HOLD remains explicit.
    QualifyBoundaryRejection(std::numeric_limits<unsigned>::max(), 1);
    QualifyBoundaryRejection(std::numeric_limits<unsigned>::max()-3, 1);
    QualifyBoundaryRejection(36, std::numeric_limits<unsigned>::max());
    QualifyBoundaryRejection(36, 0); // Existing zero-count failure is unchanged.
    SlotPoolBase pool;
    pool.m_Initial = 10;
    pool.m_AllocFn = FailAllocation;
    allocationAttempts = 0;
    bool rejected = false;
    try { SlotPoolBase::BaseAddNewBlock(&pool, 36); }
    catch (const std::bad_alloc&) { rejected = true; }
    Check(rejected && allocationAttempts == 1 && attemptedBytes == 408 &&
          !pool.m_BlockList && !pool.m_FreeList && pool.m_Initial == 10 && !pool.m_Delta,
          "Failed source allocation did not retain its count/request and empty state");
    SlotPoolBase::BaseFreeBlocks(&pool, 7);
    Check(!pool.m_BlockList && !pool.m_FreeList,
          "Empty source cleanup gained a physical slot-size rejection");
}
inline void QualifyFixedBlockState() {
    using namespace mscharged::platform;
    const auto freeBefore = VirtualAllocator.TotalFreeMemory();
    const auto largestBefore = VirtualAllocator.LargestFreeBlock();
    GameAllocationSpan outer{}, inner{};
    {
        SlotPoolFixedState<64> pool;
        pool.Initialize(2, 2);
        void* first = pool.Allocate();
        void* second = pool.Allocate();
        Check(first && second && FindGameAllocationSpan(first, 64, outer) &&
              outer.bytes == 136 && outer.owner == &VirtualAllocator,
              "Aligned source fixed blocks changed their original physical stride/count");
        std::memset(first, 0x39, 64);
        pool.PushState();
        void* temporary = pool.Allocate();
        Check(temporary && FindGameAllocationSpan(temporary, 64, inner) &&
              inner.bytes == 136 && inner.incarnation != outer.incarnation &&
              pool.m_Depth == 1 && pool.m_States[0].block,
              "Original fixed-block push did not retain a separate live source block");
        std::memset(temporary, 0xA6, 64);
        for (unsigned i = 0; i < 64; ++i)
            Check(static_cast<unsigned char*>(first)[i] == 0x39,
                  "Nested fixed-block allocation corrupted retained source payload");
        // Literal source state destructor frees the inner state, pops the
        // original saved lists, then its base destructor frees the outer block.
    }
    Check(!FindGameAllocationOwner(outer.base) && !FindGameAllocationOwner(inner.base) &&
          VirtualAllocator.TotalFreeMemory() == freeBefore &&
          VirtualAllocator.LargestFreeBlock() == largestBefore,
          "Original fixed-block state destructor did not retire both real owners");
}
inline unsigned QualifyOriginalSlotStride() {
    checks = 0;
    auto* selected = CurrentAllocator;
    const auto depth = AllocatorStackDepth;
    QualifyOriginalEventPool();
    QualifyBoundaries();
    QualifyFixedBlockState();
    Check(CurrentAllocator == selected && AllocatorStackDepth == depth,
          "Original pool allocation/free callbacks changed allocator-stack selection");
    return checks;
}
} // namespace mscharged::testing::slot_stride
