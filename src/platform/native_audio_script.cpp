#include "platform/native_audio_script.h"
#include "platform/game_allocation_ownership.h"
#include "platform/host_metadata.h"
#include "platform/vm_address_abi.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <new>
#include <stdexcept>
#include <vector>

namespace mscharged::platform {
namespace {
template<class T> class MetadataAllocator {
public:
    using value_type=T;
    MetadataAllocator() noexcept=default;
    template<class U> MetadataAllocator(const MetadataAllocator<U>&) noexcept {}
    T* allocate(std::size_t n) {
        if(n>std::numeric_limits<std::size_t>::max()/sizeof(T))throw std::bad_alloc();
        void* p=ChargedNativeMetadataAllocate(n*sizeof(T));
        if(!p)throw std::bad_alloc();
        return static_cast<T*>(p);
    }
    void deallocate(T* p,std::size_t) noexcept {ChargedNativeMetadataRelease(p);}
    template<class U> bool operator==(const MetadataAllocator<U>&) const noexcept{return true;}
    template<class U> bool operator!=(const MetadataAllocator<U>&) const noexcept{return false;}
};
struct Store {std::size_t offset;std::uint32_t value;unsigned width;};
struct Image {
    unsigned char* data;
    std::size_t bytes;
    std::vector<Store,MetadataAllocator<Store>> stores;
    void Span(std::size_t at,std::size_t n) const {
        if(at>bytes || n>bytes-at)throw std::out_of_range("Audio script field leaves actual NL completion");
    }
    std::uint32_t Word(std::size_t at) const {
        Span(at,4);const auto* p=data+at;
        return std::uint32_t(p[0])<<24 | std::uint32_t(p[1])<<16 | std::uint32_t(p[2])<<8 | p[3];
    }
    std::uint16_t Half(std::size_t at) const {
        Span(at,2);return std::uint16_t(data[at])<<8 | data[at+1];
    }
    void Put(std::size_t at,std::uint32_t value,unsigned width) {
        Span(at,width);
        for(const auto& store:stores) {
            if(at==store.offset && width==store.width && value==store.value)return;
            if(at<store.offset+store.width && store.offset<at+width)
                throw std::invalid_argument("Audio script serialized fields overlap incompatibly");
        }
        stores.push_back({at,value,width});
    }
    void PutWord(std::size_t at) {Put(at,Word(at),4);}
    void PutHalf(std::size_t at) {Put(at,Half(at),2);}
    void ScalarArray(std::size_t at,std::size_t count,std::size_t end) {
        if(at>end || count>(end-at)/4)throw std::out_of_range("Audio script scalar array leaves authored table");
        for(std::size_t i=0;i<count;++i)PutWord(at+i*4);
    }
};
}

void PrepareNativeAudioScriptData(void* data,unsigned int bytes) {
    if(!data || bytes<24)throw std::invalid_argument("Audio script serialized header is absent");
    GameCompletedSpan completion{};
    if(!FindGameCompletedSpan(data,bytes,completion)
        || FindGameByteDomain(data,bytes)!=GameByteDomain::WiiSerialized)
        throw std::invalid_argument("Audio script requires actual completed raw NL bytes; repeated relocation is unqualified");
    if(reinterpret_cast<std::uintptr_t>(data)%4)
        throw std::invalid_argument("Audio script requires original word alignment");
    Image image{static_cast<unsigned char*>(data),bytes,{}};
    if(image.Word(0)!=0x4d4d4742u)throw std::invalid_argument("Audio script serialized signature differs");
    const auto count=image.Word(8), bytecode=image.Word(12);
    const auto end=bytecode ? std::size_t(bytecode) : std::size_t(bytes);
    if(end<24 || end>bytes || count>(end-24)/8)
        throw std::out_of_range("Audio script entries leave authored table");
    const auto defaults=16+std::size_t(count)*8;
    const auto defaultsCount=image.Half(defaults);
    const auto defaultWords=image.Word(defaults+4);
    if(defaultWords<defaults+8 || defaultWords%4)
        throw std::invalid_argument("Audio script default array has unqualified serialized geometry");
    image.ScalarArray(defaultWords,defaultsCount,end);
    (void)EncodeVMAddress(image.data+defaultWords);
    image.PutHalf(defaults);image.PutHalf(defaults+2);image.PutWord(defaults+4);
    for(unsigned i=0;i<4;++i)image.PutWord(i*4);
    for(std::size_t i=0;i<count;++i) {
        const auto entry=16+i*8;
        const auto selection=image.Word(entry+4);
        if(selection<defaults+8 || selection%4 || selection>end || end-selection<4)
            throw std::out_of_range("Audio script selection leaves authored table");
        (void)EncodeVMAddress(image.data+selection);
        const auto values=image.Half(selection), conditions=image.Half(selection+2);
        auto cursor=std::size_t(selection)+4;
        image.ScalarArray(cursor,values,end);cursor+=std::size_t(values)*4;
        image.PutHalf(selection);image.PutHalf(selection+2);
        for(unsigned c=0;c<conditions;++c) {
            if(cursor>end || end-cursor<8)throw std::out_of_range("Audio script condition header leaves authored table");
            const auto arguments=image.Half(cursor+6);
            image.PutWord(cursor);image.PutHalf(cursor+4);image.PutHalf(cursor+6);
            cursor+=8;
            image.ScalarArray(cursor,arguments,end);cursor+=std::size_t(arguments)*4;
        }
        image.PutWord(entry);image.PutWord(entry+4);
    }
    if(bytecode) {
        // Original InterpreterCore performs its own complete in-place VM
        // conversion/relocation. Keep its bytes in the raw source domain.
        image.Span(bytecode,72);
        if(bytecode%4 || image.Word(bytecode)!=0xe11c2112u)
            throw std::invalid_argument("Audio script embedded bytecode has unqualified header geometry");
    }
    GameByteWriteReservation write(data,end);
    for(const auto& store:image.stores) {
        if(store.width==4)std::memcpy(image.data+store.offset,&store.value,4);
        else {const auto value=std::uint16_t(store.value);std::memcpy(image.data+store.offset,&value,2);}
    }
    write.Complete(GameByteDomain::NativePayload);
}
}
