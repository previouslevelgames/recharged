#if !defined(MSCHARGED_NATIVE) || !defined(MSCHARGED_GAME_MODULE)
#error Effects data transport belongs beneath the original game-module loader
#endif
#include "platform/effects_data_abi.h"
#include "platform/game_allocation_ownership.h"
#include "platform/host_metadata.h"
#include "NL/nlChunk.h"
#include "Game/Effects/EffectsBundleData.h"
#include "Game/Effects/EffectsGroup.h"
#include "Game/Effects/EffectsTemplate.h"
#include <dolphin/os.h>
#include <bit>
#include <cstring>
#include <limits>
#include <list>
#include <stdexcept>
#include <utility>

namespace mscharged::platform
{
std::uint32_t EncodeEffectsAddress(const void* address)
{
    if (!address) return 0;
    const auto physical = OSCachedToPhysical(const_cast<void*>(address));
    if (physical & 0xe0000000u)
        throw std::out_of_range("Effects address has no Wii cached-word representation");
    return physical | 0x80000000u;
}
void* DecodeEffectsAddress(std::uint32_t word)
{
    if (!word) return nullptr;
    if ((word & 0xe0000000u) != 0x80000000u)
        throw std::out_of_range("Effects address word is outside Wii cached memory");
    return OSPhysicalToCached(word & 0x1fffffffu);
}
namespace
{
static_assert(sizeof(nlChunk)==8);
static_assert(sizeof(EffectsBundleData)==24 && offsetof(EffectsBundleData,mTemplates)==12);
static_assert(sizeof(fxAnimatedRange)==20 && offsetof(fxAnimatedRange,mKeys)==16);
static_assert(sizeof(EffectsTemplate)==224 && offsetof(EffectsTemplate,m_cColour)==120);
static_assert(sizeof(EffectsSpec)==88 && offsetof(EffectsSpec,mPadding04C)==76);
static_assert(sizeof(EffectsGroup)==28 && offsetof(EffectsGroup,mUserSpecSources)==24);
static_assert(sizeof(UserEffectSource)==8);
static_assert(sizeof(float)==4 && std::numeric_limits<float>::is_iec559);

template<class T> struct HostAllocator
{
    using value_type=T;
    HostAllocator()=default;
    template<class U> HostAllocator(const HostAllocator<U>&) noexcept {}
    T* allocate(std::size_t count)
    {
        static_assert(alignof(T)<=alignof(std::max_align_t));
        if(count>std::numeric_limits<std::size_t>::max()/sizeof(T)) throw std::bad_alloc();
        return static_cast<T*>(ChargedNativeMetadataAllocate(count*sizeof(T)));
    }
    void deallocate(T* p,std::size_t) noexcept { ChargedNativeMetadataRelease(p); }
    template<class U> bool operator==(const HostAllocator<U>&) const noexcept { return true; }
};
struct Plan
{
    unsigned char* data;
    std::size_t bytes;
    GameByteWriteReservation ticket;
    Plan(void* p,std::size_t count):data(static_cast<unsigned char*>(p)),bytes(count){}
};
using Plans=std::list<Plan,HostAllocator<Plan>>;
struct Image
{
    unsigned char* begin;
    std::size_t bytes;
    Plans plans;
    void Bound(const void* p,std::size_t n) const
    {
        const auto b=reinterpret_cast<std::uintptr_t>(begin),a=reinterpret_cast<std::uintptr_t>(p);
        if(a<b || a-b>bytes || n>bytes-(a-b))
            throw std::out_of_range("Effects record leaves its actual completed source range");
        GameCompletedSpan completed{};
        if(n && !FindGameCompletedSpan(p,n,completed))
            throw std::invalid_argument("Effects record has no actual completed logical source span");
    }
    std::uint32_t Word(const void* p) const
    {
        Bound(p,4);
        const auto domain=FindGameByteDomain(p,4);
        const auto* b=static_cast<const unsigned char*>(p);
        if(domain==GameByteDomain::WiiSerialized)
            return std::uint32_t(b[0])<<24 | std::uint32_t(b[1])<<16 | std::uint32_t(b[2])<<8 | b[3];
        if(domain!=GameByteDomain::NativePayload)
            throw std::invalid_argument("Effects scalar has an incompatible byte domain");
        std::uint32_t value;std::memcpy(&value,p,4);return value;
    }
    void Scalars(void* p,std::size_t n)
    {
        if(!n) return;
        if(n%4 || reinterpret_cast<std::uintptr_t>(p)%4)
            throw std::invalid_argument("Effects scalar cells require four-byte source alignment");
        Bound(p,n);
        const auto domain=FindGameByteDomain(p,n);
        if(domain==GameByteDomain::NativePayload) return;
        if(domain!=GameByteDomain::WiiSerialized)
            throw std::invalid_argument("Effects scalar range has an incompatible byte domain");
        plans.emplace_back(p,n);
    }
    struct Chunk
    {
        nlChunk* header;
        unsigned char* data;
        std::size_t bytes;
        nlChunk* next;
    };
    Chunk Read(nlChunk* c,std::uint32_t id) const
    {
        Bound(c,8);
        // Chunk headers remain WiiSerialized and preserve their original bytes.
        if(FindGameByteDomain(c,8)!=GameByteDomain::WiiSerialized)
            throw std::invalid_argument("Effects chunk header is not original serialized data");
        if(c->GetID()!=id) throw std::invalid_argument("Unrepresented effects record chunk ID");
        Bound(c,8+std::size_t(c->GetSize()));
        auto* data=static_cast<unsigned char*>(c->GetData());
        const auto skip=std::size_t(data-static_cast<unsigned char*>(c->GetUnalignedData()));
        if(skip>c->GetSize()) throw std::out_of_range("Effects chunk alignment exceeds its source size");
        auto* next=c->GetNextChunk();Bound(next,0);
        if(reinterpret_cast<std::uintptr_t>(next)<=reinterpret_cast<std::uintptr_t>(c))
            throw std::invalid_argument("Effects chunk makes no source progress");
        return {c,data,c->GetSize()-skip,next};
    }
    static void Size(const Chunk& c,std::size_t bytes)
    {
        if(c.bytes!=bytes) throw std::invalid_argument("Unrepresented effects record payload size");
    }
    void Property(const Chunk& container)
    {
        auto record=Read(container.header->GetFirstChunk(),0x24005);Size(record,20);
        // Parent GetNext includes its own header; children stop at parent data end.
        auto* bodyEnd=reinterpret_cast<nlChunk*>(container.data+container.bytes);
        const auto curved=Word(record.data);
        if(curved)
        {
            const auto count=Word(record.data+12);
            auto keys=Read(record.next,0x24006);
            if(count>keys.bytes/20 || count*std::size_t(20)!=keys.bytes)
                throw std::out_of_range("Effects curve keys leave their actual payload");
            Scalars(keys.data,keys.bytes);
            if(keys.next!=bodyEnd) throw std::invalid_argument("Unrepresented effects curve trailing bytes");
        }
        else if(record.next!=bodyEnd) throw std::invalid_argument("Unrepresented effects range trailing bytes");
        Scalars(record.data,20);
    }
    void Template(const Chunk& container)
    {
        auto record=Read(container.header->GetFirstChunk(),0x24003);
        if(record.bytes!=220 && record.bytes!=224)
            throw std::invalid_argument("Unrepresented effects template colour payload");
        // The original26-element declaration can cross into the following raw
        // header for220-byte authored records. It is bounded, never filled in.
        Bound(record.data,224);
        Scalars(record.data,0x34);Scalars(record.data+0x38,0x40);
        auto* current=record.next;
        for(unsigned i=0;i<8;++i)
        {
            auto property=Read(current,0x80024004);Property(property);current=property.next;
        }
        if(current!=reinterpret_cast<nlChunk*>(container.data+container.bytes))
            throw std::invalid_argument("Unrepresented effects template trailing bytes");
    }
    void Group(const Chunk& container,std::uint32_t templateCount)
    {
        auto header=Read(container.header->GetFirstChunk(),0x24021);Size(header,28);
        const auto count=Word(header.data+8),users=Word(header.data+20);
        auto specs=Read(header.next,0x24022);
        if(count>specs.bytes/88 || count*std::size_t(88)!=specs.bytes)
            throw std::out_of_range("Effects specs leave their actual payload");
        for(std::uint32_t i=0;i<count;++i)
        {
            auto* spec=specs.data+std::size_t(i)*88;
            // Only raw numeric template indices are checked; source resolution
            // later writes this same union's actual cached pointer word.
            if(FindGameByteDomain(spec+4,4)==GameByteDomain::WiiSerialized && Word(spec+4)>=templateCount)
                throw std::out_of_range("Effects template index has no original table cell");
            Scalars(spec,76);
        }
        auto sources=Read(specs.next,0x24023);
        if(users>sources.bytes/8 || users*std::size_t(8)!=sources.bytes)
            throw std::out_of_range("Effects user sources leave their actual payload");
        auto* current=sources.next;
        for(std::uint32_t i=0;i<users;++i)
        {
            Bound(current,8);
            auto user=Read(current,current->GetID());
            if(Word(sources.data+std::size_t(i)*8)>user.bytes)
                throw std::out_of_range("Effects parser bytes leave their actual user payload");
            if(user.header->GetID()&0x80000000u)
                throw std::invalid_argument("Effects user data requires an unrepresented container");
            current=user.next;
        }
        if(current!=reinterpret_cast<nlChunk*>(container.data+container.bytes))
            throw std::invalid_argument("Unrepresented effects group trailing bytes");
        Scalars(header.data,28);Scalars(sources.data,sources.bytes);
    }
    void Bundle(const Chunk& container)
    {
        auto header=Read(container.header->GetFirstChunk(),0x24001);Size(header,24);
        const auto templates=Word(header.data+8),groups=Word(header.data+16);
        auto table=Read(header.next,0x24025),groupTable=Read(table.next,0x24026);
        if(templates>table.bytes/4 || templates*std::size_t(4)!=table.bytes ||
           groups>groupTable.bytes/4 || groups*std::size_t(4)!=groupTable.bytes)
            throw std::out_of_range("Effects pointer table leaves its actual four-byte cells");
        Scalars(header.data+8,16);Scalars(table.data,table.bytes);Scalars(groupTable.data,groupTable.bytes);
        auto* current=groupTable.next;
        for(std::uint32_t i=0;i<templates;++i)
        {
            auto value=Read(current,0x80024002);Template(value);current=value.next;
        }
        for(std::uint32_t i=0;i<groups;++i)
        {
            auto value=Read(current,0x80024020);Group(value,templates);current=value.next;
        }
        if(current!=reinterpret_cast<nlChunk*>(container.data+container.bytes))
            throw std::invalid_argument("Unrepresented effects bundle trailing bytes");
    }
    void Commit()
    {
        // Reserve every domain metadata node before touching source bytes.
        for(auto& p:plans)
        {
            p.ticket=GameByteWriteReservation(p.data,p.bytes);
            if(!p.ticket.Tracked()) throw std::invalid_argument("Effects conversion lost its source allocation");
        }
        for(auto& p:plans)
        {
            if constexpr(std::endian::native==std::endian::little)
                for(std::size_t i=0;i<p.bytes;i+=4)
                {
                    const auto* b=p.data+i;
                    const std::uint32_t value=std::uint32_t(b[0])<<24|std::uint32_t(b[1])<<16|std::uint32_t(b[2])<<8|b[3];
                    std::memcpy(p.data+i,&value,4);
                }
            p.ticket.Complete(GameByteDomain::NativePayload);
        }
    }
};
}
void PrepareEffectsBundle(nlChunk* bundle)
{
    GameCompletedSpan source{};
    if(!FindGameCompletedSpan(bundle,8,source))
        throw std::invalid_argument("Effects bundle has no genuine completed NL source span");
    const auto address=reinterpret_cast<std::uintptr_t>(bundle),base=reinterpret_cast<std::uintptr_t>(source.base);
    if(address<base || address-base>source.bytes)
        throw std::out_of_range("Effects bundle leaves its completed source image");
    Image image{reinterpret_cast<unsigned char*>(bundle),source.bytes-(address-base),{}};
    auto container=image.Read(bundle,0x80024000);
    image.Bundle(container);
    image.Commit();
}
}
