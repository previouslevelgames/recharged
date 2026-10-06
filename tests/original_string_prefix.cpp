#include "NL/nlString.h"
#include "NL/nlBasicString.h"
#include "platform/game_allocation_ownership.h"

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <iostream>
#include <stdexcept>

#if defined(__SANITIZE_ADDRESS__)
#define CHARGED_PREFIX_SANITIZED 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#define CHARGED_PREFIX_SANITIZED 1
#endif
#endif
#if defined(CHARGED_PREFIX_SANITIZED)
#include <sanitizer/lsan_interface.h>
#endif

namespace {
using Allocator = Detail::TempStringPoolAllocator;
using mscharged::platform::FindGameAllocationOwner;
unsigned checks = 0;
unsigned preserved_threshold_cases = 0;

void Check(bool condition, const char* message)
{
    ++checks;
    if (!condition) throw std::runtime_error(message);
}

Detail::TempStringAllocatorPool& Pool()
{
    return Detail::sTempStringAllocatorPool.allocator.pool;
}

void CheckPrefix(const void* payload, unsigned bytes)
{
    const auto* prefix = static_cast<const unsigned char*>(payload) - 4;
    static_assert(sizeof(u32) == 4 && sizeof(unsigned short) == 2);
    static_assert(std::endian::native == std::endian::little
        || std::endian::native == std::endian::big);
    for (unsigned i = 0; i < 4; ++i)
    {
        const unsigned shift = std::endian::native == std::endian::little
            ? 8 * i : 8 * (3 - i);
        Check(prefix[i] == ((bytes >> shift) & 255),
            "Original temporary string prefix is not the exact four-byte size word");
    }
}

void PayloadPreservation()
{
    // Source free-list bytes are the input. New must only replace the four-byte
    // prefix, retaining every byte that becomes the caller's untouched payload.
    void* slot = Pool().Allocate(64);
    Pool().Free(slot, 64);
    std::array<unsigned char, 60> expected;
    std::memcpy(expected.data(), static_cast<unsigned char*>(slot) + 4, expected.size());
    char* data = Allocator::New<char>(1, nullptr);
    Check(static_cast<void*>(data - 4) == slot, "Original slot reuse/order changed");
    CheckPrefix(data, 1);
    for (unsigned i = 0; i < expected.size(); ++i)
        Check(static_cast<unsigned char>(data[i]) == expected[i],
            "Native New overwrote source bytes after the four-byte prefix");
    Allocator::Delete(data);
}

template<class T> void RawCase(int count, MemoryAllocator& owner)
{
    const unsigned bytes = static_cast<unsigned>(count) * sizeof(T);
    const unsigned standard_before = StandardAllocator.TotalFreeMemory();
    const unsigned virtual_before = VirtualAllocator.TotalFreeMemory();
    auto* free_before = Pool().m_FreeList;
    auto* blocks_before = Pool().m_BlockList;
    CurrentAllocator = &owner;
    T* data = Allocator::New<T>(count, nullptr);
    auto* base = reinterpret_cast<unsigned char*>(data) - 4;
    Check(data != nullptr, "Actual original temporary string allocation failed");
    Check(reinterpret_cast<std::uintptr_t>(data) % alignof(T) == 0,
        "Original raw char/Wii16 payload alignment changed");
    CheckPrefix(data, bytes);
    if (bytes + 4 <= 64)
    {
        Check(base == reinterpret_cast<unsigned char*>(free_before),
            "Source small allocation did not consume the existing free-list head");
        Check(Pool().m_BlockList == blocks_before, "Small prefix caused an extra source block");
        Check(FindGameAllocationOwner(base) == nullptr,
            "An interior source slot fabricated an exact owning heap record");
    }
    else
    {
        Check(FindGameAllocationOwner(base) == &owner,
            "Original large temporary string lost its exact allocation owner");
        Check(Pool().m_BlockList == blocks_before && Pool().m_FreeList == free_before,
            "Source large allocation changed small-pool state");
    }
    for (unsigned i = 0; i < bytes; ++i) base[4 + i] = static_cast<unsigned char>(i * 17 + 91);
    CheckPrefix(data, bytes);
    for (unsigned i = 0; i < bytes; ++i)
        Check(base[4 + i] == static_cast<unsigned char>(i * 17 + 91),
            "Caller bytes overlap the source size prefix");

    CurrentAllocator = &owner == &StandardAllocator ? &VirtualAllocator : &StandardAllocator;
    Allocator::Delete(data);
    if (bytes > 60 && bytes <= 64)
    {
        // The source allocates using payload+4, but frees using payload size.
        // Observe that original branch without claiming a successful heap free.
        ++preserved_threshold_cases;
        Check(Pool().m_FreeList == reinterpret_cast<SlotPoolEntry*>(base),
            "Original 61–64-byte free threshold behavior was changed");
        Check(FindGameAllocationOwner(base) == &owner,
            "Prefix adaptation silently repaired the source threshold behavior");
        // Bounded fixture cleanup: consume that exact source free-list entry,
        // then free its genuine owning heap allocation. This is not game logic.
        Check(Pool().Allocate(64) == base, "Original threshold entry was not source-reused");
        nlFree(base);
    }
    else if (bytes > 64)
        Check(FindGameAllocationOwner(base) == nullptr,
            "Original large Free did not retire its owning allocation");
    else
        Check(Pool().m_FreeList == reinterpret_cast<SlotPoolEntry*>(base),
            "Original small Free did not restore its free-list entry");
    Check(Pool().m_FreeList == free_before && Pool().m_BlockList == blocks_before,
        "Bounded raw prefix case did not restore source pool geometry");
    Check(StandardAllocator.TotalFreeMemory() == standard_before
        && VirtualAllocator.TotalFreeMemory() == virtual_before,
        "Bounded raw prefix case lost exact owner memory");
}

void RawAllocCase(int bytes, MemoryAllocator& owner)
{
    const unsigned standard_before = StandardAllocator.TotalFreeMemory();
    const unsigned virtual_before = VirtualAllocator.TotalFreeMemory();
    CurrentAllocator = &owner;
    void* data = Allocator::Alloc(bytes);
    CheckPrefix(data, bytes);
    CurrentAllocator = &owner == &StandardAllocator ? &VirtualAllocator : &StandardAllocator;
    Allocator::Free(data);
    Check(StandardAllocator.TotalFreeMemory() == standard_before
        && VirtualAllocator.TotalFreeMemory() == virtual_before,
        "Original Alloc/ReadSize/Free lost memory after switching current arenas");
}

void ConcurrentSlots()
{
    std::array<char*, 65> live;
    for (unsigned i = 0; i < live.size(); ++i)
    {
        live[i] = Allocator::New<char>(60, nullptr);
        CheckPrefix(live[i], 60);
        std::memset(live[i], i + 1, 60);
        for (unsigned j = 0; j < i; ++j)
            Check(live[i] != live[j], "Actual source slots overlap");
    }
    unsigned block_count = 0;
    for (auto* block = Pool().m_BlockList; block; block = block->next)
    {
        ++block_count;
        auto* base = reinterpret_cast<unsigned char*>(block) - 16 * 64;
        Check(FindGameAllocationOwner(base) == &VirtualAllocator,
            "Original slot block owner is not the genuine virtual allocator");
    }
    Check(block_count == 5, "Original 16-slot source block growth changed");
    for (unsigned i = 0; i < live.size(); ++i)
    {
        CheckPrefix(live[i], 60);
        for (unsigned j = 0; j < 60; ++j)
            Check(static_cast<unsigned char>(live[i][j]) == i + 1,
                "Source slot growth or prefix writes damaged another live payload");
        Allocator::Delete(live[i]);
    }
}

void TypedAlignmentBoundary()
{
    using Data = BasicString<char, Allocator>::Data;
    void* payload = Allocator::Alloc(sizeof(Data));
    CheckPrefix(payload, sizeof(Data));
    if (alignof(Data) > 4)
        Check(reinterpret_cast<std::uintptr_t>(payload) % alignof(Data) != 0,
            "The separate original +4/native Data alignment boundary disappeared");
    // This scope observes raw storage only, never constructs a misaligned Data.
    Allocator::Free(payload);
}

void TypedStringObjects()
{
#ifdef MSCHARGED_DIAGNOSTIC_VECTORS
#error The actual source string qualifier must retain original insert/erase
#endif
    using CharString = BasicString<char, Allocator>;
    using WideString = BasicString<unsigned short, Allocator>;
    static_assert(sizeof(CharString::Data) == 20 && alignof(CharString::Data) == 4);
    static_assert(sizeof(WideString::Data) == 20 && alignof(WideString::Data) == 4);
    static_assert(sizeof(BasicString<char, DefaultAllocator>::Data) == 24
        && alignof(BasicString<char, DefaultAllocator>::Data) == 8);

    const unsigned standard_before = StandardAllocator.TotalFreeMemory();
    const unsigned virtual_before = VirtualAllocator.TotalFreeMemory();
    {
        CharString original("ab");
        CharString copy(original);
        auto* shared = original.mData;
        Check(copy.mData == shared && shared->mRefCount == 2,
            "Actual source copy/refcount does not retain shared Data");
        CheckPrefix(shared, 20);
        copy[0] = 'A';
        Check(copy.mData != shared && shared->mRefCount == 1 && copy.mData->mRefCount == 1,
            "Actual source copy-on-write did not preserve ownership");
        Check(std::strcmp(original.c_str(), "ab") == 0
            && std::strcmp(copy.c_str(), "Ab") == 0,
            "Authored source COW characters differ");
        copy.AppendInPlace("CD");
        Check(std::strcmp(copy.c_str(), "AbCD") == 0,
            "Original insertion/concat characters differ");
        Check(copy.mData->mData.mCapacity == 5,
            "Original insertion/reserve capacity differs");
        CheckPrefix(copy.c_str(), 5);
        copy.erase(copy.begin() + 1, copy.begin() + 3);
        Check(std::strcmp(copy.c_str(), "AD") == 0,
            "Original erase characters differ");
        CharString assigned;
        assigned = original;
        Check(assigned.mData == shared && shared->mRefCount == 2,
            "Original assignment ownership differs");

        const unsigned short text[] = {0x41, 0x3a9, 0x7fff, 0};
        const unsigned short suffix[] = {0x20, 0x42, 0};
        const unsigned short expected[] = {0x41, 0x3a9, 0x7fff, 0x20, 0x42, 0};
        WideString wide(text);
        CheckPrefix(wide.mData, 20);
        CheckPrefix(wide.c_str(), 10);
        wide.AppendInPlace(suffix);
        Check(wide.size() == 5, "Original Wii16 concatenated length differs");
        for (unsigned i = 0; i < 6; ++i)
            Check(wide.c_str()[i] == expected[i], "Authored Wii16 concatenation word differs");
        CheckPrefix(wide.c_str(), 12);
        Check(reinterpret_cast<std::uintptr_t>(wide.mData) % 4 == 0,
            "Actual source Data lost reviewed native alignment");
    }
    Check(StandardAllocator.TotalFreeMemory() == standard_before
        && VirtualAllocator.TotalFreeMemory() == virtual_before,
        "Actual source typed Data/buffer destruction lost owning arena memory");
}

void Qualify()
{
    const unsigned standard_baseline = StandardAllocator.TotalFreeMemory();
    const unsigned virtual_baseline = VirtualAllocator.TotalFreeMemory();
    Check(Pool().m_BlockList == nullptr && Pool().m_FreeList == nullptr,
        "Fixture did not begin with actual untouched source temporary pool state");
    PayloadPreservation();
    // Source pool has at least one free slot before per-case state comparisons.
    const int counts[] = {0, 1, 2, 3, 4, 7, 15, 16, 29, 30, 31, 32, 59, 60, 61, 62,
                          63, 64, 65, 66, 127, 255, 256, 257, 16381, 16382};
    for (auto* owner : {&StandardAllocator, &VirtualAllocator})
    {
        for (int count : counts)
        {
            RawCase<char>(count, *owner);
            RawCase<unsigned short>(count, *owner);
        }
        for (int size : {0, 1, 4, 60, 65, 66, 255, 256, 32763, 32764})
            RawAllocCase(size, *owner);
    }
    ConcurrentSlots();
    TypedAlignmentBoundary();
    TypedStringObjects();
    fn_802B467C(&Pool());
    SlotPoolBase::BaseFreeBlocks(&Pool(), 64);
    Check(Pool().m_BlockList == nullptr && Pool().m_FreeList == nullptr,
        "Actual original slot block release failed");
    Check(StandardAllocator.TotalFreeMemory() == standard_baseline
        && VirtualAllocator.TotalFreeMemory() == virtual_baseline,
        "Actual source temporary pool retained tested heap allocations");
    CurrentAllocator = &StandardAllocator;
}
}

int main()
{
    try
    {
        Qualify();
        std::cout << "actual original string prefix checks=" << checks
            << " threshold_cases=" << preserved_threshold_cases
            << " native_data_alignment=qualified4 terminal_only=1" << std::endl;
#if defined(CHARGED_PREFIX_SANITIZED)
        if (__lsan_do_recoverable_leak_check()) std::_Exit(1);
#endif
        // Preserve the independent original StringBlockAllocator CRT-null branch.
        // All explicitly tested temporary pool lifetimes have ended.
        std::_Exit(0);
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << std::endl;
        std::_Exit(1);
    }
}
