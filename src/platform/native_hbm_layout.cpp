#include "platform/native_hbm_layout.h"
#include "platform/arc_data_transport.h"
#include "platform/game_allocation_ownership.h"
#include "platform/host_metadata.h"
#include "revolution/hbm/nw4hbm/lyt/resources.h"
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <new>
#include <stdexcept>
#include <vector>

namespace mscharged::platform {
namespace {
using namespace nw4hbm::lyt;
constexpr std::uint64_t LayoutBacking=0x48424d4c41594f31ull;
constexpr std::uint32_t RLYT=0x524c5954;
static_assert(sizeof(res::BinaryFileHeader)==16 && sizeof(res::DataBlockHeader)==8);
static_assert(sizeof(res::Layout)==20 && sizeof(res::Pane)==76 && sizeof(res::Bounding)==76);
static_assert(sizeof(res::Picture)==96 && sizeof(res::TextBox)==116 && sizeof(res::Window)==104);
static_assert(sizeof(res::Group)==28 && sizeof(res::Material)==64);
static_assert(sizeof(res::TextureList)==12 && sizeof(res::FontList)==12 && sizeof(res::MaterialList)==12);
static_assert(sizeof(res::Texture)==8 && sizeof(res::Font)==8 && sizeof(res::TexMap)==4);
static_assert(sizeof(res::WindowContent)==20 && sizeof(res::WindowFrame)==4);
static_assert(sizeof(TexSRT)==20 && sizeof(TexCoordGen)==4 && sizeof(ChanCtrl)==4);
static_assert(sizeof(TevSwapMode)==1 && sizeof(IndirectStage)==4 && sizeof(TevStage)==16);
static_assert(sizeof(AlphaCompare)==4 && sizeof(BlendMode)==4 && sizeof(GXColor)==4 && sizeof(GXColorS10)==8);
static_assert(sizeof(wchar_t)==2 && sizeof(float)==4);
static_assert(offsetof(res::Pane,translate)==36 && offsetof(res::TextBox,textStrOffset)==88);
static_assert(offsetof(res::Material,resNum)==60 && offsetof(res::Window,contentOffset)==96);

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
struct Region {std::size_t offset,bytes;};
class View {
public:
    View(const void* data,std::size_t bytes):raw_(static_cast<const unsigned char*>(data)),bytes_(bytes) {}
    void Plan() {
        RegionAt(0,16);H(4);H(6);W(8);H(12);H(14);
        // The original header predicate rejects before touching any blocks.
        if(Half(4)!=0xfeff || Half(6)!=8 || Word(0)!=RLYT) {CheckFields();return;}
        if(Word(8)!=bytes_ || Half(12)!=16)
            throw std::invalid_argument("HBM layout header differs from its exact ARC file bounds");
        std::size_t at=16;unsigned paneDepth=0,groupDepth=0;
        bool lastPane=false;
        for(unsigned i=0;i<Half(14);++i) {
            Range(at,8);const auto size=Word(at+4);const auto kind=Word(at);
            if(size<8) throw std::invalid_argument("HBM layout has an incomplete block");
            Range(at,size);end_=at+size;begin_=at;W(at+4);
            switch(kind) {
            case 0x6c797431: // lyt1
                Fixed(at,20);W(at+12);W(at+16);break;
            case 0x74786c31: // txl1
                Names(at,textureCount_);break;
            case 0x666e6c31: // fnl1
                Names(at,fontCount_);break;
            case 0x6d617431: // mat1
                Materials(at);break;
            case 0x70616e31: case 0x626e6431: // pan1/bnd1
                Pane(at,76);lastPane=true;break;
            case 0x70696331: // pic1
                Pane(at,96);for(unsigned c=0;c<4;++c) W(at+76+std::size_t(c)*4);H(at+92);MaterialIndex(Half(at+92));
                TexCoords(at+96,raw_[at+94]);lastPane=true;break;
            case 0x74787431: // txt1
                Text(at);lastPane=true;break;
            case 0x776e6431: // wnd1
                Window(at);lastPane=true;break;
            case 0x70617331: // pas1
                Fixed(at,8);if(!lastPane) throw std::invalid_argument("HBM layout pane child block lacks its source parent");
                ++paneDepth;break;
            case 0x70616531: // pae1
                Fixed(at,8);if(!paneDepth) throw std::invalid_argument("HBM layout pane child block exceeds its source parents");
                --paneDepth;lastPane=true;break;
            case 0x67727031: // grp1
                Group(at);break;
            case 0x67727331: // grs1
                Fixed(at,8);++groupDepth;break;
            case 0x67726531: // gre1
                Fixed(at,8);if(!groupDepth) throw std::invalid_argument("HBM layout group child block exceeds its source parents");
                --groupDepth;break;
            default:
                // Preserve the original loader's ignored-block dispatch.
                Fixed(at,8);break;
            }
            at+=size;
        }
        if(at!=bytes_ || paneDepth || groupDepth)
            throw std::invalid_argument("HBM layout blocks do not cover a balanced exact file");
        CheckFields();
    }
    const void* Commit() {
        GameNativeBackingSpan prior{};
        if(FindGameNativeBacking(raw_,bytes_,prior)) return Header(prior,bytes_);
        if(bytes_>std::numeric_limits<std::size_t>::max()-sizeof(Prefix)) throw std::bad_alloc();
        GameNativeBackingReservation reservation(raw_,bytes_,sizeof(Prefix)+bytes_);
        auto* storage=static_cast<unsigned char*>(reservation.Data());
        const Prefix prefix{LayoutBacking,bytes_};std::memcpy(storage,&prefix,sizeof(prefix));
        auto* destination=storage+sizeof(prefix);std::memcpy(destination,raw_,bytes_);
        for(const auto& f:fields_) {
            if(f.bytes==2) {const auto value=static_cast<std::uint16_t>(f.value);std::memcpy(destination+f.offset,&value,2);}
            else std::memcpy(destination+f.offset,&f.value,4);
        }
        reservation.Commit();return destination;
    }
private:
    const unsigned char* raw_;std::size_t bytes_,begin_=0,end_=0;
    unsigned textureCount_=0,fontCount_=0,materialCount_=0;
    Vector<Field> fields_;Vector<Region> regions_;
    void Range(std::size_t at,std::size_t count) const {
        if(at>bytes_ || count>bytes_-at) throw std::invalid_argument("HBM layout exceeds its exact ARC file extent");
    }
    void BlockRange(std::size_t at,std::size_t count) const {
        if(at<begin_ || at>end_ || count>end_-at) throw std::invalid_argument("HBM layout record exceeds its source block");
        Range(at,count);
    }
    std::uint16_t Half(std::size_t at) const {Range(at,2);return (std::uint16_t(raw_[at])<<8)|raw_[at+1];}
    std::uint32_t Word(std::size_t at) const {
        Range(at,4);return (std::uint32_t(raw_[at])<<24)|(std::uint32_t(raw_[at+1])<<16)
            |(std::uint32_t(raw_[at+2])<<8)|raw_[at+3];
    }
    void H(std::size_t at) {fields_.push_back({at,Half(at),2});}
    void W(std::size_t at) {fields_.push_back({at,Word(at),4});}
    void RegionAt(std::size_t at,std::size_t count,unsigned alignment=4) {
        Range(at,count);
        if(at%alignment) throw std::invalid_argument("HBM layout record lacks its scalar alignment");
        if(!count) return;
        for(const auto& region:regions_) {
            if(at==region.offset && count==region.bytes) return;
            if(at<region.offset+region.bytes && region.offset<at+count)
                throw std::invalid_argument("HBM layout has overlapping incompatible records");
        }
        regions_.push_back({at,count});
    }
    void Fixed(std::size_t at,std::size_t count) {BlockRange(at,count);RegionAt(at,count);}
    void VariableString(std::size_t at) {
        BlockRange(at,1);
        const auto* terminator=static_cast<const unsigned char*>(std::memchr(raw_+at,0,end_-at));
        if(!terminator) throw std::invalid_argument("HBM layout resource name exceeds its block");
        RegionAt(at,std::size_t(terminator-(raw_+at))+1,1);
    }
    void Names(std::size_t at,unsigned& count) {
        Fixed(at,12);H(at+8);count=Half(at+8);
        const auto table=at+12;BlockRange(table,std::size_t(count)*8);RegionAt(table,std::size_t(count)*8);
        for(unsigned i=0;i<count;++i) {
            const auto entry=table+std::size_t(i)*8;W(entry);
            VariableString(table+Word(entry));
        }
    }
    void Materials(std::size_t at) {
        Fixed(at,12);H(at+8);materialCount_=Half(at+8);
        const auto table=at+12;BlockRange(table,std::size_t(materialCount_)*4);RegionAt(table,std::size_t(materialCount_)*4);
        for(unsigned i=0;i<materialCount_;++i) {
            W(table+std::size_t(i)*4);Material(at+Word(table+std::size_t(i)*4));
        }
    }
    void Material(std::size_t at) {
        Fixed(at,64);
        for(unsigned i=0;i<12;++i) H(at+20+std::size_t(i)*2);
        W(at+60);const auto bits=Word(at+60);
        const unsigned maps=bits&15,srts=(bits>>4)&15,coords=(bits>>8)&15;
        const unsigned swaps=(bits>>12)&1,indSRTs=(bits>>13)&3,indStages=(bits>>15)&7,tev=(bits>>18)&31;
        const unsigned alpha=(bits>>23)&1,blend=(bits>>24)&1,chan=(bits>>25)&1,color=(bits>>27)&1;
        std::size_t cursor=at+64;
        Fixed(cursor,std::size_t(maps)*4);
        for(unsigned i=0;i<maps;++i) {H(cursor+std::size_t(i)*4);if(Half(cursor+std::size_t(i)*4)>=textureCount_)
            throw std::invalid_argument("HBM layout material refers outside its source texture list");}
        cursor+=std::size_t(maps)*4;
        Scalars(cursor,std::size_t(srts)*20);cursor+=std::size_t(srts)*20;
        Fixed(cursor,std::size_t(coords)*4);cursor+=std::size_t(coords)*4;
        Fixed(cursor,std::size_t(chan)*4);cursor+=std::size_t(chan)*4;
        Fixed(cursor,std::size_t(color)*4);cursor+=std::size_t(color)*4;
        Fixed(cursor,std::size_t(swaps)*4);cursor+=std::size_t(swaps)*4;
        Scalars(cursor,std::size_t(indSRTs)*20);cursor+=std::size_t(indSRTs)*20;
        Fixed(cursor,std::size_t(indStages)*4);cursor+=std::size_t(indStages)*4;
        Fixed(cursor,std::size_t(tev)*16);cursor+=std::size_t(tev)*16;
        Fixed(cursor,std::size_t(alpha)*4);cursor+=std::size_t(alpha)*4;
        Fixed(cursor,std::size_t(blend)*4);
        // RGBA and byte-coded GX records deliberately stay byte-identical.
        // GXColor and ut::Color records have actual r/g/b/a byte members.
        // Pane colors declared u32 elsewhere use the canonical Color word ABI.
    }
    void MaterialIndex(unsigned index) const {
        if(index>=materialCount_) throw std::invalid_argument("HBM layout pane refers outside its source material list");
    }
    void Scalars(std::size_t at,std::size_t count) {
        Fixed(at,count);for(std::size_t i=0;i<count;i+=4) W(at+i);
    }
    void Pane(std::size_t at,std::size_t count) {
        Fixed(at,count);for(unsigned i=0;i<10;++i) W(at+36+std::size_t(i)*4);
    }
    void TexCoords(std::size_t at,unsigned count) {Scalars(at,std::size_t(count)*32);}
    void Text(std::size_t at) {
        Pane(at,116);for(unsigned i=0;i<4;++i) H(at+76+std::size_t(i)*2);
        MaterialIndex(Half(at+80));if(Half(at+82)>=fontCount_)
            throw std::invalid_argument("HBM layout text refers outside its source font list");
        W(at+88);W(at+92);W(at+96);for(unsigned i=0;i<4;++i) W(at+100+std::size_t(i)*4);
        const auto text=at+Word(at+88);const auto count=Half(at+78);
        if(count%2 || Half(at+76)%2) throw std::invalid_argument("HBM layout contains a partial original Wii16 text cell");
        BlockRange(text,count);RegionAt(text,count,2);
        for(unsigned i=0;i<count;i+=2) H(text+i);
        if(count && Half(text+count-2)!=0) throw std::invalid_argument("HBM layout text exceeds its terminated authored cells");
    }
    void Window(std::size_t at) {
        Pane(at,104);for(unsigned i=0;i<4;++i) W(at+76+std::size_t(i)*4);
        W(at+96);W(at+100);
        const auto content=at+Word(at+96);Fixed(content,20);for(unsigned c=0;c<4;++c) W(content+std::size_t(c)*4);H(content+16);MaterialIndex(Half(content+16));
        TexCoords(content+20,raw_[content+18]);
        const unsigned count=raw_[at+92];const auto table=at+Word(at+100);
        Fixed(table,std::size_t(count)*4);
        for(unsigned i=0;i<count;++i) {
            W(table+std::size_t(i)*4);const auto frame=at+Word(table+std::size_t(i)*4);
            Fixed(frame,4);H(frame);MaterialIndex(Half(frame));
        }
    }
    void Group(std::size_t at) {
        Fixed(at,28);H(at+24);Fixed(at+28,std::size_t(Half(at+24))*16);
    }
    void CheckFields() {
        std::sort(fields_.begin(),fields_.end(),[](const Field& a,const Field& b){return a.offset<b.offset;});
        for(std::size_t i=1;i<fields_.size();++i) {
            const auto& a=fields_[i-1];const auto& b=fields_[i];
            if(a.offset+a.bytes>b.offset && !(a.offset==b.offset && a.bytes==b.bytes && a.value==b.value))
                throw std::invalid_argument("HBM layout scalar views overlap incompatibly");
        }
    }
    static const void* Header(const GameNativeBackingSpan& backing,std::size_t bytes) {
        if(backing.bytes!=sizeof(Prefix)+bytes) throw std::invalid_argument("HBM layout backing byte bounds differ");
        Prefix prefix{};std::memcpy(&prefix,backing.data,sizeof(prefix));
        if(prefix.kind!=LayoutBacking || prefix.sourceBytes!=bytes)
            throw std::invalid_argument("HBM layout backing has a different typed owner");
        return static_cast<const unsigned char*>(backing.data)+sizeof(prefix);
    }
};
}
const void* NativeHBMLayoutHeader(const void* originalResource) {
    NativeARCFileSpan file{};
    if(!FindNativeARCFileSpan(originalResource,file))
        throw std::invalid_argument("HBM layout requires its exact completed raw ARC file");
    View view(originalResource,file.bytes);view.Plan();return view.Commit();
}
}
