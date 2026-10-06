#include "platform/game_resource_records.h"

#include "NL/gl/glMemory.h"
#include "platform/game_allocation_ownership.h"
#include "platform/host_metadata.h"
#include <limits>
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
Record* records;
std::mutex recordMutex;

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
        record_=static_cast<Record*>(ChargedNativeMetadataAllocate(sizeof(Record)+native+31));
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
        if (record_) ChargedNativeMetadataRelease(record_);
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
        record_->next=records;
        records=record_;
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
    Record** link=&records;
    while (*link)
    {
        Record* item=*link;
        const auto address=reinterpret_cast<std::uintptr_t>(item->info.quota);
        if (item->pool==pool && item->info.poolIncarnation==owner.incarnation &&
            address>=begin && address-begin<quotaBytes)
        {
            *link=item->next;
            ChargedNativeMetadataRelease(item);
        }
        else link=&item->next;
    }
}

void ReleaseGameResourceRecords(GLResourcePool* pool) noexcept
{
    GameAllocationSpan owner{};
    if (!pool || !FindGameAllocationSpan(pool,sizeof(GLResourcePool),owner)) return;
    std::lock_guard lock(recordMutex);
    Record** link=&records;
    while (*link)
    {
        Record* item=*link;
        if (item->pool==pool && item->info.poolIncarnation==owner.incarnation)
        {
            *link=item->next;
            ChargedNativeMetadataRelease(item);
        }
        else link=&item->next;
    }
}

bool FindGameResourceRecord(const void* native, GameResourceRecordInfo& info) noexcept
{
    std::lock_guard lock(recordMutex);
    for (Record* item=records; item; item=item->next)
        if (item->info.native==native) { info=item->info; return true; }
    return false;
}
}
