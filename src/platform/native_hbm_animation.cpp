#include "platform/native_hbm_animation.h"
#include "platform/arc_data_transport.h"
#include "platform/game_allocation_ownership.h"
#include "platform/host_metadata.h"
#include "revolution/hbm/nw4hbm/lyt/resources.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <limits>
#include <new>
#include <stdexcept>
#include <vector>

namespace mscharged::platform {
namespace {
using namespace nw4hbm::lyt;
constexpr std::uint64_t AnimationBacking = 0x48424d414e494d31ull;
constexpr std::uint32_t RLAN=0x524c414e, PAI1=0x70616931;
constexpr std::uint32_t RLPA=0x524c5041, RLVC=0x524c5643, RLMC=0x524c4d43;
static_assert(sizeof(res::BinaryFileHeader)==16 && sizeof(res::DataBlockHeader)==8);
static_assert(sizeof(res::AnimationBlock)==20 && sizeof(res::AnimationContent)==24);
static_assert(sizeof(res::AnimationInfo)==8 && sizeof(res::AnimationTarget)==12);
static_assert(offsetof(res::AnimationTarget,keysOffset)==8);
static_assert(sizeof(res::HermiteKey)==12 && sizeof(res::StepKey)==8 && sizeof(float)==4);

template<class T> struct Allocator {
    using value_type=T;
    Allocator() noexcept=default;
    template<class U> Allocator(const Allocator<U>&) noexcept {}
    T* allocate(std::size_t count) {
        if(count>std::numeric_limits<std::size_t>::max()/sizeof(T)) throw std::bad_alloc();
        return static_cast<T*>(ChargedNativeMetadataAllocate(count*sizeof(T)));
    }
    void deallocate(T* pointer,std::size_t) noexcept {ChargedNativeMetadataRelease(pointer);}
    template<class U> bool operator==(const Allocator<U>&) const noexcept {return true;}
};
template<class T> using Vector=std::vector<T,Allocator<T>>;
struct Prefix {std::uint64_t kind;std::size_t sourceBytes;};
static_assert(sizeof(Prefix)%alignof(res::BinaryFileHeader)==0);
struct Field {std::size_t offset;std::uint32_t value;unsigned bytes;};
struct Record {std::size_t offset,bytes;unsigned type;};

class View {
public:
    View(const void* source,std::size_t bytes):source_(static_cast<const unsigned char*>(source)),bytes_(bytes) {}
    void Plan() {
        Range(0,16);RecordAt(0,16,1);
        Field16(4);Field16(6);Field32(8);Field16(12);Field16(14);
        // Original TestFileHeader rejects these before any block access. Keep
        // the actual header values; do not fabricate a supported file version.
        if(Half(4)!=0xfeff || Half(6)!=8) {CheckFields();return;}
        if(Word(0)!=RLAN)
            throw std::invalid_argument("Native HOME animation currently requires original RLAN0.8");
        if(Word(8)!=bytes_ || Half(12)!=16)
            throw std::invalid_argument("HBM animation header differs from its exact authored file bounds");
        std::size_t at=Half(12);
        for(unsigned i=0;i<Half(14);++i) {
            Range(at,8);
            const auto size=Word(at+4);
            if(size<8) throw std::invalid_argument("HBM animation has an incomplete block");
            Range(at,size);
            if(Word(at)==PAI1) AnimationBlock(at,size);
            else RecordAt(at,8,2); // Original loader ignores unknown block kinds.
            Field32(at+4);
            at+=size;
        }
        if(at!=bytes_) throw std::invalid_argument("HBM animation blocks do not cover their exact file");
        CheckFields();
    }
    const void* Commit() {
        if(bytes_>std::numeric_limits<std::size_t>::max()-sizeof(Prefix)) throw std::bad_alloc();
        GameNativeBackingSpan prior{};
        if(FindGameNativeBacking(source_,bytes_,prior)) return Header(prior,bytes_);
        GameNativeBackingReservation reservation(source_,bytes_,sizeof(Prefix)+bytes_);
        auto* storage=static_cast<unsigned char*>(reservation.Data());
        Prefix prefix{AnimationBacking,bytes_};
        std::memcpy(storage,&prefix,sizeof(prefix));
        auto* destination=storage+sizeof(prefix);
        std::memcpy(destination,source_,bytes_);
        for(const auto& field:fields_) {
            if(field.bytes==2) {
                const auto value=static_cast<std::uint16_t>(field.value);
                std::memcpy(destination+field.offset,&value,2);
            } else std::memcpy(destination+field.offset,&field.value,4);
        }
        reservation.Commit();return destination;
    }
private:
    const unsigned char* source_;std::size_t bytes_;
    Vector<Field> fields_;Vector<Record> records_;
    void Range(std::size_t offset,std::size_t count) const {
        if(offset>bytes_ || count>bytes_-offset)
            throw std::invalid_argument("HBM animation exceeds its exact ARC file extent");
    }
    void BlockRange(std::size_t at,std::size_t count,std::size_t begin,std::size_t end) const {
        if(at<begin || at>end || count>end-at)
            throw std::invalid_argument("HBM animation record exceeds its original block extent");
        Range(at,count);
    }
    std::uint16_t Half(std::size_t at) const {
        Range(at,2);return (std::uint16_t(source_[at])<<8)|source_[at+1];
    }
    std::uint32_t Word(std::size_t at) const {
        Range(at,4);return (std::uint32_t(source_[at])<<24)|(std::uint32_t(source_[at+1])<<16)
            |(std::uint32_t(source_[at+2])<<8)|source_[at+3];
    }
    void Field16(std::size_t at) {fields_.push_back({at,Half(at),2});}
    void Field32(std::size_t at) {fields_.push_back({at,Word(at),4});}
    void RecordAt(std::size_t at,std::size_t bytes,unsigned type) {
        Range(at,bytes);
        if(at%4) throw std::invalid_argument("HBM animation record lacks its original scalar alignment");
        for(const auto& old:records_) {
            if(at==old.offset && bytes==old.bytes && type==old.type) return;
            if(at<old.offset+old.bytes && old.offset<at+bytes)
                throw std::invalid_argument("HBM animation has overlapping incompatible records");
        }
        records_.push_back({at,bytes,type});
    }
    void OffsetTable(std::size_t at,unsigned count,std::size_t blockBegin,std::size_t blockEnd) {
        BlockRange(at,std::size_t(count)*4,blockBegin,blockEnd);
        if(count) RecordAt(at,std::size_t(count)*4,3);
        for(unsigned i=0;i<count;++i) Field32(at+std::size_t(i)*4);
    }
    void AnimationBlock(std::size_t begin,std::size_t size) {
        const auto end=begin+size;
        BlockRange(begin,20,begin,end);RecordAt(begin,20,4);
        Field16(begin+8);Field16(begin+12);Field16(begin+14);Field32(begin+16);
        if(Half(begin+12)!=0)
            throw std::invalid_argument("Native HOME animation file-reference records remain unqualified");
        const auto count=Half(begin+14);
        const auto table=begin+Word(begin+16);
        OffsetTable(table,count,begin,end);
        for(unsigned i=0;i<count;++i) {
            const auto content=begin+Word(table+std::size_t(i)*4);
            BlockRange(content,24,begin,end);RecordAt(content,24,5);
            if(source_[content+21]>1)
                throw std::invalid_argument("Native HOME animation content type remains unqualified");
            const unsigned infoCount=source_[content+20];
            OffsetTable(content+24,infoCount,begin,end);
            for(unsigned j=0;j<infoCount;++j) {
                const auto info=content+Word(content+24+std::size_t(j)*4);
                BlockRange(info,8,begin,end);RecordAt(info,8,6);Field32(info);
                const auto kind=Word(info);
                if(kind!=RLPA && kind!=RLVC && kind!=RLMC)
                    throw std::invalid_argument("Native HOME animation channel remains unqualified");
                const unsigned targetCount=source_[info+4];
                OffsetTable(info+8,targetCount,begin,end);
                for(unsigned k=0;k<targetCount;++k) {
                    const auto target=info+Word(info+8+std::size_t(k)*4);
                    BlockRange(target,12,begin,end);RecordAt(target,12,7);
                    Field16(target+4);Field32(target+8);
                    if(source_[target+2]!=2)
                        throw std::invalid_argument("Native HOME animation currently requires original Hermite records");
                    const auto keys=target+Word(target+8);
                    const unsigned keyCount=Half(target+4);
                    BlockRange(keys,std::size_t(keyCount)*12,begin,end);
                    if(!keyCount) throw std::invalid_argument("HBM animation has no bytes for its requested curve keys");
                    RecordAt(keys,std::size_t(keyCount)*12,8);
                    for(unsigned n=0;n<keyCount;++n) {
                        Field32(keys+std::size_t(n)*12);
                        Field32(keys+std::size_t(n)*12+4);
                        Field32(keys+std::size_t(n)*12+8);
                    }
                }
            }
        }
    }
    void CheckFields() {
        std::sort(fields_.begin(),fields_.end(),[](const Field& a,const Field& b){return a.offset<b.offset;});
        for(std::size_t i=1;i<fields_.size();++i) {
            const auto& a=fields_[i-1];const auto& b=fields_[i];
            if(a.offset+a.bytes>b.offset && !(a.offset==b.offset && a.bytes==b.bytes && a.value==b.value))
                throw std::invalid_argument("HBM animation scalar views overlap incompatibly");
        }
    }
    static const void* Header(const GameNativeBackingSpan& backing,std::size_t bytes) {
        Prefix prefix{};
        if(backing.bytes!=sizeof(Prefix)+bytes)
            throw std::invalid_argument("HBM animation backing has incompatible byte bounds");
        std::memcpy(&prefix,backing.data,sizeof(prefix));
        if(prefix.kind!=AnimationBacking || prefix.sourceBytes!=bytes)
            throw std::invalid_argument("HBM animation backing has a different typed owner");
        return static_cast<const unsigned char*>(backing.data)+sizeof(Prefix);
    }
};
}

const void* NativeHBMAnimationHeader(const void* originalResource) {
    NativeARCFileSpan file{};
    if(!FindNativeARCFileSpan(originalResource,file))
        throw std::invalid_argument("HBM animation requires its exact completed raw ARC file");
    View view(originalResource,file.bytes);view.Plan();return view.Commit();
}
}
