#include "platform/native_tpl.h"
#include "platform/game_allocation_ownership.h"
#include "platform/arc_data_transport.h"
#include "platform/host_metadata.h"
#include <dolphin/os.h>
#include <dolphin/gx.h>

#include <cstring>
#include <limits>
#include <new>
#include <stdexcept>
#include <utility>
#include <vector>

namespace mscharged::platform
{
namespace
{
template<class T> struct MetadataAllocator
{
    using value_type = T;
    MetadataAllocator() = default;
    template<class U> MetadataAllocator(const MetadataAllocator<U>&) noexcept {}
    T* allocate(std::size_t count)
    {
        static_assert(alignof(T) <= alignof(std::max_align_t));
        if (count > std::numeric_limits<std::size_t>::max() / sizeof(T)) throw std::bad_alloc();
        return static_cast<T*>(ChargedNativeMetadataAllocate(count * sizeof(T)));
    }
    void deallocate(T* pointer, std::size_t) noexcept { ChargedNativeMetadataRelease(pointer); }
    template<class U> bool operator==(const MetadataAllocator<U>&) const noexcept { return true; }
};
template<class T> using MetadataVector = std::vector<T, MetadataAllocator<T>>;
class Image
{
public:
    explicit Image(void* pointer) : data_(static_cast<unsigned char*>(pointer))
    {
        if (!FindGameCompletedSpan(pointer, 12, source_))
            throw std::invalid_argument("TPL requires actual completed allocation-owned NL bytes");
        const auto sourceAddress = reinterpret_cast<std::uintptr_t>(source_.base);
        const auto address = reinterpret_cast<std::uintptr_t>(pointer);
        if (address < sourceAddress || address - sourceAddress > source_.bytes)
            throw std::out_of_range("TPL palette leaves its original completed source span");
        if (pointer == source_.base)
            bytes_ = source_.bytes;
        else
        {
            NativeARCFileSpan file{};
            if (!FindNativeARCFileSpan(pointer, file))
                throw std::invalid_argument("Interior TPL has no exact original ARC file extent");
            bytes_ = file.bytes;
        }
        if (!FindGameCompletedSpan(pointer, bytes_, source_)
            || FindGameByteDomain(pointer, bytes_) != GameByteDomain::WiiSerialized)
            throw std::invalid_argument("TPL raw image is not uniformly completed Wii serialized bytes");
        if (address % 4)
            throw std::invalid_argument("TPL palette requires its original word alignment");
    }
    void Range(std::size_t offset, std::size_t bytes) const
    {
        if (offset > bytes_ || bytes > bytes_ - offset)
            throw std::out_of_range("TPL record leaves actual logical source bytes");
    }
    std::uint32_t Word(std::size_t offset) const
    {
        Range(offset, 4);
        return std::uint32_t(data_[offset]) << 24 | std::uint32_t(data_[offset + 1]) << 16
            | std::uint32_t(data_[offset + 2]) << 8 | data_[offset + 3];
    }
    std::uint16_t Half(std::size_t offset) const
    {
        Range(offset, 2);
        return std::uint16_t(data_[offset]) << 8 | data_[offset + 1];
    }
    std::uint32_t Address(std::size_t offset, std::size_t bytes) const
    {
        Range(offset, bytes);
        const auto physical = OSCachedToPhysical(data_ + offset);
        if (physical & 0xe0000000u)
            throw std::out_of_range("TPL pointer has no Wii cached-word representation");
        return physical | 0x80000000u;
    }
    unsigned char* At(std::size_t offset) const { return data_ + offset; }
    std::size_t Bytes() const noexcept { return bytes_; }
    const GameCompletedSpan& Source() const noexcept { return source_; }
private:
    unsigned char* data_;
    std::size_t bytes_;
    GameCompletedSpan source_{};
};
class NativeView
{
public:
    explicit NativeView(const Image& source)
        : data_(static_cast<unsigned char*>(ChargedNativeMetadataAllocate(source.Bytes())))
    {
        std::memcpy(data_, source.At(0), source.Bytes());
    }
    ~NativeView() { ChargedNativeMetadataRelease(data_); }
    NativeView(const NativeView&) = delete;
    NativeView& operator=(const NativeView&) = delete;
    unsigned char* At(std::size_t offset) const { return data_ + offset; }
    void Word(std::size_t offset, std::uint32_t value) { std::memcpy(At(offset), &value, 4); }
    void Half(std::size_t offset, std::uint16_t value) { std::memcpy(At(offset), &value, 2); }
    std::uint32_t Word(std::size_t offset) const
    {
        std::uint32_t value; std::memcpy(&value, At(offset), 4); return value;
    }
    std::uint16_t Half(std::size_t offset) const
    {
        std::uint16_t value; std::memcpy(&value, At(offset), 2); return value;
    }
private:
    unsigned char* data_;
};
bool Overlap(std::size_t a, std::size_t as, std::size_t b, std::size_t bs)
{
    return as && bs && (a <= b ? b - a < as : a - b < bs);
}
enum class Kind { Palette, Descriptors, Texture };
struct Record { std::size_t offset, bytes; Kind kind; };
struct Pixels { std::size_t offset, bytes; };
struct Binding
{
    Image source;
    NativeView view;
    MetadataVector<Record> records;
    MetadataVector<Pixels> pixels;
    bool committed = false;
    explicit Binding(void* palette) : source(palette), view(source)
    {
        Add(0, 12, Kind::Palette);
        for (std::size_t at = 0; at < 12; at += 4) view.Word(at, source.Word(at));
        // Magic validation belongs to original TPLBind's OSPanic branch.
    }
    bool Add(std::size_t offset, std::size_t bytes, Kind kind)
    {
        source.Range(offset, bytes);
        if (offset % 4) throw std::invalid_argument("TPL structural record requires original word alignment");
        for (const auto& record : records)
        {
            if (record.offset == offset && record.bytes == bytes && record.kind == kind) return false;
            if (Overlap(offset, bytes, record.offset, record.bytes))
                throw std::invalid_argument("TPL overlapping structural records remain unqualified");
        }
        for (const auto& pixel : pixels)
            if (Overlap(offset, bytes, pixel.offset, pixel.bytes))
                throw std::invalid_argument("TPL pixels overlapping converted headers remain unqualified");
        records.push_back({offset, bytes, kind});
        return true;
    }
    std::uint32_t Descriptors(std::uint32_t relative, std::uint32_t count)
    {
        // Preserve the source's u16 loop; do not repair empty/overflow inputs.
        if (!count || count > 0xffffu)
            throw std::invalid_argument("TPL descriptor count remains unqualified");
        const auto bytes = std::size_t(count) * 8;
        if (!Add(relative, bytes, Kind::Descriptors))
            throw std::logic_error("TPL source requested descriptor relocation twice");
        // Endian transport of cells only. The original loop selects which
        // non-null headers to relocate and whether their data is unpacked.
        for (std::size_t at = relative; at < relative + bytes; at += 4)
            view.Word(at, source.Word(at));
        return source.Address(relative, bytes);
    }
    std::uint32_t Texture(std::uint32_t relative)
    {
        if (Add(relative, 36, Kind::Texture))
        {
            view.Half(relative, source.Half(relative));
            view.Half(relative + 2, source.Half(relative + 2));
            for (std::size_t at = relative + 4; at <= relative + 28; at += 4)
                view.Word(at, source.Word(at));
            // All four byte-sized fields, including unpacked, remain exactly
            // authored. Only the original source assignment can change TRUE.
            const auto bytes = TextureBytes(relative);
            if (*view.At(relative + 35))
            {
                // This source branch will skip data relocation. Validate an
                // already-cached target without rewriting its word or marker.
                const auto word = view.Word(relative + 8);
                if ((word & 0xe0000000u) != 0x80000000u)
                    throw std::invalid_argument("Already-unpacked TPL data has no cached SDK representation");
                const auto target = reinterpret_cast<std::uintptr_t>(OSPhysicalToCached(word & 0x1fffffffu));
                const auto base = reinterpret_cast<std::uintptr_t>(source.At(0));
                if (target < base) throw std::out_of_range("Already-unpacked TPL target leaves its original file");
                PixelsAt(target - base, bytes);
            }
        }
        return source.Address(relative, 36);
    }
    std::size_t TextureBytes(std::size_t at) const
    {
        const auto height = view.Half(at), width = view.Half(at + 2);
        if (!height || !width || *view.At(at + 33) || *view.At(at + 34))
            throw std::invalid_argument("TPL native transport requires original nonmip data");
        std::size_t tileWidth, tileHeight;
        switch (view.Word(at + 4))
        {
        case GX_TF_I4: tileWidth = 8; tileHeight = 8; break;
        case GX_TF_IA4: tileWidth = 8; tileHeight = 4; break;
        case GX_TF_IA8:
        case GX_TF_RGB5A3: tileWidth = 4; tileHeight = 4; break;
        default: throw std::invalid_argument("TPL native texture format remains unqualified");
        }
        const auto columns = (std::size_t(width) + tileWidth - 1) / tileWidth;
        const auto rows = (std::size_t(height) + tileHeight - 1) / tileHeight;
        if (rows > std::numeric_limits<std::size_t>::max() / columns
            || columns * rows > std::numeric_limits<std::size_t>::max() / 32)
            throw std::out_of_range("TPL tiled pixel extent overflows native size_t");
        return columns * rows * 32;
    }
    void PixelsAt(std::size_t relative, std::size_t bytes)
    {
        source.Range(relative, bytes);
        for (const auto& record : records)
            if (Overlap(relative, bytes, record.offset, record.bytes))
                throw std::invalid_argument("TPL pixels overlapping converted headers remain unqualified");
        pixels.push_back({relative, bytes});
    }
    std::uint32_t Data(std::uint32_t relative, const void* header)
    {
        const auto address = reinterpret_cast<std::uintptr_t>(header);
        const auto base = reinterpret_cast<std::uintptr_t>(view.At(0));
        const Record* texture = nullptr;
        if (address >= base)
            for (const auto& record : records)
                if (record.kind == Kind::Texture && address - base == record.offset) texture = &record;
        if (!texture) throw std::invalid_argument("TPL data relocation has no original requested texture header");
        const auto at = texture->offset;
        const auto bytes = TextureBytes(at);
        PixelsAt(relative, bytes);
        return source.Address(relative, bytes);
    }
    void* Header(const void* raw, std::size_t bytes, std::size_t alignment) const
    {
        const auto address = reinterpret_cast<std::uintptr_t>(raw);
        const auto base = reinterpret_cast<std::uintptr_t>(source.At(0));
        if (address < base || address - base >= source.Bytes()) return nullptr;
        const auto offset = address - base;
        for (const auto& record : records)
            if (offset >= record.offset && offset - record.offset < record.bytes
                && bytes <= record.bytes - (offset - record.offset))
            {
                GameCompletedSpan current{};
                if (!FindGameCompletedSpan(raw, bytes, current)
                    || current.allocation.base != source.Source().allocation.base
                    || current.allocation.incarnation != source.Source().allocation.incarnation
                    || reinterpret_cast<std::uintptr_t>(view.At(offset)) % alignment)
                    throw std::invalid_argument("TPL native view lost its original source lifetime");
                return view.At(offset);
            }
        throw std::invalid_argument("TPL source requested an unconverted header view");
    }
    void Commit()
    {
        if (committed) throw std::logic_error("TPL source binding committed twice");
        MetadataVector<GameNativeHeaderProjection> projection;
        projection.reserve(records.size());
        for (const auto& record : records)
            projection.push_back({source.At(record.offset), view.At(record.offset), record.bytes});
        ProjectGameNativeHeaders(source.At(0), source.Bytes(), source.Source(), projection.data(), projection.size());
        committed = true;
    }
};
// A temporary representation view, not a second source owner or address bank.
// Every stored word still names real raw SDK memory. The source call has no
// callbacks; nested binding cannot borrow another file's temporary records.
thread_local Binding* activeBinding = nullptr;
}

bool NativeTPLAddressLessThan(const void* cell, std::uintptr_t boundary)
{
    GameCompletedSpan source{};
    if (boundary > std::numeric_limits<std::uint32_t>::max()
        || !FindGameCompletedSpan(cell, 4, source))
        throw std::invalid_argument("TPL word comparison requires a live cell and Wii address boundary");
    const auto domain = FindGameByteDomain(cell, 4);
    std::uint32_t word;
    if (domain == GameByteDomain::WiiSerialized)
    {
        const auto* raw = static_cast<const unsigned char*>(cell);
        word = std::uint32_t(raw[0]) << 24 | std::uint32_t(raw[1]) << 16
            | std::uint32_t(raw[2]) << 8 | raw[3];
    }
    else if (domain == GameByteDomain::NativeHeader)
        std::memcpy(&word, cell, sizeof(word));
    else
        throw std::invalid_argument("TPL address cell has no serialized/native-header domain");
    return word < boundary;
}

void* DecodeNativeTPLAddress(std::uint32_t word, std::size_t bytes,
                             std::size_t alignment, bool header)
{
    if (!word) return nullptr;
    if ((word & 0xe0000000u) != 0x80000000u)
        throw std::out_of_range("TPL address cell is outside Wii cached memory");
    auto* pointer = OSPhysicalToCached(word & 0x1fffffffu);
    if (!bytes || !alignment || reinterpret_cast<std::uintptr_t>(pointer) % alignment)
        throw std::out_of_range("TPL cached pointer has no aligned native representation");
    if (header && activeBinding)
        if (auto* view = activeBinding->Header(pointer, bytes, alignment)) return view;
    GameCompletedSpan source{};
    if (!FindGameCompletedSpan(pointer, bytes, source))
        throw std::out_of_range("TPL cached pointer has no live completed source backing");
    if (header && FindGameByteDomain(pointer, bytes) != GameByteDomain::NativeHeader)
        throw std::invalid_argument("TPL header pointer has no qualified native record domain");
    if (!header && FindGameByteDomain(pointer, bytes) != GameByteDomain::WiiSerialized)
        throw std::invalid_argument("TPL pixel pointer no longer refers to original serialized bytes");
    return pointer;
}

NativeTPLBinding::NativeTPLBinding(void* palette) : pending_(nullptr)
{
    if (activeBinding) throw std::logic_error("Nested original TPL binding remains unqualified");
    auto* storage = ChargedNativeMetadataAllocate(sizeof(Binding));
    try { pending_ = new (storage) Binding(palette); }
    catch (...) { ChargedNativeMetadataRelease(storage); throw; }
    activeBinding = static_cast<Binding*>(pending_);
}
NativeTPLBinding::~NativeTPLBinding()
{
    auto* binding = static_cast<Binding*>(pending_);
    activeBinding = nullptr;
    binding->~Binding(); ChargedNativeMetadataRelease(binding);
}
void* NativeTPLBinding::Palette() const noexcept { return static_cast<Binding*>(pending_)->view.At(0); }
std::uint32_t NativeTPLBinding::RelocateDescriptors(std::uint32_t relative, std::uint32_t count)
{ return static_cast<Binding*>(pending_)->Descriptors(relative, count); }
std::uint32_t NativeTPLBinding::RelocateTexture(std::uint32_t relative)
{ return static_cast<Binding*>(pending_)->Texture(relative); }
std::uint32_t NativeTPLBinding::RelocatePixels(std::uint32_t relative, const void* header)
{ return static_cast<Binding*>(pending_)->Data(relative, header); }
std::uint32_t NativeTPLBinding::RelocateClut(std::uint32_t)
{ throw std::invalid_argument("TPL CLUT transport remains unqualified"); }
std::uint32_t NativeTPLBinding::RelocateClutPixels(std::uint32_t)
{ throw std::invalid_argument("TPL CLUT transport remains unqualified"); }
void NativeTPLBinding::Commit() { static_cast<Binding*>(pending_)->Commit(); }
}
