#include "platform/game_allocation_ownership.h"
#include "platform/host_metadata.h"
#include "NL/MemAlloc.h"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <mutex>
#include <stdexcept>
#include <utility>

namespace mscharged::platform
{
namespace
{
struct Allocation
{
    MemoryAllocator* owner;
    std::uintptr_t arena;
    unsigned int arena_bytes;
    unsigned long bytes;
};
template<class T> struct HostMetadataAllocator
{
    using value_type = T;
    HostMetadataAllocator() = default;
    template<class U> HostMetadataAllocator(const HostMetadataAllocator<U>&) noexcept {}
    T* allocate(std::size_t count)
    {
        static_assert(alignof(T) <= alignof(std::max_align_t));
        if (count > std::numeric_limits<std::size_t>::max() / sizeof(T)) throw std::bad_alloc();
        return static_cast<T*>(ChargedNativeMetadataAllocate(count * sizeof(T)));
    }
    void deallocate(T* pointer, std::size_t) noexcept { ChargedNativeMetadataRelease(pointer); }
    template<class U> bool operator==(const HostMetadataAllocator<U>&) const noexcept { return true; }
};
using Records = std::map<std::uintptr_t, Allocation, std::less<std::uintptr_t>,
    HostMetadataAllocator<std::pair<const std::uintptr_t, Allocation>>>;
struct Registry
{
    std::mutex mutex;
    Records records;
};
Registry& State()
{
    // Constructed on first use; no dependence on another TU's static order.
    // The explicit C allocation boundary keeps nodes in the host CRT even when
    // this registry is compiled inside the original-game module.
    static Registry registry;
    return registry;
}
struct Pending
{
    Records::node_type record;
    Pending(MemoryAllocator& owner, unsigned long bytes)
    {
        Records temporary;
        temporary.emplace(0, Allocation{&owner,
            reinterpret_cast<std::uintptr_t>(owner.m_memory), owner.m_memory_size, bytes});
        record = temporary.extract(0);
    }
};
void EraseBackingRange(Records& records, std::uintptr_t address, unsigned long bytes)
{
    // Child heaps can be contained in an allocation from another original heap.
    // Retiring that backing invalidates their metadata without running new game
    // destructors or altering the original bulk-discard operation.
    auto first = records.lower_bound(address);
    auto last = records.lower_bound(address + bytes);
    records.erase(first, last);
}
}

GameAllocationReservation::GameAllocationReservation(MemoryAllocator& owner, unsigned long bytes)
    : pending_(nullptr)
{
    (void)State();
    void* storage = ChargedNativeMetadataAllocate(sizeof(Pending));
    try { pending_ = new (storage) Pending(owner, bytes); }
    catch (...) { ChargedNativeMetadataRelease(storage); throw; }
}
GameAllocationReservation::~GameAllocationReservation()
{
    if (pending_)
    {
        static_cast<Pending*>(pending_)->~Pending();
        ChargedNativeMetadataRelease(pending_);
    }
}
void GameAllocationReservation::Commit(void* pointer)
{
    auto& pending = *static_cast<Pending*>(pending_);
    pending.record.key() = reinterpret_cast<std::uintptr_t>(pointer);
    auto& state = State();
    std::lock_guard lock(state.mutex);
    auto insertion = state.records.insert(std::move(pending.record));
    if (!insertion.inserted)
    {
        pending.record = std::move(insertion.node);
        throw std::logic_error("Original allocator returned an already live allocation");
    }
}
MemoryAllocator* FindGameAllocationOwner(const void* pointer)
{
    auto& state = State();
    std::lock_guard lock(state.mutex);
    auto found = state.records.find(reinterpret_cast<std::uintptr_t>(pointer));
    return found == state.records.end() ? nullptr : found->second.owner;
}
void ValidateGameAllocationFree(MemoryAllocator& owner, const void* pointer)
{
    auto& state = State();
    std::lock_guard lock(state.mutex);
    auto found = state.records.find(reinterpret_cast<std::uintptr_t>(pointer));
    if (found == state.records.end())
        throw std::invalid_argument("Game free has no exact live allocation record");
    // A source allocator copy can free through the same arena snapshot. This
    // check does not make conflicting mutations of two copies safe or transfer
    // the original game ownership/lifetime to a different allocator object.
    if (found->second.arena != reinterpret_cast<std::uintptr_t>(owner.m_memory)
        || found->second.arena_bytes != owner.m_memory_size)
        throw std::invalid_argument("Game free uses a different allocator arena");
}
void FinishGameAllocationFree(const void* pointer)
{
    auto& state = State();
    std::lock_guard lock(state.mutex);
    auto address = reinterpret_cast<std::uintptr_t>(pointer);
    auto found = state.records.find(address);
    if (found == state.records.end())
        throw std::logic_error("Game free lost its native ownership record");
    EraseBackingRange(state.records, address, found->second.bytes);
}
void DiscardGameAllocatorRecords(MemoryAllocator& owner)
{
    auto& state = State();
    std::lock_guard lock(state.mutex);
    // Initialize replaces the original free-list backing. Walk without holding
    // iterators across descendant retirement; reset is infrequent and allocates
    // no native metadata.
    for (;;)
    {
        auto found = state.records.begin();
        while (found != state.records.end() && found->second.owner != &owner) ++found;
        if (found == state.records.end()) break;
        EraseBackingRange(state.records, found->first, found->second.bytes);
    }
}
void FreeGameAllocation(void* pointer)
{
    if (!pointer) return;
    auto* owner = FindGameAllocationOwner(pointer);
    if (!owner) throw std::invalid_argument("nlFree has no exact live game allocation owner");
    owner->Free(pointer);
}
}
