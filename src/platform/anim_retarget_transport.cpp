#include "platform/anim_retarget_transport.h"
#include "platform/game_allocation_ownership.h"
#include "Game/SAnim/AnimRetargeter.h"
#include <bit>
#include <cstring>
#include <limits>
#include <new>
#include <stdexcept>
#include <type_traits>

namespace mscharged::platform {
namespace {
struct Entry { nlChunk* source; void* data; std::size_t bytes; };
struct Storage { std::uint32_t marker, version; std::size_t count, entries; };
struct Child { nlChunk* source; const unsigned char* raw; std::size_t bytes; };
struct IdentifierLayout : cIdentifier {
    static constexpr std::size_t HashOffset() { return offsetof(IdentifierLayout,m_uHashID); }
};
constexpr std::uint32_t Marker=0x41525447;
static_assert(sizeof(std::uint32_t)==4 && sizeof(short)==2);
static_assert(alignof(AnimRetargetList)<=8 && alignof(AnimRetarget)<=8 && alignof(Entry)<=8);
static_assert(std::is_trivially_destructible_v<AnimRetargetList> && std::is_trivially_destructible_v<AnimRetarget>);
std::uint32_t Word(const unsigned char* p) {
    return std::uint32_t(p[0])<<24|std::uint32_t(p[1])<<16|std::uint32_t(p[2])<<8|p[3];
}
std::size_t Extend(std::size_t& total,std::size_t count,std::size_t stride) {
    if(total>std::numeric_limits<std::size_t>::max()-7)throw std::overflow_error("Retarget native alignment overflows");
    total=(total+7)&~std::size_t(7);const auto offset=total;
    if(count>(std::numeric_limits<std::size_t>::max()-total)/stride)throw std::overflow_error("Retarget native extent overflows");
    total+=count*stride;return offset;
}
Child Describe(nlChunk* chunk,std::uintptr_t limit) {
    const auto at=reinterpret_cast<std::uintptr_t>(chunk);GameCompletedSpan span{};
    if(at>limit||limit-at<8||!FindGameCompletedSpan(chunk,8,span))throw std::out_of_range("Required original retarget child is absent");
    const auto size=std::size_t(chunk->GetSize());
    if(size>limit-at-8)throw std::out_of_range("Retarget child leaves its original parent");
    const auto end=at+8+size;
    if(end>std::numeric_limits<std::uintptr_t>::max()-3||((end+3)&~std::uintptr_t(3))>limit)throw std::out_of_range("Retarget child padding leaves original parent");
    auto* raw=static_cast<const unsigned char*>(chunk->GetData());const auto start=reinterpret_cast<std::uintptr_t>(raw);
    const auto bytes=std::size_t(chunk->GetDataSize());
    if(start<at+8||start>end||bytes>end-start||(bytes&&!FindGameCompletedSpan(raw,bytes,span)))throw std::out_of_range("Retarget payload leaves completed original bytes");
    return {chunk,raw,bytes};
}
NativeAnimRetargetView View(nlChunk* source,std::size_t bytes,const GameNativeBackingSpan& backing) {
    if(backing.bytes<sizeof(Storage))throw std::invalid_argument("Retarget native prefix is incomplete");
    const auto& s=*static_cast<const Storage*>(backing.data);
    if(s.marker!=Marker||s.version!=1||s.entries>backing.bytes||s.count>(backing.bytes-s.entries)/sizeof(Entry))throw std::invalid_argument("Retarget source has another native backing profile");
    return {source,bytes,backing.data,backing.bytes,backing.allocation.incarnation};
}
}
NativeAnimRetargetView PrepareNativeAnimRetarget(nlChunk* source) {
    GameCompletedSpan completed{};
    if(!source||!FindGameCompletedSpan(source,8,completed))throw std::invalid_argument("Retarget requires actual completed original NL bytes");
    const auto payload=std::size_t(source->GetSize())+8;
    if(!FindGameCompletedSpan(source,payload,completed))throw std::out_of_range("Retarget root leaves original completion");
    const auto at=reinterpret_cast<std::uintptr_t>(source),end=at+payload;
    if(end>std::numeric_limits<std::uintptr_t>::max()-3)throw std::overflow_error("Retarget root padding overflows");
    const auto bytes=((end+3)&~std::uintptr_t(3))-at;
    if(!FindGameCompletedSpan(source,bytes,completed)||FindGameByteDomain(source,bytes)!=GameByteDomain::WiiSerialized)throw std::invalid_argument("Retarget requires one completed Wii serialized span");
    GameNativeBackingSpan backing{};
    if(FindGameNativeBacking(source,bytes,backing))return View(source,bytes,backing);
    const auto limit=at+bytes;
    const auto header=Describe(source->GetFirstChunk(),limit);
    if(header.bytes<16)throw std::out_of_range("Original retarget list header is incomplete");
    // Original source preserves the identifier name cell; it overwrites only
    // the records pointer below. No supplied owned file has a non-null name.
    if(Word(header.raw))throw std::invalid_argument("Retarget identifier name has no qualified original pointer owner");
    const auto signedCount=std::bit_cast<std::int32_t>(Word(header.raw+8));
    const auto count=signedCount>0?std::size_t(signedCount):0;
    const auto container=Describe(header.source->GetNextChunk(),limit);
    const auto nestedLimit=reinterpret_cast<std::uintptr_t>(container.source->GetLastChunk());
    const auto records=Describe(container.source->GetFirstChunk(),nestedLimit);
    const auto capacity=records.bytes/16;
    if(count>capacity)throw std::out_of_range("Retarget list count leaves original records chunk");
    std::size_t total=sizeof(Storage);
    const auto entryOffset=Extend(total,2+count,sizeof(Entry));
    const auto listOffset=Extend(total,1,sizeof(AnimRetargetList));
    const auto recordOffset=Extend(total,capacity?capacity:1,sizeof(AnimRetarget));
    auto* cursor=records.source->GetNextChunk();
    for(std::size_t i=0;i<count;++i) {
        const auto child=Describe(cursor,nestedLimit);
        if(child.bytes%2)throw std::invalid_argument("Retarget map has a partial original signed-short cell");
        Extend(total,child.bytes/2?child.bytes/2:1,sizeof(short));
        cursor=cursor->GetNextChunk();
    }
    for(std::size_t i=count;i<capacity;++i)
        if(Word(records.raw+i*16+12))throw std::invalid_argument("Unassigned retarget map pointer has no qualified owner");
    GameNativeBackingReservation reservation(source,bytes,total);
    auto* base=static_cast<unsigned char*>(reservation.Data());
    new(base)Storage{Marker,1,2+count,entryOffset};
    auto* entries=reinterpret_cast<Entry*>(base+entryOffset);
    auto* list=new(base+listOffset)AnimRetargetList{};
    const auto hash=Word(header.raw+4);
    std::memcpy(base+listOffset+IdentifierLayout::HashOffset(),&hash,4);
    list->m_NumAnimRetargets=signedCount;
    new(entries)Entry{header.source,list,sizeof(AnimRetargetList)};
    auto* nativeRecords=reinterpret_cast<AnimRetarget*>(base+recordOffset);
    for(std::size_t i=0;i<capacity;++i) {
        auto* record=new(nativeRecords+i)AnimRetarget{};const auto* raw=records.raw+i*16;
        record->m_TargetHierarchySignature=Word(raw);
        record->m_NumBones=std::bit_cast<std::int32_t>(Word(raw+4));
        record->m_Unknown08=Word(raw+8);
        // The original loop assigns each supported m_pMap from its next child;
        // stale authored pointer values are not host addresses.
    }
    new(entries+1)Entry{records.source,nativeRecords,capacity*sizeof(AnimRetarget)};
    cursor=records.source->GetNextChunk();std::size_t used=recordOffset+(capacity?capacity:1)*sizeof(AnimRetarget);
    for(std::size_t i=0;i<count;++i) {
        const auto child=Describe(cursor,nestedLimit);const auto halves=child.bytes/2;
        const auto offset=Extend(used,halves?halves:1,sizeof(short));auto* map=reinterpret_cast<short*>(base+offset);
        for(std::size_t j=0;j<halves;++j) {
            const auto bits=std::uint16_t(std::uint16_t(child.raw[j*2])<<8|child.raw[j*2+1]);
            new(map+j)short(std::bit_cast<std::int16_t>(bits));
        }
        new(entries+2+i)Entry{cursor,map,halves*sizeof(short)};cursor=cursor->GetNextChunk();
    }
    if(used!=total)throw std::logic_error("Retarget measured native storage differs from population");
    reservation.Commit();
    if(!FindGameNativeBacking(source,bytes,backing))throw std::logic_error("Retarget native publication is absent");
    return View(source,bytes,backing);
}
void* NativeAnimRetargetChunkData(const NativeAnimRetargetView& view,nlChunk* child) {
    GameNativeBackingSpan backing{};
    if(!FindGameNativeBacking(view.source,view.source_bytes,backing)||backing.data!=view.data||backing.bytes!=view.native_bytes||backing.allocation.incarnation!=view.incarnation)throw std::invalid_argument("Retarget view has no live original source incarnation");
    View(view.source,view.source_bytes,backing);const auto& s=*static_cast<const Storage*>(view.data);
    auto* entries=reinterpret_cast<const Entry*>(static_cast<const unsigned char*>(view.data)+s.entries);
    for(std::size_t i=0;i<s.count;++i)if(entries[i].source==child)return entries[i].data;
    throw std::invalid_argument("Retarget child is outside original initializer projection");
}
namespace {
struct RecordProjection {
    const unsigned char* raw;
    std::size_t count;
    const AnimRetarget* native;
};
RecordProjection Records(const GameNativeBackingSourceSpan& origin) {
    auto* root = static_cast<nlChunk*>(const_cast<void*>(origin.source));
    const auto view = View(root, origin.source_bytes, origin.backing);
    GameCompletedSpan complete{};
    if (!FindGameCompletedSpan(root, origin.source_bytes, complete)
        || complete.allocation.incarnation != origin.backing.allocation.incarnation
        || FindGameByteDomain(root, origin.source_bytes) != GameByteDomain::WiiSerialized)
        throw std::invalid_argument("Retarget record has no completed original raw owner");
    const auto& storage = *static_cast<const Storage*>(view.data);
    if (storage.count < 2)
        throw std::invalid_argument("Retarget native backing has no original records child");
    const auto* entries = reinterpret_cast<const Entry*>(static_cast<const unsigned char*>(view.data) + storage.entries);
    const auto& entry = entries[1];
    const auto limit = reinterpret_cast<std::uintptr_t>(root) + origin.source_bytes;
    const auto child = Describe(entry.source, limit);
    if (entry.bytes % sizeof(AnimRetarget) || entry.bytes / sizeof(AnimRetarget) != child.bytes / 16)
        throw std::invalid_argument("Retarget native records extent differs from original raw records");
    const auto begin = reinterpret_cast<std::uintptr_t>(view.data);
    const auto recordAt = reinterpret_cast<std::uintptr_t>(entry.data);
    if (recordAt < begin || recordAt - begin > view.native_bytes || entry.bytes > view.native_bytes - (recordAt - begin))
        throw std::out_of_range("Retarget native records leave their current backing");
    return {child.raw, entry.bytes / sizeof(AnimRetarget), static_cast<const AnimRetarget*>(entry.data)};
}
}
const void* NativeAnimRetargetRawRecord(const AnimRetarget* record) {
    GameNativeBackingSourceSpan origin{};
    if (!record || !FindGameNativeBackingSource(record, sizeof(AnimRetarget), origin))
        throw std::invalid_argument("Retarget record has no current native source backing");
    const auto records = Records(origin);
    const auto begin = reinterpret_cast<std::uintptr_t>(records.native);
    const auto at = reinterpret_cast<std::uintptr_t>(record);
    if (at < begin || (at - begin) % sizeof(AnimRetarget) || (at - begin) / sizeof(AnimRetarget) >= records.count)
        throw std::invalid_argument("Retarget pointer is not an exact original native record");
    return records.raw + ((at - begin) / sizeof(AnimRetarget)) * 16;
}
const AnimRetarget* NativeAnimRetargetRecordFromRaw(const void* record) {
    GameNativeBackingSourceSpan origin{};
    if (!record || !FindGameNativeBackingForSource(record, 16, origin))
        throw std::invalid_argument("Retarget raw record has no current initialized native backing");
    const auto records = Records(origin);
    const auto begin = reinterpret_cast<std::uintptr_t>(records.raw);
    const auto at = reinterpret_cast<std::uintptr_t>(record);
    if (at < begin || (at - begin) % 16 || (at - begin) / 16 >= records.count)
        throw std::invalid_argument("Retarget word is not an exact original raw record");
    return records.native + (at - begin) / 16;
}
}
