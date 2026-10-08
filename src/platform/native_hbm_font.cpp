#include "platform/native_hbm_font.h"
#include "platform/arc_data_transport.h"
#include "platform/game_allocation_ownership.h"
#include "platform/host_metadata.h"
#include "revolution/hbm/nw4hbm/ut/fontResources.h"
#include "revolution/hbm/nw4hbm/ut/binaryFileFormat.h"

#include <cstring>
#include <limits>
#include <new>
#include <stdexcept>
#include <vector>

namespace mscharged::platform {
namespace {
using namespace nw4hbm::ut;
constexpr std::uint32_t RFNT=0x52464e54, RFNU=0x52464e55;
constexpr std::uint32_t FINF=0x46494e46, TGLP=0x54474c50, CWDH=0x43574448, CMAP=0x434d4150;
constexpr std::uint64_t FontBacking=0x48424d464f4e5431ull;
static_assert(sizeof(BinaryFileHeader)==16 && sizeof(BinaryBlockHeader)==8);
static_assert(sizeof(CharWidths)==3 && sizeof(FontInformation)==40 && sizeof(FontTextureGlyph)==32);
static_assert(offsetof(FontWidth,widthTable)==16 && offsetof(FontCodeMap,mapInfo)==16);

template<class T> struct Allocator {
    using value_type=T;
    Allocator() noexcept=default;
    template<class U> Allocator(const Allocator<U>&) noexcept {}
    T* allocate(std::size_t n) {
        if(n>std::numeric_limits<std::size_t>::max()/sizeof(T)) throw std::bad_alloc();
        return static_cast<T*>(ChargedNativeMetadataAllocate(n*sizeof(T)));
    }
    void deallocate(T* p,std::size_t) noexcept { ChargedNativeMetadataRelease(p); }
    template<class U> bool operator==(const Allocator<U>&) const noexcept { return true; }
};
template<class T> using Vector=std::vector<T,Allocator<T>>;
struct Prefix {
    std::uint64_t kind;
    std::size_t sourceBytes, headerOffset, nativeBytes, relocationCount;
};
struct Relocation { std::uint32_t wireOffset, raw; std::size_t nativeOffset, bytes; };
struct Block { std::uint32_t kind; std::size_t wireOffset, wireBytes, nativeOffset, nativeBytes; };

std::size_t Align(std::size_t n,std::size_t alignment) {
    if(n>std::numeric_limits<std::size_t>::max()-(alignment-1)) throw std::bad_alloc();
    return (n+alignment-1)&~(alignment-1);
}
class FontView {
public:
    FontView(void* source,std::size_t bytes):source_(static_cast<const unsigned char*>(source)),bytes_(bytes) {}
    void Range(std::size_t offset,std::size_t count) const {
        if(offset>bytes_ || count>bytes_-offset) throw std::invalid_argument("HBM font exceeds its actual file extent");
    }
    std::uint16_t Half(std::size_t offset) const {
        Range(offset,2);return (std::uint16_t(source_[offset])<<8)|source_[offset+1];
    }
    std::uint32_t Word(std::size_t offset) const {
        Range(offset,4);return (std::uint32_t(source_[offset])<<24)|(std::uint32_t(source_[offset+1])<<16)
            |(std::uint32_t(source_[offset+2])<<8)|source_[offset+3];
    }
    void Plan() {
        Range(0,16);
        if(Word(0)!=RFNT || Half(4)!=0xfeff || Half(6)!=0x104 || Half(12)!=16)
            throw std::invalid_argument("Native HBM font currently requires the serialized RFNT1.4 profile");
        const auto fileBytes=Word(8);
        if(fileBytes!=bytes_) throw std::invalid_argument("HBM font declared size differs from its exact file extent");
        std::size_t offset=16;
        nativeBytes_=16;
        for(unsigned i=0;i<Half(14);++i) {
            Range(offset,8);
            const auto kind=Word(offset), size=Word(offset+4);
            if(size<8) throw std::invalid_argument("HBM font has an incomplete block header");
            Range(offset,size);
            std::size_t extra=0;
            switch(kind) {
            case FINF: RequirePayload(size,24);extra=sizeof(FontInformation)-24;break;
            case TGLP: RequirePayload(size,24);extra=sizeof(FontTextureGlyph)-24;break;
            case CWDH: RequirePayload(size,8);extra=offsetof(FontWidth,widthTable)-8;break;
            case CMAP: RequirePayload(size,12);extra=offsetof(FontCodeMap,mapInfo)-12;break;
            default: break; // Original Rebuild owns GLGR/unknown-block decisions.
            }
            const auto nativeSize=Align(std::size_t(size)+extra,8);
            if(nativeSize>UINT32_MAX || nativeBytes_>UINT32_MAX-nativeSize)
                throw std::invalid_argument("Native HBM font exceeds its source block-size field");
            blocks_.push_back({kind,offset,size,nativeBytes_,nativeSize});
            relocations_.push_back({std::uint32_t(offset+8),0,nativeBytes_+8,nativeSize-8});
            if(kind==TGLP) {
                const auto p=offset+8;
                const std::size_t image=Word(p+20), imageBytes=std::size_t(Word(p+4))*Half(p+8);
                Range(image,imageBytes);
                if(!imageBytes) throw std::invalid_argument("HBM font texture extent is empty");
                for(const auto& previous:relocations_)
                    if(previous.wireOffset==image) throw std::invalid_argument("HBM font texture/record offsets overlap");
                relocations_.push_back({std::uint32_t(image),1,0,imageBytes});
            }
            offset+=size; nativeBytes_+=nativeSize;
        }
        if(offset!=bytes_) throw std::invalid_argument("HBM font block walk does not cover its authored file");
        ValidateLinks();
    }
    void* Commit() {
        GameNativeBackingSpan old{};
        if(FindGameNativeBacking(source_,bytes_,old)) {
            Prefix prefix=CheckPrefix(old,bytes_);
            if(prefix.nativeBytes!=nativeBytes_ || prefix.relocationCount!=relocations_.size())
                throw std::invalid_argument("HBM font native representation differs from its source");
            return static_cast<unsigned char*>(old.data)+prefix.headerOffset;
        }
        const auto tableBytes=relocations_.size()*sizeof(Relocation);
        const auto minimum=sizeof(Prefix)+tableBytes;
        const auto backingBytes=minimum+31+nativeBytes_;
        GameNativeBackingReservation backing(source_,bytes_,backingBytes);
        auto* storage=static_cast<unsigned char*>(backing.Data());
        const auto address=reinterpret_cast<std::uintptr_t>(storage);
        const auto headerOffset=Align(address+minimum,32)-address;
        Prefix prefix{FontBacking,bytes_,headerOffset,nativeBytes_,relocations_.size()};
        std::memcpy(storage,&prefix,sizeof(prefix));
        std::memcpy(storage+sizeof(prefix),relocations_.data(),tableBytes);
        auto* header=storage+headerOffset;
        std::memset(header,0,nativeBytes_);
        BinaryFileHeader file{Word(0),Half(4),Half(6),std::uint32_t(nativeBytes_),Half(12),Half(14)};
        std::memcpy(header,&file,sizeof(file));
        for(const auto& block:blocks_) CopyBlock(header,block);
        backing.Commit();
        return header;
    }
    static Prefix CheckPrefix(const GameNativeBackingSpan& backing,std::size_t sourceBytes) {
        Prefix prefix{};
        if(backing.bytes<sizeof(prefix)) throw std::invalid_argument("HBM font backing lacks its type metadata");
        std::memcpy(&prefix,backing.data,sizeof(prefix));
        if(prefix.kind!=FontBacking || prefix.sourceBytes!=sourceBytes
            || prefix.relocationCount>(backing.bytes-sizeof(prefix))/sizeof(Relocation)
            || prefix.headerOffset<sizeof(prefix)+prefix.relocationCount*sizeof(Relocation)
            || prefix.headerOffset>backing.bytes || prefix.nativeBytes>backing.bytes-prefix.headerOffset)
            throw std::invalid_argument("HBM font backing has an incompatible representation");
        return prefix;
    }
private:
    static void RequirePayload(std::size_t block,std::size_t payload) {
        if(block-8<payload) throw std::invalid_argument("HBM font record is truncated");
    }
    const Block& Record(std::uint32_t offset, std::uint32_t kind) const {
        for (const auto& block : blocks_)
            if (block.wireOffset + 8 == offset && block.kind == kind) return block;
        throw std::invalid_argument("HBM font offset is not an exact record of its declared type");
    }
    void ValidateLinks() const {
        // Check byte/record transport before the original Rebuild dereferences
        // those records. It still performs all pointer resolution and mutations.
        for (const auto& block : blocks_) {
            const auto p = block.wireOffset + 8;
            if (block.kind == FINF) {
                Record(Word(p + 8), TGLP);
                if (Word(p + 12)) Record(Word(p + 12), CWDH);
                if (Word(p + 16)) Record(Word(p + 16), CMAP);
            } else if (block.kind == CWDH || block.kind == CMAP) {
                const auto nextField = block.kind == CWDH ? 4 : 8;
                auto offset = Word(p + nextField);
                std::size_t visited = 0;
                while (offset) {
                    if (++visited > blocks_.size())
                        throw std::invalid_argument("HBM font record chain is cyclic");
                    const auto& next = Record(offset, block.kind);
                    offset = Word(next.wireOffset + 8 + nextField);
                }
            }
        }
    }
    template<class T> T* Offset(std::size_t offset) const { return reinterpret_cast<T*>(std::uintptr_t(Word(offset))); }
    void CopyBlock(unsigned char* header,const Block& block) {
        const auto p=block.wireOffset+8;
        auto* destination=header+block.nativeOffset;
        BinaryBlockHeader native{block.kind,std::uint32_t(block.nativeBytes)};
        std::memcpy(destination,&native,sizeof(native));destination+=8;
        switch(block.kind) {
        case FINF: {
            FontInformation record{};
            std::memcpy(&record,source_+p,8);record.alterCharIndex=Half(p+2);
            record.pGlyph=Offset<FontTextureGlyph>(p+8);record.pWidth=Offset<FontWidth>(p+12);
            record.pMap=Offset<FontCodeMap>(p+16);record.height=source_[p+20];record.width=source_[p+21];
            record.ascent=source_[p+22];record.padding_[0]=source_[p+23];
            std::memcpy(destination,&record,sizeof(record));
            std::memcpy(destination+sizeof(record),source_+p+24,block.wireBytes-32);
            break;
        }
        case TGLP: {
            FontTextureGlyph record{};
            std::memcpy(&record,source_+p,4);record.sheetSize=Word(p+4);record.sheetNum=Half(p+8);
            record.sheetFormat=Half(p+10);record.sheetRow=Half(p+12);record.sheetLine=Half(p+14);
            record.sheetWidth=Half(p+16);record.sheetHeight=Half(p+18);record.sheetImage=Offset<u8>(p+20);
            std::memcpy(destination,&record,sizeof(record));
            std::memcpy(destination+sizeof(record),source_+p+24,block.wireBytes-32);
            break;
        }
        case CWDH: {
            FontWidth record{};record.indexBegin=Half(p);record.indexEnd=Half(p+2);record.pNext=Offset<FontWidth>(p+4);
            const auto count=std::size_t(record.indexEnd)-record.indexBegin+1;
            if(record.indexEnd<record.indexBegin || count>(block.wireBytes-16)/sizeof(CharWidths))
                throw std::invalid_argument("HBM font widths exceed their source record");
            std::memcpy(destination,&record,offsetof(FontWidth,widthTable));
            std::memcpy(destination+offsetof(FontWidth,widthTable),source_+p+8,block.wireBytes-16);
            break;
        }
        case CMAP: {
            FontCodeMap record{};record.ccodeBegin=Half(p);record.ccodeEnd=Half(p+2);
            record.mappingMethod=Half(p+4);record.reserved=Half(p+6);record.pNext=Offset<FontCodeMap>(p+8);
            const auto infoBytes=block.wireBytes-20;
            if(infoBytes%2) throw std::invalid_argument("HBM font mapping cells are not complete16-bit values");
            if(record.mappingMethod==0 && infoBytes<2)
                throw std::invalid_argument("HBM direct map lacks its offset cell");
            if(record.mappingMethod==1 && (record.ccodeEnd<record.ccodeBegin
                || std::size_t(record.ccodeEnd-record.ccodeBegin+1)>infoBytes/2))
                throw std::invalid_argument("HBM font table exceeds its source record");
            if(record.mappingMethod==2 && (infoBytes<2 || std::size_t(Half(p+12))*4>infoBytes-2))
                throw std::invalid_argument("HBM scan map exceeds its source record");
            std::memcpy(destination,&record,offsetof(FontCodeMap,mapInfo));
            for(std::size_t i=0;i<infoBytes;i+=2) {
                const auto cell=Half(p+12+i);std::memcpy(destination+offsetof(FontCodeMap,mapInfo)+i,&cell,2);
            }
            break;
        }
        default:std::memcpy(destination,source_+p,block.wireBytes-8);break;
        }
    }
    const unsigned char* source_;std::size_t bytes_,nativeBytes_{};
    Vector<Block> blocks_;Vector<Relocation> relocations_;
};
}

void* NativeHBMFontHeader(void* source) {
    GameCompletedSpan completed{};
    if(!source || !FindGameCompletedSpan(source,16,completed))
        throw std::invalid_argument("HBM font lacks completed original source bytes");
    std::size_t bytes;
    if(completed.base==source) bytes=completed.bytes;
    else {
        NativeARCFileSpan file{};
        if(!FindNativeARCFileSpan(source,file))
            throw std::invalid_argument("HBM font lacks an exact borrowed ARC file extent");
        bytes=file.bytes;
    }
    if(FindGameByteDomain(source,bytes)!=GameByteDomain::WiiSerialized)
        throw std::invalid_argument("HBM font source is not serialized data");
    FontView view(source,bytes);view.Plan();return view.Commit();
}

void* ResolveNativeHBMFontOffset(void* nativeHeader,std::uintptr_t offset) {
    GameNativeBackingSourceSpan owner{};
    if(!FindGameNativeBackingSource(nativeHeader,sizeof(BinaryFileHeader),owner))
        throw std::invalid_argument("HBM font pointer resolution lost its source owner");
    const auto prefix=FontView::CheckPrefix(owner.backing,owner.source_bytes);
    const auto* storage=static_cast<const unsigned char*>(owner.backing.data);
    if(nativeHeader!=storage+prefix.headerOffset)
        throw std::invalid_argument("HBM font pointer base is not its native file header");
    for(std::size_t i=0;i<prefix.relocationCount;++i) {
        Relocation relocation{};
        std::memcpy(&relocation,storage+sizeof(Prefix)+i*sizeof(Relocation),sizeof(relocation));
        if(relocation.wireOffset!=offset) continue;
        if(relocation.raw) {
            if(offset>owner.source_bytes || relocation.bytes>owner.source_bytes-offset)
                throw std::invalid_argument("HBM font texture pointer exceeds its original file");
            return const_cast<unsigned char*>(static_cast<const unsigned char*>(owner.source)+offset);
        }
        if(relocation.nativeOffset>prefix.nativeBytes || relocation.bytes>prefix.nativeBytes-relocation.nativeOffset)
            throw std::invalid_argument("HBM font record pointer exceeds its native backing");
        return static_cast<unsigned char*>(nativeHeader)+relocation.nativeOffset;
    }
    throw std::invalid_argument("HBM font pointer has no exact native record or raw sheet mapping");
}
}
