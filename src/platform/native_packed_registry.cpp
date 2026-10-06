#include "platform/native_packed_registry.h"
#include "platform/game_allocation_ownership.h"
#include "platform/host_metadata.h"
#include "NL/nlRegistry.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <limits>
#include <new>
#include <stdexcept>
#include <vector>

namespace mscharged::platform {
namespace {
template<class T> struct MetadataAllocator {
    using value_type=T;
    MetadataAllocator()=default;
    template<class U> MetadataAllocator(const MetadataAllocator<U>&) noexcept {}
    T* allocate(std::size_t count) {
        static_assert(alignof(T)<=alignof(std::max_align_t));
        if(count>std::numeric_limits<std::size_t>::max()/sizeof(T))throw std::bad_alloc();
        return static_cast<T*>(ChargedNativeMetadataAllocate(count*sizeof(T)));
    }
    void deallocate(T* pointer,std::size_t) noexcept {ChargedNativeMetadataRelease(pointer);}
    template<class U> bool operator==(const MetadataAllocator<U>&) const noexcept {return true;}
};
struct Layout {
    std::size_t raw,native,entries,bytes;
    std::uint16_t named,unnamed;
};
using Layouts=std::vector<Layout,MetadataAllocator<Layout>>;
std::size_t Aligned(std::size_t bytes,std::size_t alignment) {
    if(bytes>std::numeric_limits<std::size_t>::max()-(alignment-1))throw std::length_error("Registry native layout overflows");
    return (bytes+alignment-1)&~(alignment-1);
}
class RawImage {
public:
    RawImage(const void* data,std::size_t bytes):data_(static_cast<const unsigned char*>(data)),bytes_(bytes) {}
    void Range(std::size_t offset,std::size_t bytes) const {
        if(offset>bytes_ || bytes>bytes_-offset)throw std::length_error("Serialized registry field exceeds completed image bytes");
    }
    std::uint16_t U16(std::size_t at) const {
        Range(at,2);return std::uint16_t(data_[at])<<8|data_[at+1];
    }
    std::uint32_t U32(std::size_t at) const {
        Range(at,4);return std::uint32_t(data_[at])<<24|std::uint32_t(data_[at+1])<<16|std::uint32_t(data_[at+2])<<8|data_[at+3];
    }
    void Copy(void* output,std::size_t at,std::size_t bytes) const {Range(at,bytes);std::memcpy(output,data_+at,bytes);}
    std::size_t Size() const noexcept {return bytes_;}
    void Gather(Layouts& layouts,std::size_t offset) const {
        if(std::find_if(layouts.begin(),layouts.end(),[&](const Layout& v){return v.raw==offset;})!=layouts.end())return;
        Range(offset,8);
        const auto named=U16(offset+4),unnamed=U16(offset+6);
        const auto words=RegistryTypeWords(named)+RegistryTypeWords(unnamed);
        const auto entries=offset+8+std::size_t(words)*4;
        Range(entries,std::size_t(named)*8+std::size_t(unnamed)*4);
        layouts.push_back({offset,0,entries,0,named,unnamed});
        // This enumerates serialized pointer fields for width conversion only.
        // Original Load/Relocate performs every runtime marker/selection walk.
        for(unsigned i=0;i<unsigned(named)+unnamed;++i) {
            const bool keyed=i<named;
            const unsigned index=keyed?i:i-named;
            const auto typeWord=U32(offset+8+(keyed?index/16:RegistryTypeWords(named)+index/16)*4);
            if(((typeWord>>(index%16*2))&3)!=3)continue;
            const auto slot=keyed?entries+i*8+4:entries+std::size_t(named)*8+index*4;
            const auto child=std::size_t(U32(slot));
            if(child>bytes_-offset)throw std::length_error("Registry child offset exceeds completed image bytes");
            Gather(layouts,offset+child);
        }
    }
private:
    const unsigned char* data_;
    std::size_t bytes_;
};
std::uintptr_t SlotWord(const RawImage& raw,const Layouts& layouts,const Layout& parent,std::size_t at,int type) {
    const auto word=raw.U32(at);
    if(type==2 && word>raw.Size()-raw.U32(8))
        throw std::length_error("Registry type2 offset exceeds its completed raw relocation payload");
    if(type!=3)return word;
    const auto child=std::find_if(layouts.begin(),layouts.end(),[&](const Layout& v){return v.raw==parent.raw+word;});
    if(child==layouts.end() || child->native<parent.native || child->native-parent.native>std::numeric_limits<std::uint32_t>::max())
        throw std::length_error("Expanded child offset has no representable original relative word");
    return child->native-parent.native;
}
}

void* PrepareNativePackedRegistryImage(const void* rawData,std::size_t bytes) {
    GameCompletedSpan completed{};
    if(!FindGameCompletedSpan(rawData,bytes,completed) || FindGameByteDomain(rawData,bytes)!=GameByteDomain::WiiSerialized)
        throw std::invalid_argument("Packed registry requires genuine completed serialized source bytes");
    RawImage raw(rawData,bytes);
    raw.Range(0,20);
    const auto relocation=raw.U32(8);
    raw.Range(relocation,0);
    Layouts layouts;
    raw.Gather(layouts,12);
    std::sort(layouts.begin(),layouts.end(),[](const Layout& a,const Layout& b){return a.raw<b.raw;});
    if(layouts.front().raw!=12)throw std::invalid_argument("Packed registry root has no native image header");
    auto cursor=offsetof(PackedRegistryImage,mRoot);
    for(auto& layout:layouts) {
        layout.native=Aligned(cursor,alignof(PackedRegistryContainer));
        const auto words=RegistryTypeWords(layout.named)+RegistryTypeWords(layout.unnamed);
        const auto table=Aligned(sizeof(PackedRegistryContainer)+std::size_t(words)*4,alignof(PackedRegistryEntry));
        layout.bytes=table+std::size_t(layout.named)*sizeof(PackedRegistryEntry)+std::size_t(layout.unnamed)*sizeof(void*);
        if(layout.bytes>std::numeric_limits<std::size_t>::max()-layout.native)throw std::length_error("Registry native backing overflows");
        cursor=layout.native+layout.bytes;
    }
    const auto payload=Aligned(cursor,alignof(PackedRegistryContainer));
    if(payload>std::numeric_limits<std::uint32_t>::max() || bytes-relocation>std::numeric_limits<std::size_t>::max()-payload)
        throw std::length_error("Native registry relocation base is unrepresentable");
    GameNativeBackingReservation backing(rawData,bytes,payload+bytes-relocation);
    auto* data=static_cast<unsigned char*>(backing.Data());
    std::memset(data,0,payload+bytes-relocation);
    auto* image=reinterpret_cast<PackedRegistryImage*>(data);
    image->mReserved00=raw.U32(0);image->mReserved04=raw.U32(4);image->mRelocationBaseOffset=static_cast<u32>(payload);
    for(const auto& layout:layouts) {
        auto* container=new(data+layout.native) PackedRegistryContainer;
        // Establish the real native type/lifetime, then transport the original
        // vptr marker until the unchanged source installs the parent's vptr.
        const std::uint32_t marker=raw.U32(layout.raw);
        std::memset(container,0,sizeof(void*));
        std::memcpy(container,&marker,sizeof(marker));
        container->mNamedCount=layout.named;container->mUnnamedCount=layout.unnamed;
        auto* types=const_cast<u32*>(container->NamedTypes());
        const auto words=RegistryTypeWords(layout.named)+RegistryTypeWords(layout.unnamed);
        for(unsigned i=0;i<words;++i)types[i]=raw.U32(layout.raw+8+i*4);
        auto* entries=const_cast<PackedRegistryEntry*>(container->NamedEntries());
        for(unsigned i=0;i<layout.named;++i) {
            entries[i].mHash=raw.U32(layout.entries+i*8);
            entries[i].mData=reinterpret_cast<void*>(SlotWord(raw,layouts,layout,layout.entries+i*8+4,RegistryTypeAt(types,i)));
        }
        auto** unnamed=const_cast<void**>(container->UnnamedEntries());
        for(unsigned i=0;i<layout.unnamed;++i)
            unnamed[i]=reinterpret_cast<void*>(SlotWord(raw,layouts,layout,layout.entries+std::size_t(layout.named)*8+i*4,RegistryTypeAt(container->UnnamedTypes(),i)));
    }
    raw.Copy(data+payload,relocation,bytes-relocation);
    backing.Commit();
    return data;
}
}
