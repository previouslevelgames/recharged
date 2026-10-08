#include "platform/native_hbm_sound_archive.h"
#include "platform/game_allocation_ownership.h"
#include "platform/host_metadata.h"
#include "revolution/hbm/nw4hbm/snd/SoundArchiveFile.h"
#include <array>
#include <cstring>
#include <limits>
#include <optional>
#include <new>
#include <stdexcept>
#include <vector>

namespace mscharged::platform {
namespace {
namespace File = nw4hbm::snd::detail::SoundArchiveFile;
static_assert(sizeof(File::Header) == 40 && sizeof(nw4hbm::ut::BinaryFileHeader) == 16);
static_assert(sizeof(nw4hbm::snd::detail::Util::DataRef<void>) == 8);
static_assert(sizeof(File::StringTreeNode) == 20 && offsetof(File::StringTreeNode, id) == 16);
static_assert(sizeof(File::SoundCommonInfo) == 44 && offsetof(File::SoundCommonInfo, soundInfoRef) == 24);
static_assert(sizeof(File::Info) == 48 && sizeof(File::BankInfo) == 8);
static_assert(sizeof(File::PlayerInfo) == 12 && offsetof(File::PlayerInfo, heapSize) == 8);
static_assert(sizeof(File::FileInfo) == 28 && sizeof(File::GroupInfo) == 40);
static_assert(sizeof(File::GroupItemInfo) == 20 && sizeof(File::SoundArchivePlayerInfo) == 14);

enum class Kind : std::uint64_t { Header = 0x48424d4152434844ull,
    Info = 0x48424d415243494eull, Symbols = 0x48424d4152435359ull };
struct Prefix { Kind kind; std::size_t source_bytes; std::size_t payload_bytes; std::uint64_t reserved; };
static_assert(sizeof(Prefix) == 32);
struct Cell { std::size_t offset; std::uint32_t value; unsigned width; };
struct Copy { std::size_t source, target, bytes; };
template<class T> struct MetadataAllocator {
    using value_type=T;
    MetadataAllocator() noexcept=default;
    template<class U> MetadataAllocator(const MetadataAllocator<U>&) noexcept {}
    T* allocate(std::size_t n) {
        if (n>std::numeric_limits<std::size_t>::max()/sizeof(T)) throw std::bad_alloc();
        return static_cast<T*>(ChargedNativeMetadataAllocate(n*sizeof(T)));
    }
    void deallocate(T* p,std::size_t) noexcept { ChargedNativeMetadataRelease(p); }
    template<class U> bool operator==(const MetadataAllocator<U>&) const noexcept { return true; }
};
template<class T> using MetadataVector=std::vector<T,MetadataAllocator<T>>;
class Plan {
public:
    Plan(const void* source, std::size_t bytes) : source_(source), raw_(static_cast<const unsigned char*>(source)),
        bytes_(bytes), payload_bytes_(bytes) {
        GameCompletedSpan completed{};
        if (!source || !bytes || !FindGameCompletedSpan(source, bytes, completed)
            || FindGameByteDomain(source, bytes) != GameByteDomain::WiiSerialized)
            throw std::invalid_argument("HBM sound metadata lacks completed serialized owner bytes");
    }
    void Range(std::size_t offset, std::size_t bytes) const {
        if (offset > bytes_ || bytes > bytes_ - offset)
            throw std::invalid_argument("HBM sound metadata exceeds its completed source extent");
    }
    std::uint32_t Word(std::size_t offset) const {
        Range(offset, 4); return (std::uint32_t(raw_[offset]) << 24) | (std::uint32_t(raw_[offset+1]) << 16)
            | (std::uint32_t(raw_[offset+2]) << 8) | raw_[offset+3];
    }
    std::uint16_t Half(std::size_t offset) const {
        Range(offset, 2); return std::uint16_t(std::uint16_t(raw_[offset]) << 8 | raw_[offset+1]);
    }
    unsigned Byte(std::size_t offset) const { Range(offset, 1); return raw_[offset]; }
    void W(std::size_t offset) { cells_.push_back({offset, Word(offset), 4}); }
    void H(std::size_t offset) { cells_.push_back({offset, Half(offset), 2}); }
    void Words(std::size_t offset, std::size_t count) { for (std::size_t i=0; i<count; ++i) W(offset+i*4); }
    void String(std::size_t offset) const {
        Range(offset, 1);
        if (!std::memchr(raw_+offset, 0, bytes_-offset))
            throw std::invalid_argument("HBM sound metadata label/path has no source terminator");
    }
    std::optional<std::size_t> Ref(std::size_t offset, std::size_t minimum=1) {
        Range(offset, 8); H(offset+2); W(offset+4);
        const auto type=Byte(offset), value=Word(offset+4);
        if (type == 0 && value == 0) return std::nullopt;
        if (type != 1) throw std::invalid_argument("HBM sound metadata has an unqualified absolute-address reference");
        const std::size_t target=8+std::size_t(value); Range(target,minimum);
        return target;
    }
    MetadataVector<std::size_t> Table(std::size_t offset) {
        Range(offset,4);
        if (offset % 4) throw std::invalid_argument("HBM INFO table has an unqualified unaligned wire layout");
        const auto count=Word(offset); Range(offset+4,std::size_t(count)*8); W(offset);
        MetadataVector<std::size_t> items;
        for (std::size_t i=0; i<count; ++i) {
            const auto target=Ref(offset+4+i*8);
            // A null entry remains null in the source view. Callers already
            // decide whether to use it; no successful record is synthesized.
            items.push_back(target ? *target : 0);
        }
        return items;
    }
    void Record(std::size_t offset, std::size_t bytes, unsigned alignment=4) const {
        Range(offset,bytes);
        if (offset % alignment) throw std::invalid_argument("HBM INFO record has an unqualified unaligned wire layout");
    }
    std::size_t Relocate(std::size_t offset, std::size_t bytes) {
        Range(offset,bytes);
        const auto target=(payload_bytes_+3)&~std::size_t(3);
        if (bytes>std::numeric_limits<std::uint32_t>::max()
            || target > std::numeric_limits<std::uint32_t>::max()-bytes)
            throw std::invalid_argument("HBM sound native metadata offset exceeds its four-byte field");
        copies_.push_back({offset,target,bytes}); payload_bytes_=target+bytes;
        return target;
    }
    void AtWord(std::size_t target, std::uint32_t value) { cells_.push_back({target,value,4}); }
    void AtHalf(std::size_t target, std::uint16_t value) { cells_.push_back({target,value,2}); }
    const void* Commit(Kind kind) {
        GameNativeBackingSpan old{};
        if (FindGameNativeBacking(source_,bytes_,old)) {
            if (old.bytes != sizeof(Prefix)+payload_bytes_)
                throw std::invalid_argument("HBM sound metadata native backing extent differs");
            Prefix prefix{}; std::memcpy(&prefix,old.data,sizeof(prefix));
            if (prefix.kind != kind || prefix.source_bytes != bytes_ || prefix.payload_bytes != payload_bytes_)
                throw std::invalid_argument("HBM sound metadata source has a different native backing type");
            return static_cast<const unsigned char*>(old.data)+sizeof(Prefix);
        }
        GameNativeBackingReservation backing(source_,bytes_,sizeof(Prefix)+payload_bytes_);
        auto* storage=static_cast<unsigned char*>(backing.Data());
        const Prefix prefix{kind,bytes_,payload_bytes_,0}; std::memcpy(storage,&prefix,sizeof(prefix));
        auto* payload=storage+sizeof(Prefix);
        std::memset(payload,0,payload_bytes_); std::memcpy(payload,raw_,bytes_);
        for (const auto& copy:copies_) std::memcpy(payload+copy.target,raw_+copy.source,copy.bytes);
        for (const auto& cell:cells_) {
            if (cell.width == 4) std::memcpy(payload+cell.offset,&cell.value,4);
            else { const auto value=std::uint16_t(cell.value); std::memcpy(payload+cell.offset,&value,2); }
        }
        backing.Commit(); return payload;
    }
private:
    const void* source_; const unsigned char* raw_; std::size_t bytes_,payload_bytes_;
    MetadataVector<Cell> cells_; MetadataVector<Copy> copies_;
};
}

const void* NativeHBMSoundArchiveHeader(const void* source) {
    Plan plan(source,40);
    plan.W(0); plan.H(4); plan.H(6); plan.W(8); plan.H(12); plan.H(14); plan.Words(16,6);
    return plan.Commit(Kind::Header);
}

const void* NativeHBMSoundArchiveSymbols(const void* source, std::size_t bytes) {
    Plan plan(source,bytes); plan.Range(0,28); plan.Words(0,2);
    for (unsigned i=0; i<5; ++i) {
        const auto offset=plan.Word(8+i*4);
        if (!offset) { plan.W(8+i*4); continue; }
        const auto start=8+std::size_t(offset);
        plan.Range(start,i ? 8 : 4);
        const auto count=plan.Word(start+(i?4:0));
        const std::size_t length=i ? 8+std::size_t(count)*20 : 4+std::size_t(count)*4;
        const auto target=plan.Relocate(start,length);
        plan.AtWord(8+i*4,std::uint32_t(target-8));
        if (!i) {
            plan.AtWord(target,count);
            for (std::size_t j=0; j<count; ++j) {
                const auto string=plan.Word(start+4+j*4);
                plan.AtWord(target+4+j*4,string);
                if (string) plan.String(8+std::size_t(string));
            }
        } else {
            plan.AtWord(target,plan.Word(start)); plan.AtWord(target+4,count);
            for (std::size_t j=0; j<count; ++j) {
                const auto raw=start+8+j*20, native=target+8+j*20;
                plan.AtHalf(native,plan.Half(raw)); plan.AtHalf(native+2,plan.Half(raw+2));
                for (unsigned k=0; k<4; ++k) plan.AtWord(native+4+k*4,plan.Word(raw+4+k*4));
                if (!(plan.Half(raw)&1) && (plan.Word(raw+4)>=count || plan.Word(raw+8)>=count))
                    throw std::invalid_argument("HBM symbol tree child exceeds its original node table");
            }
        }
    }
    return plan.Commit(Kind::Symbols);
}

const void* NativeHBMSoundArchiveInfo(const void* source, std::size_t bytes, std::uint16_t version) {
    Plan plan(source,bytes); plan.Range(0,56); plan.Words(0,2);
    if (version != 0x101)
        throw std::invalid_argument("HBM sound INFO native transport currently qualifies authored version1.1 only");
    std::array<std::optional<std::size_t>,6> roots{};
    for (unsigned i=0; i<6; ++i) roots[i]=plan.Ref(8+i*8,i==5?14:4);
    for (unsigned t=0; t<5; ++t) {
        if (!roots[t]) continue;
        for (const auto record:plan.Table(*roots[t])) {
            if (!record) continue;
            if (t==0) {
                plan.Record(record,44); plan.Words(record,3); plan.Words(record+32,2);
                if (const auto param=plan.Ref(record+12,8)) { plan.Record(*param,8); plan.W(*param); }
                if (const auto detail=plan.Ref(record+24)) {
                    const auto type=plan.Byte(record+25);
                    if (type==1) { plan.Record(*detail,16); plan.Words(*detail,3); }
                    else if (type==3) { plan.Record(*detail,12); plan.Words(*detail,2); }
                    else if (type!=2) throw std::invalid_argument("HBM sound INFO detail has an unqualified record type");
                }
            } else if (t==1) { plan.Record(record,8); plan.Words(record,2); }
            else if (t==2) { plan.Record(record,12); plan.W(record); plan.W(record+8); }
            else if (t==3) {
                plan.Record(record,28); plan.Words(record,3);
                if (const auto path=plan.Ref(record+12)) plan.String(*path);
                if (const auto positions=plan.Ref(record+20,4))
                    for (const auto pos:plan.Table(*positions)) if (pos) { plan.Record(pos,8); plan.Words(pos,2); }
            } else {
                plan.Record(record,40); plan.Words(record,2); plan.Words(record+16,4);
                if (const auto path=plan.Ref(record+8)) plan.String(*path);
                if (const auto items=plan.Ref(record+32,4))
                    for (const auto item:plan.Table(*items)) if (item) { plan.Record(item,20); plan.Words(item,5); }
            }
        }
    }
    if (roots[5]) { plan.Record(*roots[5],14,2); for (unsigned i=0; i<7; ++i) plan.H(*roots[5]+i*2); }
    return plan.Commit(Kind::Info);
}
}
