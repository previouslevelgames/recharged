#include "platform/game_allocation_ownership.h"
#include "platform/host_metadata.h"
#include "NL/MemAlloc.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <limits>
#include <map>
#include <mutex>
#include <stdexcept>
#include <utility>

namespace mscharged::platform
{
namespace
{
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
struct ByteSpan
{
    std::size_t bytes;
    GameByteDomain domain;
    std::uintptr_t logical_base;
    std::size_t logical_bytes;
    std::uint64_t logical_tag;
    bool graphics_native = false;
};
using ByteSpans = std::map<std::uintptr_t, ByteSpan, std::less<std::uintptr_t>,
    HostMetadataAllocator<std::pair<const std::uintptr_t, ByteSpan>>>;
struct PendingWrite { std::size_t bytes; std::uint64_t tag; };
using PendingWrites = std::map<std::uintptr_t, PendingWrite, std::less<std::uintptr_t>,
    HostMetadataAllocator<std::pair<const std::uintptr_t, PendingWrite>>>;
struct GraphicsStorage
{
    std::size_t bytes;
    std::uint64_t incarnation;
    ByteSpans::node_type native_publication;
};
using GraphicsStorageSpans = std::map<std::uintptr_t, GraphicsStorage, std::less<std::uintptr_t>,
    HostMetadataAllocator<std::pair<const std::uintptr_t, GraphicsStorage>>>;
struct NativeBacking {
    void* data;
    std::size_t bytes, source_bytes;
    std::uint64_t logical_tag;
    NativeBacking(std::size_t nativeBytes, std::size_t sourceBytes, std::uint64_t tag)
        : data(ChargedNativeMetadataAllocate(nativeBytes)), bytes(nativeBytes), source_bytes(sourceBytes), logical_tag(tag) {}
    NativeBacking(const NativeBacking&)=delete;
    NativeBacking& operator=(const NativeBacking&)=delete;
    NativeBacking(NativeBacking&& other) noexcept
        : data(std::exchange(other.data,nullptr)), bytes(other.bytes), source_bytes(other.source_bytes), logical_tag(other.logical_tag) {}
    ~NativeBacking() { ChargedNativeMetadataRelease(data); }
};
using NativeBackings=std::map<std::uintptr_t,NativeBacking,std::less<std::uintptr_t>,
    HostMetadataAllocator<std::pair<const std::uintptr_t,NativeBacking>>>;
struct DeviceLease
{
    void* data = nullptr;
    void (*release)(void*) = nullptr;
    DeviceLease() = default;
    DeviceLease(std::size_t bytes, void (*callback)(void*))
        : data(ChargedNativeMetadataAllocate(bytes)), release(callback)
    {
        std::memset(data, 0, bytes);
    }
    DeviceLease(const DeviceLease&) = delete;
    DeviceLease& operator=(const DeviceLease&) = delete;
    DeviceLease(DeviceLease&& other) noexcept
        : data(std::exchange(other.data, nullptr)), release(other.release) {}
    DeviceLease& operator=(DeviceLease&& other) noexcept
    {
        if (data) std::terminate(); // Commit only replaces an empty device slot.
        data = std::exchange(other.data, nullptr); release = other.release;
        return *this;
    }
    void Reset()
    {
        if (!data) return;
        release(data); // Actual device transfer/pin ownership drains here.
        ChargedNativeMetadataRelease(data);
        data = nullptr;
    }
    ~DeviceLease() { Reset(); }
};
struct MemoryStorage { std::size_t bytes; const void* owner; std::uint64_t incarnation; };
using MemoryStorageSpans=std::map<std::uintptr_t,MemoryStorage,std::less<std::uintptr_t>,
    HostMetadataAllocator<std::pair<const std::uintptr_t,MemoryStorage>>>;
struct Allocation
{
    MemoryAllocator* owner;
    std::uintptr_t arena;
    unsigned int arena_bytes;
    unsigned long bytes;
    std::uint64_t incarnation;
    ByteSpans byte_spans;
    PendingWrites pending_writes;
    std::uint64_t next_write = 1;
    GraphicsStorageSpans graphics_storage;
    NativeBackings native_backings;
    MemoryStorageSpans memory_storage;
    DeviceLease device;
};
using Records = std::map<std::uintptr_t, Allocation, std::less<std::uintptr_t>,
    HostMetadataAllocator<std::pair<const std::uintptr_t, Allocation>>>;
struct Registry
{
    std::mutex mutex;
    Records records;
    std::uint64_t next_incarnation = 1;
    std::uint64_t next_graphics_incarnation = 1;
    std::uint64_t next_memory_incarnation = 1;
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
    Pending(MemoryAllocator& owner, unsigned long bytes, std::uint64_t incarnation)
    {
        Records temporary;
        temporary.emplace(0, Allocation{&owner,
            reinterpret_cast<std::uintptr_t>(owner.m_memory), owner.m_memory_size, bytes, incarnation});
        record = temporary.extract(0);
    }
};
struct PendingDevice
{
    DeviceLease lease;
    std::uint64_t minimum_incarnation;
    std::uintptr_t address = 0;
    std::uint64_t incarnation = 0;
    PendingDevice(std::size_t bytes, void (*release)(void*), std::uint64_t minimum)
        : lease(bytes, release), minimum_incarnation(minimum) {}
};
void RetireDeviceRange(Records& records, std::uintptr_t address, unsigned long bytes)
{
    auto first = records.lower_bound(address);
    auto last = records.lower_bound(address + bytes);
    for (auto it = first; it != last; ++it) it->second.device.Reset();
}
void EraseBackingRange(Records& records, std::uintptr_t address, unsigned long bytes)
{
    // Child heaps can be contained in an allocation from another original heap.
    // Retiring that backing invalidates their metadata without running new game
    // destructors or altering the original bulk-discard operation.
    RetireDeviceRange(records, address, bytes);
    auto first = records.lower_bound(address);
    auto last = records.lower_bound(address + bytes);
    records.erase(first, last);
}
bool Contains(std::uintptr_t base, std::size_t bytes, std::uintptr_t address, std::size_t count)
{
    return count && address >= base && address - base < bytes
        && count <= bytes - (address - base);
}
bool Overlaps(std::uintptr_t a, std::size_t n, std::uintptr_t b, std::size_t m)
{
    return n && m && (a <= b ? b - a < n : a - b < m);
}
Records::iterator Containing(Records& records, std::uintptr_t address, std::size_t count)
{
    if (!count) return records.end();
    auto candidate = records.upper_bound(address);
    while (candidate != records.begin())
    {
        --candidate;
        // Select by the starting byte first. An extent leaving this actual
        // child must fail; it must never fall back to an outer allocation.
        if (!Contains(candidate->first, candidate->second.bytes, address, 1)) continue;
        if (!Contains(candidate->first, candidate->second.bytes, address, count))
            throw std::invalid_argument("Byte span leaves its actual source allocation");
        auto child = records.upper_bound(address);
        if (child != records.end() && child->first - address < count)
            throw std::invalid_argument("Byte span crosses a live nested allocation");
        return candidate;
    }
    return records.end();
}
GameAllocationSpan Describe(Records::iterator found)
{
    return {reinterpret_cast<void*>(found->first), found->second.bytes,
            found->second.owner, found->second.incarnation};
}
MemoryStorageSpans::iterator ContainingMemory(Allocation& allocation, std::uintptr_t address, std::size_t count) {
    auto& spans=allocation.memory_storage;
    auto candidate=spans.upper_bound(address);
    if(candidate==spans.begin())return spans.end();
    --candidate;
    if(candidate->first==address && !candidate->second.bytes)
        throw std::invalid_argument("Source zero-size MEM request grants no readable bytes");
    if(!Contains(candidate->first,candidate->second.bytes,address,1))return spans.end();
    if(!Contains(candidate->first,candidate->second.bytes,address,count))
        throw std::invalid_argument("Byte span leaves its actual MEM child storage");
    return candidate;
}
void ValidateMemoryExtent(Allocation& allocation,std::uintptr_t address,std::size_t count) {
    if(!count)return;
    if(ContainingMemory(allocation,address,count)!=allocation.memory_storage.end())return;
    auto next=allocation.memory_storage.lower_bound(address);
    if(next!=allocation.memory_storage.end() && next->first-address<count)
        throw std::invalid_argument("Byte span crosses live MEM child storage; parent capacity cannot grant it");
}
GraphicsStorageSpans::iterator ContainingGraphics(Allocation& allocation,
                                                  std::uintptr_t address, std::size_t count)
{
    auto& spans = allocation.graphics_storage;
    auto candidate = spans.upper_bound(address);
    if (candidate == spans.begin()) return spans.end();
    --candidate;
    if (!Contains(candidate->first, candidate->second.bytes, address, 1)) return spans.end();
    if (!Contains(candidate->first, candidate->second.bytes, address, count))
        throw std::invalid_argument("Byte span leaves its actual graphics suballocation");
    return candidate;
}
void ValidateGraphicsExtent(Allocation& allocation, std::uintptr_t address, std::size_t count)
{
    auto& spans = allocation.graphics_storage;
    auto found = ContainingGraphics(allocation, address, count);
    if (found != spans.end()) return;
    auto next = spans.lower_bound(address);
    if (next != spans.end() && next->first - address < count)
        throw std::invalid_argument("Byte span crosses a live graphics suballocation");
}
GameGraphicsStorageSpan DescribeGraphics(Records::iterator owner, GraphicsStorageSpans::iterator storage)
{
    return {reinterpret_cast<const void*>(storage->first), storage->second.bytes,
            Describe(owner), storage->second.incarnation};
}
bool Completed(Allocation& allocation, std::uintptr_t address, std::size_t count, ByteSpan& origin)
{
    if (!count) return false;
    auto span = allocation.byte_spans.upper_bound(address);
    if (span == allocation.byte_spans.begin()) return false;
    --span;
    if (!Contains(span->first, span->second.bytes, address, 1)) return false;
    origin = span->second;
    if (!Contains(origin.logical_base, origin.logical_bytes, address, count)) return false;
    auto cursor = address;
    while (count)
    {
        if (span == allocation.byte_spans.end()
            || !Contains(span->first, span->second.bytes, cursor, 1)
            || span->second.logical_tag != origin.logical_tag
            || span->second.logical_base != origin.logical_base
            || span->second.logical_bytes != origin.logical_bytes) return false;
        const auto available = span->second.bytes - (cursor - span->first);
        const auto take = count < available ? count : available;
        cursor += take; count -= take; ++span;
    }
    return true;
}
struct PendingBytes
{
    std::uintptr_t allocation;
    std::uint64_t incarnation;
    std::uint64_t tag;
    ByteSpans::node_type completed;
    PendingWrites::node_type active;
    ByteSpan prior_origin{};
    bool has_prior_origin;
    std::uintptr_t graphics_base = 0;
    std::uint64_t graphics_incarnation = 0;
    std::uintptr_t memory_base = 0;
    std::uint64_t memory_incarnation = 0;
    PendingBytes(Records::iterator owner, std::uintptr_t address,
                 std::size_t logical, std::size_t physical, std::uint64_t write)
        : allocation(owner->first), incarnation(owner->second.incarnation), tag(write),
          has_prior_origin(Completed(owner->second, address, logical, prior_origin))
    {
        auto memory=ContainingMemory(owner->second,address,physical);
        if(memory!=owner->second.memory_storage.end()) {
            memory_base=memory->first;memory_incarnation=memory->second.incarnation;
        }
        auto graphics = ContainingGraphics(owner->second, address, physical);
        if (graphics != owner->second.graphics_storage.end())
        {
            graphics_base = graphics->first;
            graphics_incarnation = graphics->second.incarnation;
        }
        ByteSpans temporary;
        temporary.emplace(address, ByteSpan{logical, GameByteDomain::WiiSerialized, address, logical, write});
        completed = temporary.extract(address);
        PendingWrites writes;
        writes.emplace(address, PendingWrite{physical, write});
        active = writes.extract(address);
    }
};
ByteSpans::node_type ReservedByteNode()
{
    ByteSpans temporary;
    temporary.emplace(0, ByteSpan{});
    return temporary.extract(0);
}
// A contiguous retired range can split at most one preexisting disjoint span
// into two pieces. Reuse a pre-reserved node for that case; all other trims are
// in-place/extract operations and preserve unaffected source logical origins.
void RetireCompletedByteRange(Allocation& owner, std::uintptr_t address, std::size_t bytes,
                              ByteSpans::node_type& split)
{
    const auto end = address + bytes;
    // Completed spans are disjoint. Only the predecessor and spans starting
    // before the retired end can overlap; retain unrelated nodes in place.
    auto i = owner.byte_spans.upper_bound(address);
    if (i != owner.byte_spans.begin()) --i;
    while (i != owner.byte_spans.end() && i->first < end)
    {
        auto current = i++;
        const auto base = current->first;
        auto& span = current->second;
        if (!Overlaps(base, span.bytes, address, bytes)) continue;
        const bool left = base < address;
        const bool right = end - base < span.bytes;
        if (left && right)
        {
            if (split.empty()) throw std::logic_error("Byte retirement split reservation was consumed");
            auto suffix = span;
            suffix.bytes = span.bytes - (end - base);
            span.bytes = address - base;
            split.key() = end;
            split.mapped() = suffix;
            auto inserted = owner.byte_spans.insert(std::move(split));
            if (!inserted.inserted) throw std::logic_error("Byte retirement suffix address conflict");
        }
        else if (left) span.bytes = address - base;
        else if (right)
        {
            auto node = owner.byte_spans.extract(current);
            node.mapped().bytes -= end - base;
            node.key() = end;
            auto inserted = owner.byte_spans.insert(std::move(node));
            if (!inserted.inserted) throw std::logic_error("Byte retirement trim address conflict");
        }
        else owner.byte_spans.erase(current);
    }
}
void RetireByteRange(Allocation& owner, std::uintptr_t address, std::size_t bytes,
                     ByteSpans::node_type& split)
{
    RetireCompletedByteRange(owner, address, bytes, split);
    for (auto i = owner.pending_writes.begin(); i != owner.pending_writes.end();)
    {
        if (Overlaps(i->first, i->second.bytes, address, bytes)) i = owner.pending_writes.erase(i);
        else ++i;
    }
}
struct PendingNativeBacking {
    std::uintptr_t allocation;
    std::uint64_t incarnation;
    NativeBackings::node_type record;
    PendingNativeBacking(Records::iterator owner, std::uintptr_t source, std::size_t sourceBytes,
                         std::size_t nativeBytes, std::uint64_t tag)
        : allocation(owner->first), incarnation(owner->second.incarnation) {
        NativeBackings temporary;
        temporary.emplace(source,NativeBacking(nativeBytes,sourceBytes,tag));
        record=temporary.extract(source);
    }
};
void RetireNativeBackings(Allocation& owner,std::uintptr_t address,std::size_t bytes) {
    // Attachments can overlap/nest. Every earlier covering range remains a
    // candidate; later nonoverlapping starts are skipped without rebuilding
    // existing byte-span nodes or changing their optimized retirement.
    const auto end=address+bytes;
    for(auto i=owner.native_backings.begin();i!=owner.native_backings.end() && i->first<end;) {
        if(Overlaps(i->first,i->second.source_bytes,address,bytes))i=owner.native_backings.erase(i);
        else ++i;
    }
}
struct PendingMemoryStorage {
    std::uintptr_t allocation;
    std::uint64_t incarnation;
    MemoryStorageSpans::node_type record;
    ByteSpans::node_type split;
    PendingMemoryStorage(Records::iterator parent,const void* sourceOwner,std::size_t bytes,std::uint64_t generation)
        : allocation(parent->first),incarnation(parent->second.incarnation),split(ReservedByteNode()) {
        MemoryStorageSpans temporary;
        temporary.emplace(0,MemoryStorage{bytes,sourceOwner,generation});
        record=temporary.extract(0);
    }
};
void RetireMemory(Allocation& parent,MemoryStorageSpans::iterator child) {
    const auto address=child->first,bytes=child->second.bytes;
    // Registration clears crossing parent domains; every subsequent producer
    // is bounded by this exact child. Retirement therefore needs no split/OOM.
    for(const auto& [base,span]:parent.byte_spans)
        if(Overlaps(base,span.bytes,address,bytes) && !Contains(address,bytes,base,span.bytes))
            throw std::logic_error("MEM retirement found a crossing completed domain");
    for(const auto& [base,span]:parent.graphics_storage)
        if(Overlaps(base,span.bytes,address,bytes) && !Contains(address,bytes,base,span.bytes))
            throw std::logic_error("MEM retirement cuts live graphics storage");
    ByteSpans::node_type noSplit;
    RetireByteRange(parent,address,bytes,noSplit);
    RetireNativeBackings(parent,address,bytes);
    auto first=parent.graphics_storage.lower_bound(address);
    auto last=parent.graphics_storage.lower_bound(address+bytes);
    parent.graphics_storage.erase(first,last);
    parent.memory_storage.erase(child);
}
struct PendingGraphicsStorage
{
    GraphicsStorageSpans::node_type record;
    ByteSpans::node_type split;
    PendingGraphicsStorage(std::size_t bytes, std::uint64_t incarnation) : split(ReservedByteNode())
    {
        GraphicsStorageSpans temporary;
        temporary.emplace(0, GraphicsStorage{bytes, incarnation, ReservedByteNode()});
        record = temporary.extract(0);
    }
};
struct PendingGraphicsRetirement
{
    ByteSpans::node_type split = ReservedByteNode();
};
std::uint32_t HeaderWord(const unsigned char* bytes, GameByteDomain domain)
{
    if (domain == GameByteDomain::WiiSerialized)
        return (std::uint32_t(bytes[0]) << 24) | (std::uint32_t(bytes[1]) << 16)
            | (std::uint32_t(bytes[2]) << 8) | std::uint32_t(bytes[3]);
    if (domain != GameByteDomain::NativeHeader)
        throw std::invalid_argument("Native payload is not a chunk-header domain");
    std::uint32_t result; std::memcpy(&result, bytes, sizeof(result)); return result;
}
}

GameAllocationReservation::GameAllocationReservation(MemoryAllocator& owner, unsigned long bytes)
    : pending_(nullptr)
{
    auto& state = State();
    std::uint64_t incarnation;
    {
        std::lock_guard lock(state.mutex);
        if (!state.next_incarnation) throw std::overflow_error("Allocation incarnation exhausted");
        incarnation = state.next_incarnation++;
    }
    void* storage = ChargedNativeMetadataAllocate(sizeof(Pending));
    try { pending_ = new (storage) Pending(owner, bytes, incarnation); }
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
GameAllocationDeviceReservation::GameAllocationDeviceReservation(
    std::size_t metadataBytes, void (*release)(void*)) : pending_(nullptr)
{
    if (!metadataBytes || !release)
        throw std::invalid_argument("Game device lease has no metadata/release contract");
    void* storage = ChargedNativeMetadataAllocate(sizeof(PendingDevice));
    try
    {
        auto& state = State(); std::lock_guard lock(state.mutex);
        pending_ = new(storage) PendingDevice(metadataBytes, release, state.next_incarnation);
    }
    catch (...) { ChargedNativeMetadataRelease(storage); throw; }
}
GameAllocationDeviceReservation::~GameAllocationDeviceReservation()
{
    static_cast<PendingDevice*>(pending_)->~PendingDevice();
    ChargedNativeMetadataRelease(pending_);
}
void* GameAllocationDeviceReservation::Data() const noexcept
{
    return static_cast<PendingDevice*>(pending_)->lease.data;
}
void GameAllocationDeviceReservation::Prepare(const void* pointer, std::size_t bytes)
{
    auto& pending = *static_cast<PendingDevice*>(pending_);
    if (!pending.lease.data || pending.incarnation || !pointer || !bytes)
        throw std::invalid_argument("Game device lease preparation is not a fresh positive request");
    auto& state = State(); std::lock_guard lock(state.mutex);
    const auto address = reinterpret_cast<std::uintptr_t>(pointer);
    auto found = state.records.find(address);
    if (found == state.records.end() || found->second.incarnation < pending.minimum_incarnation
        || bytes > found->second.bytes || found->second.device.data
        || Containing(state.records, address, bytes) != found)
        throw std::invalid_argument("Game device lease requires its exact fresh original allocation");
    pending.address = address; pending.incarnation = found->second.incarnation;
}
void GameAllocationDeviceReservation::Commit()
{
    auto& pending = *static_cast<PendingDevice*>(pending_);
    if (!pending.lease.data || !pending.incarnation)
        throw std::invalid_argument("Game device lease has no prepared live source owner");
    auto& state = State(); std::lock_guard lock(state.mutex);
    auto found = state.records.find(pending.address);
    if (found == state.records.end() || found->second.incarnation != pending.incarnation
        || found->second.device.data)
        throw std::invalid_argument("Game device source allocation was retired/reused");
    found->second.device = std::move(pending.lease);
}
void RetireGameAllocationDevicePins(const void* pointer)
{
    auto& state = State(); std::lock_guard lock(state.mutex);
    const auto address = reinterpret_cast<std::uintptr_t>(pointer);
    auto found = state.records.find(address);
    if (found == state.records.end())
        throw std::invalid_argument("Device pin retirement has no exact original allocation");
    RetireDeviceRange(state.records, address, found->second.bytes);
}
bool FindGameAllocationSpan(const void* pointer, std::size_t bytes, GameAllocationSpan& result)
{
    auto& state = State(); std::lock_guard lock(state.mutex);
    auto found = Containing(state.records, reinterpret_cast<std::uintptr_t>(pointer), bytes);
    if (found == state.records.end()) return false;
    ValidateMemoryExtent(found->second,reinterpret_cast<std::uintptr_t>(pointer),bytes);
    result = Describe(found); return true;
}
bool FindGameCompletedSpan(const void* pointer, std::size_t bytes, GameCompletedSpan& result)
{
    auto& state = State(); std::lock_guard lock(state.mutex);
    auto address = reinterpret_cast<std::uintptr_t>(pointer);
    auto found = Containing(state.records, address, bytes);
    if (found == state.records.end()) return false;
    ValidateMemoryExtent(found->second,address,bytes);
    ByteSpan origin{};
    if (!Completed(found->second, address, bytes, origin)) return false;
    result = {reinterpret_cast<const void*>(origin.logical_base), origin.logical_bytes, Describe(found)};
    return true;
}
GameByteDomain FindGameByteDomain(const void* pointer, std::size_t bytes)
{
    auto& state = State(); std::lock_guard lock(state.mutex);
    const auto address = reinterpret_cast<std::uintptr_t>(pointer);
    auto found = Containing(state.records, address, bytes);
    if (found == state.records.end()) throw std::invalid_argument("Bytes have no actual source allocation");
    ValidateMemoryExtent(found->second,address,bytes);
    auto span = found->second.byte_spans.upper_bound(address);
    if (span != found->second.byte_spans.begin())
    {
        --span;
        if (Contains(span->first, span->second.bytes, address, bytes)) return span->second.domain;
    }
    throw std::invalid_argument("Bytes have no uniform completed explicit domain");
}
GameByteWriteReservation::GameByteWriteReservation(void* destination, std::size_t logicalBytes,
                                                 std::size_t physicalBytes) : pending_(nullptr)
{
    if (!logicalBytes) throw std::invalid_argument("A byte reservation requires positive logical bytes");
    if (!physicalBytes) physicalBytes = logicalBytes;
    if (physicalBytes < logicalBytes) throw std::invalid_argument("Physical write is shorter than logical bytes");
    auto& state = State(); std::lock_guard lock(state.mutex);
    const auto address = reinterpret_cast<std::uintptr_t>(destination);
    auto owner = Containing(state.records, address, physicalBytes);
    if (owner == state.records.end()) throw std::invalid_argument("Byte write has no actual source allocation");
    ValidateMemoryExtent(owner->second,address,physicalBytes);
    ValidateGraphicsExtent(owner->second, address, physicalBytes);
    for (const auto& [base, active] : owner->second.pending_writes)
        if (Overlaps(base, active.bytes, address, physicalBytes))
            throw std::logic_error("Concurrent overlapping byte producers are unsupported");
    if (!owner->second.next_write) throw std::overflow_error("Byte write incarnation exhausted");
    void* storage = ChargedNativeMetadataAllocate(sizeof(PendingBytes));
    PendingBytes* token = nullptr;
    try
    {
        token = new (storage) PendingBytes(owner, address, logicalBytes, physicalBytes, owner->second.next_write);
        // Any host OOM occurs before completed-domain invalidation or the actual
        // producer write. After reservation, completion inserts reserved nodes.
        auto split = ReservedByteNode();
        auto inserted = owner->second.pending_writes.insert(std::move(token->active));
        if (!inserted.inserted) throw std::logic_error("Byte reservation address conflict");
        RetireCompletedByteRange(owner->second, address, physicalBytes, split);
        RetireNativeBackings(owner->second,address,physicalBytes);
        ++owner->second.next_write;
        pending_ = token;
    }
    catch (...)
    {
        if (token) token->~PendingBytes();
        ChargedNativeMetadataRelease(storage); throw;
    }
}
GameByteWriteReservation::~GameByteWriteReservation() { Reset(); }
GameByteWriteReservation::GameByteWriteReservation(GameByteWriteReservation&& other) noexcept
    : pending_(std::exchange(other.pending_, nullptr)) {}
GameByteWriteReservation& GameByteWriteReservation::operator=(GameByteWriteReservation&& other) noexcept
{
    if (this != &other) { Reset(); pending_ = std::exchange(other.pending_, nullptr); }
    return *this;
}
void GameByteWriteReservation::Reset() noexcept
{
    if (!pending_) return;
    auto& token = *static_cast<PendingBytes*>(pending_);
    if (!token.completed.empty())
    {
        auto& state = State(); std::lock_guard lock(state.mutex);
        auto owner = state.records.find(token.allocation);
        if (owner != state.records.end() && owner->second.incarnation == token.incarnation)
        {
            auto active = owner->second.pending_writes.find(token.completed.key());
            if (active != owner->second.pending_writes.end() && active->second.tag == token.tag)
                owner->second.pending_writes.erase(active);
        }
    }
    token.~PendingBytes(); ChargedNativeMetadataRelease(pending_); pending_ = nullptr;
}
void GameByteWriteReservation::Complete(GameByteDomain domain)
{
    if (domain != GameByteDomain::WiiSerialized && domain != GameByteDomain::NativeHeader
        && domain != GameByteDomain::NativePayload) throw std::invalid_argument("Unknown byte domain");
    if (!pending_) throw std::logic_error("Byte reservation is inactive");
    auto& token = *static_cast<PendingBytes*>(pending_);
    if (token.completed.empty()) throw std::logic_error("Byte reservation completed twice");
    auto& state = State(); std::lock_guard lock(state.mutex);
    auto owner = state.records.find(token.allocation);
    if (owner == state.records.end() || owner->second.incarnation != token.incarnation)
        throw std::invalid_argument("Byte write belongs to a retired allocation");
    const auto address = token.completed.key();
    auto active = owner->second.pending_writes.find(address);
    if (active == owner->second.pending_writes.end() || active->second.tag != token.tag
        || Containing(state.records, address, active->second.bytes) != owner)
        throw std::invalid_argument("Byte write source ownership changed");
    ValidateMemoryExtent(owner->second,address,active->second.bytes);
    if(token.memory_incarnation) {
        auto memory=owner->second.memory_storage.find(token.memory_base);
        if(memory==owner->second.memory_storage.end() || memory->second.incarnation!=token.memory_incarnation)
            throw std::invalid_argument("Byte producer MEM child was freed or reused");
    }
    ValidateGraphicsExtent(owner->second, address, active->second.bytes);
    if (token.graphics_incarnation)
    {
        auto graphics = owner->second.graphics_storage.find(token.graphics_base);
        if (graphics == owner->second.graphics_storage.end()
            || graphics->second.incarnation != token.graphics_incarnation)
            throw std::invalid_argument("Byte write graphics storage was retired or reused");
    }
    auto& complete = token.completed.mapped();
    complete.domain = domain;
    if (domain != GameByteDomain::WiiSerialized && token.has_prior_origin)
    {
        complete.logical_base = token.prior_origin.logical_base;
        complete.logical_bytes = token.prior_origin.logical_bytes;
        complete.logical_tag = token.prior_origin.logical_tag;
    }
    auto inserted = owner->second.byte_spans.insert(std::move(token.completed));
    if (!inserted.inserted)
    {
        token.completed = std::move(inserted.node);
        throw std::logic_error("Completed byte domain address conflict");
    }
    owner->second.pending_writes.erase(active);
}
GameNativeBackingReservation::GameNativeBackingReservation(const void* source,std::size_t sourceBytes,
                                                         std::size_t nativeBytes) : pending_(nullptr) {
    if(!sourceBytes || !nativeBytes)throw std::invalid_argument("Native backing requires positive real extents");
    auto& state=State();std::lock_guard lock(state.mutex);
    const auto address=reinterpret_cast<std::uintptr_t>(source);
    auto owner=Containing(state.records,address,sourceBytes);
    ByteSpan origin{};
    if(owner!=state.records.end())ValidateMemoryExtent(owner->second,address,sourceBytes);
    if(owner==state.records.end() || !Completed(owner->second,address,sourceBytes,origin))
        throw std::invalid_argument("Native backing has no completed source allocation");
    auto domain=owner->second.byte_spans.upper_bound(address);
    if(domain==owner->second.byte_spans.begin())throw std::invalid_argument("Native backing lacks a serialized domain");
    --domain;
    if(!Contains(domain->first,domain->second.bytes,address,sourceBytes) || domain->second.domain!=GameByteDomain::WiiSerialized)
        throw std::invalid_argument("Native backing requires one genuine serialized source extent");
    if(owner->second.native_backings.find(address)!=owner->second.native_backings.end())throw std::logic_error("Source registry backing already expanded");
    void* storage=ChargedNativeMetadataAllocate(sizeof(PendingNativeBacking));
    try {pending_=new(storage) PendingNativeBacking(owner,address,sourceBytes,nativeBytes,origin.logical_tag);}
    catch(...) {ChargedNativeMetadataRelease(storage);throw;}
}
GameNativeBackingReservation::~GameNativeBackingReservation() {
    if(pending_) {
        static_cast<PendingNativeBacking*>(pending_)->~PendingNativeBacking();
        ChargedNativeMetadataRelease(pending_);
    }
}
void* GameNativeBackingReservation::Data() const noexcept {
    auto* token=static_cast<PendingNativeBacking*>(pending_);
    return token && !token->record.empty()?token->record.mapped().data:nullptr;
}
void GameNativeBackingReservation::Commit() {
    if(!pending_)throw std::logic_error("Native backing reservation is inactive");
    auto& token=*static_cast<PendingNativeBacking*>(pending_);
    if(token.record.empty())throw std::logic_error("Native backing committed twice");
    auto& state=State();std::lock_guard lock(state.mutex);
    auto owner=state.records.find(token.allocation);
    ByteSpan origin{};
    if(owner==state.records.end() || owner->second.incarnation!=token.incarnation
        || Containing(state.records,token.record.key(),token.record.mapped().source_bytes)!=owner
        || !Completed(owner->second,token.record.key(),token.record.mapped().source_bytes,origin)
        || origin.logical_tag!=token.record.mapped().logical_tag)
        throw std::invalid_argument("Expanded backing belongs to a retired or overwritten source incarnation");
    // A native conversion preserves the logical source tag. Recheck the raw
    // domain before publication so that pending expansion cannot revive a view
    // derived from bytes that have since been converted.
    auto domain=owner->second.byte_spans.upper_bound(token.record.key());
    if(domain==owner->second.byte_spans.begin())
        throw std::invalid_argument("Expanded backing lost its serialized source domain");
    --domain;
    if(!Contains(domain->first,domain->second.bytes,token.record.key(),token.record.mapped().source_bytes)
        || domain->second.domain!=GameByteDomain::WiiSerialized)
        throw std::invalid_argument("Expanded backing requires unchanged serialized source bytes");
    auto inserted=owner->second.native_backings.insert(std::move(token.record));
    if(!inserted.inserted) {
        token.record=std::move(inserted.node);
        throw std::logic_error("Expanded source backing address conflict");
    }
}
bool FindGameNativeBacking(const void* source,std::size_t sourceBytes,GameNativeBackingSpan& result) {
    auto& state=State();std::lock_guard lock(state.mutex);
    auto address=reinterpret_cast<std::uintptr_t>(source);
    auto owner=Containing(state.records,address,sourceBytes);
    if(owner==state.records.end())return false;
    auto backing=owner->second.native_backings.find(address);
    ByteSpan origin{};
    if(backing==owner->second.native_backings.end() || backing->second.source_bytes!=sourceBytes
        || !Completed(owner->second,address,sourceBytes,origin) || origin.logical_tag!=backing->second.logical_tag)return false;
    result={backing->second.data,backing->second.bytes,Describe(owner)};return true;
}
bool FindGameNativeBackingSource(const void* native, std::size_t nativeBytes,
    const void* sourceProbe, std::size_t sourceProbeBytes,
    GameNativeBackingSourceSpan& result)
{
    result = {};
    if (!native || !nativeBytes || !sourceProbe || !sourceProbeBytes) return false;
    auto& state = State(); std::lock_guard lock(state.mutex);
    const auto probe = reinterpret_cast<std::uintptr_t>(sourceProbe);
    auto owner = Containing(state.records, probe, sourceProbeBytes);
    if (owner == state.records.end()) return false;
    const auto address = reinterpret_cast<std::uintptr_t>(native);
    for (const auto& entry : owner->second.native_backings)
    {
        const auto& backing = entry.second;
        if (!Contains(entry.first, backing.source_bytes, probe, sourceProbeBytes)
            || !Contains(reinterpret_cast<std::uintptr_t>(backing.data),
                         backing.bytes, address, nativeBytes)) continue;
        ByteSpan origin{};
        if (Containing(state.records, entry.first, backing.source_bytes) != owner
            || !Completed(owner->second, entry.first, backing.source_bytes, origin)
            || origin.logical_tag != backing.logical_tag) return false;
        result = {reinterpret_cast<const void*>(entry.first), backing.source_bytes,
                  {backing.data, backing.bytes, Describe(owner)}};
        return true;
    }
    return false;
}
bool FindGameNativeBackingSource(const void* native, std::size_t nativeBytes,
    GameNativeBackingSourceSpan& result)
{
    result = {};
    if (!native || !nativeBytes) return false;
    auto& state = State(); std::lock_guard lock(state.mutex);
    const auto address = reinterpret_cast<std::uintptr_t>(native);
    GameNativeBackingSourceSpan candidate{};
    for (auto owner = state.records.begin(); owner != state.records.end(); ++owner)
    {
        for (const auto& entry : owner->second.native_backings)
        {
            const auto& backing = entry.second;
            if (!Contains(reinterpret_cast<std::uintptr_t>(backing.data),
                          backing.bytes, address, nativeBytes)) continue;
            ByteSpan origin{};
            if (Containing(state.records, entry.first, backing.source_bytes) != owner
                || !Completed(owner->second, entry.first, backing.source_bytes, origin)
                || origin.logical_tag != backing.logical_tag) return false;
            ValidateMemoryExtent(owner->second, entry.first, backing.source_bytes);
            if (candidate.source) return false;
            candidate = {reinterpret_cast<const void*>(entry.first), backing.source_bytes,
                         {backing.data, backing.bytes, Describe(owner)}};
        }
    }
    if (!candidate.source) return false;
    result = candidate;
    return true;
}
bool FindGameNativeBackingForSource(const void* sourceProbe, std::size_t sourceProbeBytes,
    GameNativeBackingSourceSpan& result)
{
    result = {};
    if (!sourceProbe || !sourceProbeBytes) return false;
    auto& state = State(); std::lock_guard lock(state.mutex);
    const auto probe = reinterpret_cast<std::uintptr_t>(sourceProbe);
    auto owner = Containing(state.records, probe, sourceProbeBytes);
    if (owner == state.records.end()) return false;
    ValidateMemoryExtent(owner->second, probe, sourceProbeBytes);
    GameNativeBackingSourceSpan candidate{};
    for (const auto& entry : owner->second.native_backings)
    {
        const auto& backing = entry.second;
        if (!Contains(entry.first, backing.source_bytes, probe, sourceProbeBytes)) continue;
        ByteSpan origin{};
        if (Containing(state.records, entry.first, backing.source_bytes) != owner
            || !Completed(owner->second, entry.first, backing.source_bytes, origin)
            || origin.logical_tag != backing.logical_tag) return false;
        ValidateMemoryExtent(owner->second, entry.first, backing.source_bytes);
        if (candidate.source) return false;
        candidate = {reinterpret_cast<const void*>(entry.first), backing.source_bytes,
                     {backing.data, backing.bytes, Describe(owner)}};
    }
    if (!candidate.source) return false;
    result = candidate;
    return true;
}
std::uint32_t ReadGameChunkWord(const void* header, unsigned word)
{
    if (word > 1) throw std::invalid_argument("Chunk header has exactly two words");
    auto& state = State(); std::lock_guard lock(state.mutex);
    const auto address = reinterpret_cast<std::uintptr_t>(header);
    auto owner = Containing(state.records, address, 8);
    if (owner == state.records.end()) throw std::invalid_argument("Chunk has no live source allocation");
    ValidateMemoryExtent(owner->second,address,8);
    auto span = owner->second.byte_spans.upper_bound(address);
    if (span == owner->second.byte_spans.begin()) throw std::invalid_argument("Chunk header has no completed domain");
    --span;
    if (!Contains(span->first, span->second.bytes, address, 8))
        throw std::invalid_argument("Chunk header has no uniform eight-byte domain");
    const auto* bytes = static_cast<const unsigned char*>(header);
    const auto id = HeaderWord(bytes, span->second.domain);
    const auto size = HeaderWord(bytes + 4, span->second.domain);
    const auto extent = std::size_t(size) + 8;
    ValidateMemoryExtent(owner->second,address,extent);
    ByteSpan origin{};
    if (Containing(state.records, address, extent) != owner
        || !Completed(owner->second, address, extent, origin))
        throw std::invalid_argument("Authored chunk leaves completed logical source bytes");
    const auto bits = (id >> 24) & 15;
    const auto alignment = bits ? std::uintptr_t(1) << bits : std::uintptr_t(1);
    const auto payload = (address + 8 + alignment - 1) & ~(alignment - 1);
    if (payload - address - 8 > size) throw std::invalid_argument("Chunk alignment leaves raw extent");
    return word ? size : id;
}

GameMemoryStorageReservation::GameMemoryStorageReservation(const void* sourceOwner,std::size_t bytes):pending_(nullptr) {
    auto& state=State();std::lock_guard lock(state.mutex);
    auto parent=Containing(state.records,reinterpret_cast<std::uintptr_t>(sourceOwner),1);
    if(parent==state.records.end())throw std::invalid_argument("MEM heap has no live original backing allocation");
    if(!state.next_memory_incarnation)throw std::overflow_error("MEM storage incarnation exhausted");
    void* storage=ChargedNativeMetadataAllocate(sizeof(PendingMemoryStorage));
    try {pending_=new(storage) PendingMemoryStorage(parent,sourceOwner,bytes,state.next_memory_incarnation++);}
    catch(...) {ChargedNativeMetadataRelease(storage);throw;}
}
GameMemoryStorageReservation::~GameMemoryStorageReservation() {
    if(!pending_)return;
    static_cast<PendingMemoryStorage*>(pending_)->~PendingMemoryStorage();ChargedNativeMetadataRelease(pending_);
}
void GameMemoryStorageReservation::Commit(void* pointer) {
    auto& token=*static_cast<PendingMemoryStorage*>(pending_);
    if(token.record.empty())throw std::logic_error("MEM storage committed twice");
    const auto address=reinterpret_cast<std::uintptr_t>(pointer),bytes=token.record.mapped().bytes;
    auto& state=State();std::lock_guard lock(state.mutex);
    auto parent=state.records.find(token.allocation);
    if(parent==state.records.end() || parent->second.incarnation!=token.incarnation
        || Containing(state.records,address,bytes?bytes:1)!=parent)
        throw std::invalid_argument("MEM allocation left its genuine source heap backing");
    for(const auto& [base,span]:parent->second.memory_storage)
        if(base==address || Overlaps(base,span.bytes,address,bytes))
            throw std::invalid_argument("MEM allocation overlaps another live child");
    for(const auto& [base,span]:parent->second.graphics_storage)
        if(Overlaps(base,span.bytes,address,bytes))throw std::invalid_argument("MEM allocation overlaps live graphics storage");
    for(const auto& [base,span]:parent->second.pending_writes)
        if(Overlaps(base,span.bytes,address,bytes))throw std::invalid_argument("MEM allocation overlaps unfinished producer");
    RetireByteRange(parent->second,address,bytes,token.split);
    RetireNativeBackings(parent->second,address,bytes);
    token.record.key()=address;
    auto inserted=parent->second.memory_storage.insert(std::move(token.record));
    if(!inserted.inserted)throw std::logic_error("MEM storage address conflict");
}
bool FindGameMemoryStorage(const void* pointer,std::size_t bytes,GameMemoryStorageSpan& result) {
    result={};if(!bytes)return false;
    auto& state=State();std::lock_guard lock(state.mutex);const auto address=reinterpret_cast<std::uintptr_t>(pointer);
    auto parent=Containing(state.records,address,bytes);if(parent==state.records.end())return false;
    auto child=ContainingMemory(parent->second,address,bytes);if(child==parent->second.memory_storage.end())return false;
    result={reinterpret_cast<const void*>(child->first),child->second.bytes,child->second.owner,Describe(parent),child->second.incarnation};return true;
}
void RetireGameMemoryStorage(const void* sourceOwner,const void* pointer) {
    auto& state=State();std::lock_guard lock(state.mutex);const auto address=reinterpret_cast<std::uintptr_t>(pointer);
    auto parent=Containing(state.records,address,1);
    if(parent==state.records.end())throw std::invalid_argument("MEM free lost its actual original backing");
    auto child=parent->second.memory_storage.find(address);
    if(child==parent->second.memory_storage.end() || child->second.owner!=sourceOwner)
        throw std::invalid_argument("MEM free requires its exact live source heap child");
    RetireMemory(parent->second,child);
}
void RetireGameMemoryStorageOwner(const void* sourceOwner) {
    auto& state=State();std::lock_guard lock(state.mutex);
    for(auto& [base,parent]:state.records) {
        for(auto child=parent.memory_storage.begin();child!=parent.memory_storage.end();) {
            auto current=child++;
            if(current->second.owner==sourceOwner)RetireMemory(parent,current);
        }
    }
}

GameGraphicsStorageReservation::GameGraphicsStorageReservation(std::size_t bytes) : pending_(nullptr)
{
    if (!bytes) return;
    auto& state = State();
    std::uint64_t incarnation;
    {
        std::lock_guard lock(state.mutex);
        if (!state.next_graphics_incarnation) throw std::overflow_error("Graphics storage incarnation exhausted");
        incarnation = state.next_graphics_incarnation++;
    }
    void* storage = ChargedNativeMetadataAllocate(sizeof(PendingGraphicsStorage));
    try { pending_ = new (storage) PendingGraphicsStorage(bytes, incarnation); }
    catch (...) { ChargedNativeMetadataRelease(storage); throw; }
}
GameGraphicsStorageReservation::~GameGraphicsStorageReservation()
{
    if (!pending_) return;
    static_cast<PendingGraphicsStorage*>(pending_)->~PendingGraphicsStorage();
    ChargedNativeMetadataRelease(pending_);
}
void GameGraphicsStorageReservation::Commit(void* pointer)
{
    if (!pending_) return; // A source zero-size request is not a positive span.
    auto& pending = *static_cast<PendingGraphicsStorage*>(pending_);
    if (pending.record.empty()) throw std::logic_error("Graphics storage committed twice");
    const auto address = reinterpret_cast<std::uintptr_t>(pointer);
    const auto bytes = pending.record.mapped().bytes;
    auto& state = State(); std::lock_guard lock(state.mutex);
    auto owner = Containing(state.records, address, bytes);
    if (owner == state.records.end()) throw std::invalid_argument("Graphics storage has no real allocation owner");
    ValidateMemoryExtent(owner->second,address,bytes);
    for (const auto& [base, live] : owner->second.graphics_storage)
        if (Overlaps(base, live.bytes, address, bytes))
            throw std::invalid_argument("Graphics suballocation overlaps live source storage");
    RetireByteRange(owner->second, address, bytes, pending.split);
    pending.record.key() = address;
    auto inserted = owner->second.graphics_storage.insert(std::move(pending.record));
    if (!inserted.inserted) throw std::logic_error("Graphics suballocation address conflict");
}
GameGraphicsRetirementReservation::GameGraphicsRetirementReservation() : pending_(nullptr)
{
    void* storage = ChargedNativeMetadataAllocate(sizeof(PendingGraphicsRetirement));
    try { pending_ = new (storage) PendingGraphicsRetirement; }
    catch (...) { ChargedNativeMetadataRelease(storage); throw; }
}
GameGraphicsRetirementReservation::~GameGraphicsRetirementReservation()
{
    static_cast<PendingGraphicsRetirement*>(pending_)->~PendingGraphicsRetirement();
    ChargedNativeMetadataRelease(pending_);
}
void GameGraphicsRetirementReservation::Commit(const void* pointer, std::size_t bytes)
{
    if (!bytes) return;
    const auto address = reinterpret_cast<std::uintptr_t>(pointer);
    auto& state = State(); std::lock_guard lock(state.mutex);
    auto owner = Containing(state.records, address, bytes);
    if (owner == state.records.end()) throw std::invalid_argument("Graphics rewind has no real allocation backing");
    ValidateMemoryExtent(owner->second,address,bytes);
    for (const auto& [base, live] : owner->second.graphics_storage)
        if (Overlaps(base, live.bytes, address, bytes) && !Contains(address, bytes, base, live.bytes))
            throw std::invalid_argument("Graphics rewind cuts through a live source suballocation");
    auto& pending = *static_cast<PendingGraphicsRetirement*>(pending_);
    RetireByteRange(owner->second, address, bytes, pending.split);
    auto first = owner->second.graphics_storage.lower_bound(address);
    auto last = owner->second.graphics_storage.lower_bound(address + bytes);
    owner->second.graphics_storage.erase(first, last);
}
bool FindGameGraphicsStorage(const void* pointer, std::size_t bytes, GameGraphicsStorageSpan& result)
{
    if (!bytes) return false;
    auto& state = State(); std::lock_guard lock(state.mutex);
    const auto address = reinterpret_cast<std::uintptr_t>(pointer);
    auto owner = Containing(state.records, address, bytes);
    if (owner == state.records.end()) return false;
    auto graphics = ContainingGraphics(owner->second, address, bytes);
    if (graphics == owner->second.graphics_storage.end()) return false;
    result = DescribeGraphics(owner, graphics); return true;
}
void PublishGameGraphicsNativeBytes(const void* base, const void* writtenEnd)
{
    if (!base && !writtenEnd) return;
    const auto address = reinterpret_cast<std::uintptr_t>(base);
    const auto end = reinterpret_cast<std::uintptr_t>(writtenEnd);
    if (end < address) throw std::invalid_argument("Native graphics writer cursor precedes its source base");
    const auto count = end - address;
    if (!count) return;
    auto& state = State(); std::lock_guard lock(state.mutex);
    auto owner = Containing(state.records, address, count);
    if (owner == state.records.end()) throw std::invalid_argument("Native stream has no real allocation backing");
    ValidateMemoryExtent(owner->second,address,count);
    auto graphics = owner->second.graphics_storage.find(address);
    if (graphics == owner->second.graphics_storage.end() || count > graphics->second.bytes)
        throw std::invalid_argument("Native stream exceeds its exact source suballocation");
    for (const auto& [activeBase, active] : owner->second.pending_writes)
        if (Overlaps(activeBase, active.bytes, address, count))
            throw std::invalid_argument("Native stream overlaps an unfinished source byte producer");
    const ByteSpan ready{count, GameByteDomain::NativePayload, address, count, graphics->second.incarnation, true};
    auto& prepared = graphics->second.native_publication;
    if (!prepared.empty())
    {
        for (const auto& [oldBase, old] : owner->second.byte_spans)
            if (Overlaps(oldBase, old.bytes, address, count))
                throw std::invalid_argument("Native stream publication overlaps another completed producer");
        prepared.key() = address; prepared.mapped() = ready;
        auto inserted = owner->second.byte_spans.insert(std::move(prepared));
        if (!inserted.inserted) throw std::logic_error("Native stream publication address conflict");
    }
    else
    {
        auto old = owner->second.byte_spans.find(address);
        if (old == owner->second.byte_spans.end() || !old->second.graphics_native
            || old->second.logical_tag != ready.logical_tag
            || old->second.domain != GameByteDomain::NativePayload)
            throw std::invalid_argument("Native stream publication was replaced by another producer");
        old->second = ready; // Original repeated End/continued cursor writes are not forbidden.
    }
}
GameGraphicsArraySpan ResolveGameGraphicsArray(const void* pointer)
{
    const auto address = reinterpret_cast<std::uintptr_t>(pointer);
    auto& state = State(); std::lock_guard lock(state.mutex);
    auto owner = Containing(state.records, address, 1);
    if (owner == state.records.end()) throw std::invalid_argument("GX array has no real allocation backing");
    auto graphics = ContainingGraphics(owner->second, address, 1);
    if (graphics == owner->second.graphics_storage.end())
        throw std::invalid_argument("GX array has no exact source suballocation");
    auto span = owner->second.byte_spans.upper_bound(address);
    if (span == owner->second.byte_spans.begin()) throw std::invalid_argument("GX array producer has not completed");
    --span;
    if (!Contains(span->first, span->second.bytes, address, 1))
        throw std::invalid_argument("GX array bytes have no completed explicit domain");
    const auto domain = span->second.domain;
    if (domain != GameByteDomain::NativePayload && domain != GameByteDomain::WiiSerialized)
        throw std::invalid_argument("Chunk header bytes are not a GX array domain");
    auto bytes = graphics->second.bytes - (address - graphics->first);
    const auto domainBytes = span->second.bytes - (address - span->first);
    if (domainBytes < bytes) bytes = domainBytes;
    const auto logicalBytes = span->second.logical_bytes - (address - span->second.logical_base);
    if (logicalBytes < bytes) bytes = logicalBytes;
    if (Containing(state.records, address, bytes) != owner)
        throw std::invalid_argument("GX array crosses a live nested allocation");
    ValidateMemoryExtent(owner->second,address,bytes);
    ByteSpan origin{};
    if (!Completed(owner->second, address, bytes, origin))
        throw std::invalid_argument("GX array exceeds actual completed logical source bytes");
    // Query the actual native scalar representation, never the plausibility of
    // asset bytes. Keep the existing C++17 metadata consumer ABI supported.
    const std::uint16_t nativeOne = 1;
    unsigned char nativeBytes[sizeof(nativeOne)];
    std::memcpy(nativeBytes, &nativeOne, sizeof(nativeBytes));
    const bool little = domain == GameByteDomain::NativePayload && nativeBytes[0] == 1;
    return {pointer, bytes, little, DescribeGraphics(owner, graphics)};
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
