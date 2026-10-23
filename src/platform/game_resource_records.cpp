#include "platform/game_resource_records.h"

#include "NL/MemAlloc.h"
#include "NL/gl/glMemory.h"
#include "platform/game_allocation_ownership.h"
#include <cstdlib>
#include <limits>
#if defined(_WIN32)
#include <malloc.h>
#endif
#include <algorithm>
#include <mutex>
#include <new>
#include <stdexcept>

namespace mscharged::platform
{
namespace
{
struct Record
{
    Record* next;
    GLResourcePool* pool;
    GameResourceRecordInfo info;
};
// The records and their list live in a heap of their own, an original
// MemoryAllocator at the start of one page-aligned host block: a rollback
// saves and restores the block with the game's arenas, so the records go back
// with the pools holding their quota and the game objects pointing at them.
constexpr std::size_t kStoreBytes = 16u << 20;
struct Store
{
    MemoryAllocator heap;
    Record* records;
};
std::mutex recordMutex;
// The most the heap held, outside it (a rollback takes the heap back, not this).
std::size_t peakUsed;
Store& TheStore()
{
    static Store* store = [] {
        // The host CRT, never the game's arenas (never freed).
#if defined(_WIN32)
        void* block = _aligned_malloc(kStoreBytes, 4096);
#else
        void* block = std::aligned_alloc(4096, kStoreBytes);
#endif
        if (!block) throw std::bad_alloc();
        auto* created = new (block) Store{};
        const std::size_t header = (sizeof(Store) + 63) & ~std::size_t(63);
        created->heap.Initialize(static_cast<unsigned char*>(block) + header,
                                 static_cast<unsigned int>(kStoreBytes - header));
        return created;
    }();
    return *store;
}

std::uint64_t PoolIncarnation(GLResourcePool* pool)
{
    GameAllocationSpan span{};
    if (!pool || !FindGameAllocationSpan(pool,sizeof(GLResourcePool),span))
        throw std::invalid_argument("Native resource record requires a genuine allocated source pool");
    return span.incarnation;
}
class Reservation
{
public:
    Reservation(GLResourcePool* pool, std::size_t wii, std::size_t native)
    {
        if (!wii || !native || wii > std::numeric_limits<unsigned long>::max() ||
            native > std::numeric_limits<std::size_t>::max()-sizeof(Record)-31)
            throw std::length_error("Invalid native/Wii resource record size");
        const auto incarnation=PoolIncarnation(pool);
        {
            std::lock_guard lock(recordMutex);
            Store& store=TheStore();
            record_=static_cast<Record*>(store.heap.Allocate(sizeof(Record)+native+31,32,false));
            peakUsed=std::max<std::size_t>(peakUsed,kStoreBytes-store.heap.TotalFreeMemory());
        }
        new (record_) Record{};
        record_->pool=pool;
        auto address=reinterpret_cast<std::uintptr_t>(record_+1);
        record_->info.native=reinterpret_cast<void*>((address+31)&~std::uintptr_t(31));
        record_->info.nativeBytes=native;
        record_->info.wiiBytes=wii;
        record_->info.poolIncarnation=incarnation;
    }
    ~Reservation()
    {
        if (!record_) return;
        std::lock_guard lock(recordMutex);
        TheStore().heap.Free(record_);
    }
    void* Complete(void* quota)
    {
        GameGraphicsStorageSpan span{};
        if (!FindGameGraphicsStorage(quota,record_->info.wiiBytes,span) ||
            span.base!=quota || span.bytes!=record_->info.wiiBytes)
            throw std::invalid_argument("Native record quota is not the exact original GL allocation");
        if (PoolIncarnation(record_->pool)!=record_->info.poolIncarnation)
            throw std::invalid_argument("Native resource pool incarnation changed during allocation");
        record_->info.quota=quota;
        record_->info.quotaIncarnation=span.allocation.incarnation;
        record_->info.quotaStorageIncarnation=span.incarnation;
        std::lock_guard lock(recordMutex);
        record_->next=TheStore().records;
        TheStore().records=record_;
        auto* result=const_cast<void*>(record_->info.native);
        record_=nullptr;
        return result;
    }
private:
    Record* record_{};
};
}

void* AllocateGameResourceRecord(GLResourcePool* pool, std::size_t wiiBytes,
                                std::size_t nativeBytes)
{
    Reservation representation(pool,wiiBytes,nativeBytes);
    // Preserve exactly one source request, source GLX selection/alignment,
    // source logical usage/peak/overflow reporting and original pool lifetime.
    void* quota=glResourceAlloc(static_cast<unsigned long>(wiiBytes),GLM_Header,pool);
    return representation.Complete(quota);
}

void RetireGameResourceRecords(GLResourcePool* pool, const void* quotaBegin,
                              std::size_t quotaBytes) noexcept
{
    if (!quotaBytes) return;
    GameAllocationSpan owner{};
    if (!pool || !FindGameAllocationSpan(pool,sizeof(GLResourcePool),owner)) return;
    const auto begin=reinterpret_cast<std::uintptr_t>(quotaBegin);
    std::lock_guard lock(recordMutex);
    Record** link=&TheStore().records;
    while (*link)
    {
        Record* item=*link;
        const auto address=reinterpret_cast<std::uintptr_t>(item->info.quota);
        if (item->pool==pool && item->info.poolIncarnation==owner.incarnation &&
            address>=begin && address-begin<quotaBytes)
        {
            *link=item->next;
            TheStore().heap.Free(item);
        }
        else link=&item->next;
    }
}

void ReleaseGameResourceRecords(GLResourcePool* pool) noexcept
{
    GameAllocationSpan owner{};
    if (!pool || !FindGameAllocationSpan(pool,sizeof(GLResourcePool),owner)) return;
    std::lock_guard lock(recordMutex);
    Record** link=&TheStore().records;
    while (*link)
    {
        Record* item=*link;
        if (item->pool==pool && item->info.poolIncarnation==owner.incarnation)
        {
            *link=item->next;
            TheStore().heap.Free(item);
        }
        else link=&item->next;
    }
}

bool FindGameResourceRecord(const void* native, GameResourceRecordInfo& info) noexcept
{
    std::lock_guard lock(recordMutex);
    for (Record* item=TheStore().records; item; item=item->next)
        if (item->info.native==native) { info=item->info; return true; }
    return false;
}

void GameResourceRecordStorage(const void** base, std::size_t* bytes)
{
    *base=&TheStore();
    *bytes=kStoreBytes;
}

void GameResourceRecordUsage(std::size_t* used, std::size_t* peak, std::size_t* capacity)
{
    std::lock_guard lock(recordMutex);
    *used=kStoreBytes-TheStore().heap.TotalFreeMemory();
    *peak=peakUsed;
    *capacity=kStoreBytes;
}
}
