#pragma once

#include <cstddef>
#include <cstdint>

class MemoryAllocator;

namespace mscharged::platform
{
// Native ownership metadata lives outside the original game arena. Reserve it
// before the original allocator mutates its ring, so host metadata OOM cannot
// turn a successful game allocation into an unowned block.
class GameAllocationReservation
{
public:
    GameAllocationReservation(MemoryAllocator& owner, unsigned long bytes);
    ~GameAllocationReservation();
    GameAllocationReservation(const GameAllocationReservation&) = delete;
    GameAllocationReservation& operator=(const GameAllocationReservation&) = delete;
    void Commit(void* pointer);

private:
    void* pending_;
};

// Native device leases attach to a newly created original allocation, never to
// game readiness. Reserve their host metadata before the original request;
// Prepare checks its exact fresh incarnation, Commit adds no metadata. The
// device callback must not reenter this allocation registry. Actual free/reset
// retires the lease before the original free-list bytes can be overwritten.
class GameAllocationDeviceReservation
{
public:
    GameAllocationDeviceReservation(std::size_t metadataBytes, void (*release)(void*));
    ~GameAllocationDeviceReservation();
    GameAllocationDeviceReservation(const GameAllocationDeviceReservation&) = delete;
    GameAllocationDeviceReservation& operator=(const GameAllocationDeviceReservation&) = delete;
    void* Data() const noexcept;
    void Prepare(const void* pointer, std::size_t bytes);
    void Commit();
private:
    void* pending_;
};
void RetireGameAllocationDevicePins(const void* pointer);

// Native storage facts, not game readiness or GL suballocation/frame lifetime.
// Positive byte ranges choose the innermost actual original allocation. A
// range crossing a child allocation is rejected, never assigned to its parent.
struct GameAllocationSpan
{
    void* base;
    std::size_t bytes;
    MemoryAllocator* owner;
    std::uint64_t incarnation;
};
bool FindGameAllocationSpan(const void* pointer, std::size_t bytes, GameAllocationSpan& result);

// Full-width native MEM headers are metadata, not completed payload bytes.
// Their original logical region stays inside an exact live NL incarnation.
// Reserve all backing and the publication node before original MEM creation;
// lookups and Commit allocate nothing. Source owners must remain quiescent.
struct GameHeapMetadataSpan {
    void* data;
    std::size_t bytes;
    const void* source;
    std::size_t source_bytes;
    GameAllocationSpan allocation;
};
class GameHeapMetadataReservation {
public:
    GameHeapMetadataReservation(const void* source, std::size_t sourceBytes,
                                std::size_t nativeBytes);
    ~GameHeapMetadataReservation();
    GameHeapMetadataReservation(const GameHeapMetadataReservation&)=delete;
    GameHeapMetadataReservation& operator=(const GameHeapMetadataReservation&)=delete;
    void* Data() const noexcept;
    void Commit(const void* owner);
private:
    void* pending_;
};
bool FindGameHeapMetadata(const void* owner, GameHeapMetadataSpan& result);
bool FindGameHeapMetadataNative(const void* pointer, std::size_t bytes,
                               GameHeapMetadataSpan& result);
bool FindGameHeapMetadataSource(const void* pointer, std::size_t bytes,
                               GameHeapMetadataSpan& result);
void ValidateGameHeapMetadataRetirement(const void* owner);
void ValidateGameHeapMetadataRangeRetirement(const void* owner, const void* base,
                                            std::size_t bytes);
void RetireGameHeapMetadata(const void* owner);

// Actual nonoverlapping MEM child storage inside an original NL allocation.
// The opaque source heap is a storage/lifetime owner, not a replacement
// MemoryAllocator. Requested bytes never imply completed data or graphics.
struct GameMemoryStorageSpan {
    const void* base;
    std::size_t bytes;
    const void* owner;
    GameAllocationSpan allocation;
    std::uint64_t incarnation;
};
class GameMemoryStorageReservation {
public:
    GameMemoryStorageReservation(const void* owner, std::size_t requestedBytes);
    ~GameMemoryStorageReservation();
    GameMemoryStorageReservation(const GameMemoryStorageReservation&)=delete;
    GameMemoryStorageReservation& operator=(const GameMemoryStorageReservation&)=delete;
    void Commit(void* pointer);
private:
    void* pending_;
};
bool FindGameMemoryStorage(const void* pointer, std::size_t bytes, GameMemoryStorageSpan& result);
void RetireGameMemoryStorage(const void* owner, const void* pointer);
void RetireGameMemoryStorageOwner(const void* owner);

enum class GameByteDomain { WiiSerialized, NativeHeader, NativePayload };
struct GameCompletedSpan
{
    const void* base;
    std::size_t bytes;
    GameAllocationSpan allocation;
};
// Every requested byte must belong to one completed logical source span;
// splitting a domain for native conversion preserves that source extent.
bool FindGameCompletedSpan(const void* pointer, std::size_t bytes, GameCompletedSpan& result);
GameByteDomain FindGameByteDomain(const void* pointer, std::size_t bytes);

// Reserve every metadata node before bytes are touched. physicalBytes includes
// real DMA padding; only logicalBytes is published at completion. Partial writes
// preserve unaffected domains. Concurrent overlapping producers are unsupported.
// Native conversions can preserve an existing completed logical source extent;
// new serialized reads always publish their actual requested logical extent.
class GameByteWriteReservation
{
public:
    GameByteWriteReservation() noexcept : pending_(nullptr) {}
    GameByteWriteReservation(void* destination, std::size_t logicalBytes,
                            std::size_t physicalBytes = 0);
    ~GameByteWriteReservation();
    GameByteWriteReservation(const GameByteWriteReservation&) = delete;
    GameByteWriteReservation& operator=(const GameByteWriteReservation&) = delete;
    GameByteWriteReservation(GameByteWriteReservation&& other) noexcept;
    GameByteWriteReservation& operator=(GameByteWriteReservation&& other) noexcept;
    bool Tracked() const noexcept { return pending_ != nullptr; }
    void Complete(GameByteDomain domain);
    void Reset() noexcept;
private:
    void* pending_;
};
std::uint32_t ReadGameChunkWord(const void* header, unsigned word);

// Expanded native data is attached to the exact completed serialized source
// range/allocation incarnation. Original owner decisions and destructors do not
// change. Real source free, arena retirement or overlapping producer writes
// release the CRT-owned twin; no game readiness is represented here.
struct GameNativeBackingSpan {
    void* data;
    std::size_t bytes;
    GameAllocationSpan allocation;
};
bool FindGameNativeBacking(const void* source, std::size_t sourceBytes, GameNativeBackingSpan& result);
// Resolve a live native view only within the actual owner identified by an
// existing raw source probe. This reads metadata without dereferencing either
// pointer or allocating. No lookup grants a lease: the caller must retain the
// original source object/callback/job lifetime through its subsequent access.
struct GameNativeBackingSourceSpan {
    const void* source;
    std::size_t source_bytes;
    GameNativeBackingSpan backing;
};
bool FindGameNativeBackingSource(const void* native, std::size_t nativeBytes,
    const void* sourceProbe, std::size_t sourceProbeBytes,
    GameNativeBackingSourceSpan& result);
// Identify the unique currently live backing containing these native bytes.
// Only registry metadata is read; neither pointer is dereferenced. A successful
// lookup reports the actual raw source anchor and its allocation incarnation,
// without granting a lease or validating a pointer retained after address reuse.
// The caller must already retain the original source owner's lifetime.
bool FindGameNativeBackingSource(const void* native, std::size_t nativeBytes,
    GameNativeBackingSourceSpan& result);
// Resolve an actual completed raw probe to its unique current native backing.
// This reads metadata only, creates no twin/ID, and grants no lifetime lease.
bool FindGameNativeBackingForSource(const void* sourceProbe, std::size_t sourceProbeBytes,
    GameNativeBackingSourceSpan& result);
class GameNativeBackingReservation {
public:
    GameNativeBackingReservation(const void* source, std::size_t sourceBytes, std::size_t nativeBytes);
    ~GameNativeBackingReservation();
    GameNativeBackingReservation(const GameNativeBackingReservation&)=delete;
    GameNativeBackingReservation& operator=(const GameNativeBackingReservation&)=delete;
    void* Data() const noexcept;
    void Commit();
private:
    void* pending_;
};

// Exact source GL suballocations are storage facts inside an existing actual
// allocation. They do not invent MemoryAllocator owners or imply written bytes.
class GameGraphicsStorageReservation
{
public:
    explicit GameGraphicsStorageReservation(std::size_t bytes);
    ~GameGraphicsStorageReservation();
    GameGraphicsStorageReservation(const GameGraphicsStorageReservation&) = delete;
    GameGraphicsStorageReservation& operator=(const GameGraphicsStorageReservation&) = delete;
    void Commit(void* pointer);
private:
    void* pending_;
};
// Reserve the one possible split node before the original callback/rewind. The
// actual source result supplies the retired range; Commit allocates no metadata.
class GameGraphicsRetirementReservation
{
public:
    GameGraphicsRetirementReservation();
    ~GameGraphicsRetirementReservation();
    GameGraphicsRetirementReservation(const GameGraphicsRetirementReservation&) = delete;
    GameGraphicsRetirementReservation& operator=(const GameGraphicsRetirementReservation&) = delete;
    void Commit(const void* pointer, std::size_t bytes);
private:
    void* pending_;
};
struct GameGraphicsStorageSpan
{
    const void* base;
    std::size_t bytes;
    GameAllocationSpan allocation;
    std::uint64_t incarnation;
};
bool FindGameGraphicsStorage(const void* pointer, std::size_t bytes, GameGraphicsStorageSpan& result);
// Source writer cursors supply the genuinely written range. No vertex/index
// count, normalization or alternate game-side readiness flag is used.
void PublishGameGraphicsNativeBytes(const void* base, const void* writtenEnd);
// A parsed original vertex stream supplies its actual count * wire stride.
// Bounds attach to its existing live graphics storage, never to a new owner.
// The completed logical tag/domain qualify each bound; rewind/free retires it
// with that storage's original incarnation. Shared aliases remain enclosing.
void RegisterGameGraphicsArray(const void* pointer, std::size_t bytes);
struct GameGraphicsArraySpan
{
    const void* data;
    std::size_t bytes;
    bool little_endian;
    GameGraphicsStorageSpan storage;
};
GameGraphicsArraySpan ResolveGameGraphicsArray(const void* pointer);

MemoryAllocator* FindGameAllocationOwner(const void* pointer);
void ValidateGameAllocationFree(MemoryAllocator& owner, const void* pointer);
void FinishGameAllocationFree(const void* pointer);
void DiscardGameAllocatorRecords(MemoryAllocator& owner);
void FreeGameAllocation(void* pointer);
}
