#include "platform/native_audio_bank.h"
#include "platform/game_allocation_ownership.h"
#include "platform/host_metadata.h"
#include "Game/Audio/AudioBankTable.h"
#include "NL/nlChunk.h"
#include <cstring>
#include <limits>
#include <new>
#include <stdexcept>
#include <type_traits>
#include <vector>

namespace mscharged::platform {
namespace {
template<class T> class MetadataAllocator {
public:
    using value_type=T;
    MetadataAllocator()noexcept=default;
    template<class U>MetadataAllocator(const MetadataAllocator<U>&)noexcept{}
    T* allocate(std::size_t n) {
        if(n>std::numeric_limits<std::size_t>::max()/sizeof(T))throw std::bad_alloc();
        auto* p=ChargedNativeMetadataAllocate(n*sizeof(T));
        if(!p)throw std::bad_alloc();
        return static_cast<T*>(p);
    }
    void deallocate(T* p,std::size_t)noexcept{ChargedNativeMetadataRelease(p);}
    template<class U>bool operator==(const MetadataAllocator<U>&)const noexcept{return true;}
    template<class U>bool operator!=(const MetadataAllocator<U>&)const noexcept{return false;}
};
struct Child {
    nlChunk* header;
    const unsigned char* data;
    std::size_t bytes;
    std::size_t native_offset;
};
struct Storage {
    std::uint32_t marker;
    std::uint32_t original_origin;
    std::uint32_t source_count;
    std::uint32_t child_count;
    std::size_t records_offset;
};
constexpr std::uint32_t Marker=0x42414e4bu;
static_assert(sizeof(AudioBankTable)==48 && alignof(AudioBankTable)==8);
static_assert(sizeof(AudioBankGroup)==32 && alignof(AudioBankGroup)==8);
static_assert(sizeof(AudioResourceSource)==40 && alignof(AudioResourceSource)==8);
static_assert(sizeof(AudioResourceName)==16 && alignof(AudioResourceName)==8);
static_assert(std::is_trivially_destructible_v<AudioBankTable>);
std::uint32_t Word(const unsigned char* p) {
    return std::uint32_t(p[0])<<24|std::uint32_t(p[1])<<16|std::uint32_t(p[2])<<8|p[3];
}
template<class T>T* SavedWord(std::uint32_t word) {
    // These source slots carry old serialized relocation values only. They
    // are not registered memory or dereferenced before the original rebasing.
    return reinterpret_cast<T*>(std::uintptr_t(word));
}
std::size_t Extend(std::size_t& cursor,std::size_t count,std::size_t stride) {
    if(cursor>std::numeric_limits<std::size_t>::max()-7)throw std::overflow_error("Bank native alignment overflow");
    cursor=(cursor+7)&~std::size_t(7);const auto at=cursor;
    if(count>(std::numeric_limits<std::size_t>::max()-cursor)/stride)
        throw std::overflow_error("Bank native record extent overflow");
    cursor+=count*stride;return at;
}
const Storage& Validate(const NativeAudioBankView& view) {
    GameNativeBackingSpan backing{};
    if(!FindGameNativeBacking(view.source,view.source_bytes,backing)
        || backing.data!=view.data || backing.bytes!=view.native_bytes
        || backing.allocation.incarnation!=view.incarnation)
        throw std::invalid_argument("Bank typed backing has no live original NL allocation incarnation");
    auto& storage=*static_cast<const Storage*>(view.data);
    if(storage.marker!=Marker)throw std::invalid_argument("Bank source attachment has another typed layout");
    return storage;
}
const Child* Children(const Storage& storage) {
    return reinterpret_cast<const Child*>(&storage+1);
}
}

NativeAudioBankView PrepareNativeAudioBankChunk(nlChunk* chunk) {
    GameCompletedSpan completed{};
    if(!chunk || !FindGameCompletedSpan(chunk,8,completed))
        throw std::invalid_argument("Bank chunk has no original completed NL source");
    const auto bytes=std::size_t(chunk->GetSize())+8;
    if(!FindGameCompletedSpan(chunk,bytes,completed)
        || FindGameByteDomain(chunk,bytes)!=GameByteDomain::WiiSerialized)
        throw std::invalid_argument("Bank requires one exact serialized source extent");
    GameNativeBackingSpan existing{};
    if(FindGameNativeBacking(chunk,bytes,existing)) {
        NativeAudioBankView view{chunk,bytes,existing.data,existing.bytes,existing.allocation.incarnation};
        (void)Validate(view);return view;
    }
    std::vector<Child,MetadataAllocator<Child>> children;
    auto* end=chunk->GetLastChunk();
    auto* current=chunk->GetFirstChunk();
    const auto append=[&] {
        if(current>=end)throw std::out_of_range("Bank required child is absent from its actual parent");
        auto* next=current->GetNextChunk();
        if(next<=current || next>end)throw std::out_of_range("Bank child leaves its actual parent chunk");
        children.push_back({current,static_cast<const unsigned char*>(current->GetData()),current->GetDataSize(),0});
        current=next;
    };
    append();
    if(children[0].bytes<24)throw std::out_of_range("Bank original table header is incomplete");
    auto* header=children[0].data;
    const auto groups=Word(header),sources=Word(header+8),names=Word(header+16),origin=Word(header+12);
    // The original source consumes only4+groups+names children. Unrequested
    // trailing bytes/children are not another parse or readiness condition.
    if(bytes<40 || groups>(bytes-40)/8 || names>(bytes-40)/8-groups)
        throw std::out_of_range("Bank required child counts leave actual parent extent");
    const auto required=std::size_t(groups)+names+4;
    while(children.size()<required)append();
    if(groups>children.size()-4 || names>children.size()-4-groups
        || groups>children[1].bytes/20 || sources>children[2+groups].bytes/24
        || names>children[3+groups].bytes/8)
        throw std::out_of_range("Bank authored counts leave actual record chunks");
    for(std::size_t i=0;i<groups;++i) {
        const auto count=Word(children[1].data+i*20+8);
        if(count>children[2+i].bytes/4)throw std::out_of_range("Bank source-address list leaves its child extent");
        for(std::size_t j=0;j<count;++j) {
            const std::uint32_t offset=Word(children[2+i].data+j*4)-origin;
            if(offset%24 || offset/24>=sources)
                throw std::invalid_argument("Bank authored address is outside the original24-byte source array");
        }
    }
    for(std::size_t i=0;i<names;++i) {
        const auto& name=children[4+groups+i];
        if(!name.bytes || !std::memchr(name.data,0,name.bytes))
            throw std::invalid_argument("Bank authored name has no terminator in its original child");
    }
    std::size_t cursor=sizeof(Storage);
    if(children.size()>(std::numeric_limits<std::size_t>::max()-cursor)/sizeof(Child))throw std::overflow_error("Bank typed child table overflow");
    cursor+=children.size()*sizeof(Child);
    children[0].native_offset=Extend(cursor,1,sizeof(AudioBankTable));
    children[1].native_offset=Extend(cursor,groups,sizeof(AudioBankGroup));
    for(std::size_t i=0;i<groups;++i)
        children[2+i].native_offset=Extend(cursor,Word(children[1].data+i*20+8),sizeof(AudioResourceSource*));
    children[2+groups].native_offset=Extend(cursor,sources,sizeof(AudioResourceSource));
    children[3+groups].native_offset=Extend(cursor,names,sizeof(AudioResourceName));
    // Name byte strings remain at the actual completed source addresses.
    GameNativeBackingReservation reservation(chunk,bytes,cursor);
    auto* base=static_cast<unsigned char*>(reservation.Data());
    auto* storage=new(base)Storage{Marker,origin,sources,static_cast<std::uint32_t>(children.size()),children[2+groups].native_offset};
    auto* descriptors=reinterpret_cast<Child*>(storage+1);
    for(std::size_t i=0;i<children.size();++i)new(descriptors+i)Child(children[i]);
    auto* table=new(base+children[0].native_offset)AudioBankTable{};
    table->count_00=groups;table->records_04=SavedWord<AudioBankGroup>(Word(header+4));
    table->count_08=sources;table->records_0C=SavedWord<AudioResourceSource>(origin);
    table->count_10=names;table->records_14=SavedWord<AudioResourceName>(Word(header+20));
    for(std::size_t i=0;i<groups;++i) {
        const auto* raw=children[1].data+i*20;
        auto* group=new(base+children[1].native_offset+i*sizeof(AudioBankGroup))AudioBankGroup{};
        group->field_00=Word(raw);group->field_04=Word(raw+4);group->count_08=Word(raw+8);
        group->records_0C=SavedWord<AudioResourceSource*>(Word(raw+12));
        group->field_10=raw[16];std::memcpy(group->pad_11,raw+17,3);
        for(std::size_t j=0;j<group->count_08;++j) {
            auto** entries=reinterpret_cast<AudioResourceSource**>(base+children[2+i].native_offset);
            new(entries+j)AudioResourceSource*(SavedWord<AudioResourceSource>(Word(children[2+i].data+j*4)));
        }
    }
    for(std::size_t i=0;i<sources;++i) {
        const auto* raw=children[2+groups].data+i*24;
        auto* source=new(base+children[2+groups].native_offset+i*sizeof(AudioResourceSource))AudioResourceSource{};
        source->field_00=Word(raw);source->field_04=Word(raw+4);
        source->resource=SavedWord<AudioResourceName>(Word(raw+8));source->field_0C=SavedWord<AudioBankGroup>(Word(raw+12));
        source->field_10=SavedWord<AudioResourceLoadOwner>(Word(raw+16));
        source->field_14=raw[20];source->useCompactCallback=raw[21];std::memcpy(source->pad_16,raw+22,2);
    }
    for(std::size_t i=0;i<names;++i) {
        const auto* raw=children[3+groups].data+i*8;
        auto* name=new(base+children[3+groups].native_offset+i*sizeof(AudioResourceName))AudioResourceName{};
        name->field_00=Word(raw);name->name=SavedWord<const char>(Word(raw+4));
    }
    reservation.Commit();
    return {chunk,bytes,base,cursor,completed.allocation.incarnation};
}

void* NativeAudioBankData(const NativeAudioBankView& view,nlChunk* child) {
    const auto& storage=Validate(view);const auto* children=Children(storage);
    for(std::size_t i=0;i<storage.child_count;++i)if(children[i].header==child) {
        if(!children[i].native_offset)return const_cast<unsigned char*>(children[i].data);
        return static_cast<unsigned char*>(view.data)+children[i].native_offset;
    }
    throw std::invalid_argument("Bank original chunk is outside this exact typed source image");
}
NativeAudioBankRelocation NativeAudioBankDelta(const NativeAudioBankView& view,
    AudioResourceSource* oldRecords,AudioResourceSource* records,std::uint32_t count) {
    const auto& storage=Validate(view);
    if(records!=reinterpret_cast<AudioResourceSource*>(static_cast<unsigned char*>(view.data)+storage.records_offset)
        || count!=storage.source_count)throw std::invalid_argument("Bank record rebasing lost its actual typed extent");
    if(oldRecords==records)return {view,storage.original_origin,records,count,true};
    if(reinterpret_cast<std::uintptr_t>(oldRecords)!=storage.original_origin)
        throw std::invalid_argument("Bank old-record value differs from authored32-bit origin");
    return {view,storage.original_origin,records,count,false};
}
AudioResourceSource* RelocateNativeAudioBankRecord(AudioResourceSource* original,
    const NativeAudioBankRelocation& relocation) {
    (void)Validate(relocation.view);
    const auto address=reinterpret_cast<std::uintptr_t>(original);
    if(relocation.already_native) {
        const auto base=reinterpret_cast<std::uintptr_t>(relocation.records);
        if(address<base || (address-base)%sizeof(AudioResourceSource)
            || (address-base)/sizeof(AudioResourceSource)>=relocation.count)
            throw std::out_of_range("Bank repeated source rebasing leaves its native array");
        return original;
    }
    if(address>std::numeric_limits<std::uint32_t>::max())throw std::invalid_argument("Bank serialized old address has an unsupported width");
    const std::uint32_t offset=std::uint32_t(address)-relocation.original_origin;
    if(offset%24 || offset/24>=relocation.count)throw std::out_of_range("Bank old address leaves authored source array");
    return relocation.records+offset/24;
}
}
