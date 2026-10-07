#include "platform/native_tpl.h"
#include "platform/game_allocation_ownership.h"
#include "platform/host_metadata.h"
#include <dolphin/os.h>

#include <array>
#include <cstring>
#include <limits>
#include <new>
#include <stdexcept>
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
enum class RecordKind { Palette, Descriptors, Texture };
struct Record
{
    std::size_t offset;
    std::size_t bytes;
    RecordKind kind;
    std::array<std::uint32_t, 10> words;
};
struct Descriptor
{
    std::uint32_t texture;
    std::uint32_t clut;
};
struct Pixels
{
    std::size_t offset;
    std::size_t bytes;
};
class Image
{
public:
    explicit Image(void* pointer) : data_(static_cast<unsigned char*>(pointer))
    {
        GameCompletedSpan source{};
        if (!FindGameCompletedSpan(pointer, 12, source))
            throw std::invalid_argument("TPL requires actual completed allocation-owned NL bytes");
        const auto sourceAddress = reinterpret_cast<std::uintptr_t>(source.base);
        const auto address = reinterpret_cast<std::uintptr_t>(pointer);
        if (address < sourceAddress || address - sourceAddress > source.bytes)
            throw std::out_of_range("TPL palette leaves its original completed source span");
        bytes_ = source.bytes - (address - sourceAddress);
        if (!FindGameCompletedSpan(pointer, bytes_, source)
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
    std::uint8_t Byte(std::size_t offset) const { Range(offset, 1); return data_[offset]; }
    std::uint32_t Address(std::size_t offset, std::size_t bytes) const
    {
        Range(offset, bytes);
        const auto physical = OSCachedToPhysical(data_ + offset);
        if (physical & 0xe0000000u)
            throw std::out_of_range("TPL pointer has no Wii cached-word representation");
        return physical | 0x80000000u;
    }
    void PutWord(std::size_t offset, std::uint32_t value) const { std::memcpy(data_ + offset, &value, 4); }
    void PutHalf(std::size_t offset, std::uint16_t value) const { std::memcpy(data_ + offset, &value, 2); }
    void PutByte(std::size_t offset, std::uint8_t value) const { data_[offset] = value; }
    void* At(std::size_t offset) const { return data_ + offset; }
private:
    unsigned char* data_;
    std::size_t bytes_;
};
bool Overlap(std::size_t a, std::size_t as, std::size_t b, std::size_t bs)
{
    return as && bs && a < b + bs && b < a + as;
}
}

void* DecodeNativeTPLAddress(std::uint32_t word, std::size_t bytes,
                             std::size_t alignment, bool header)
{
    if (!word) return nullptr;
    if ((word & 0xe0000000u) != 0x80000000u)
        throw std::out_of_range("TPL address cell is outside Wii cached memory");
    auto* pointer = OSPhysicalToCached(word & 0x1fffffffu);
    GameCompletedSpan source{};
    if (!bytes || !alignment || reinterpret_cast<std::uintptr_t>(pointer) % alignment
        || !FindGameCompletedSpan(pointer, bytes, source))
        throw std::out_of_range("TPL cached pointer has no live completed source backing");
    if (header && FindGameByteDomain(pointer, bytes) != GameByteDomain::NativeHeader)
        throw std::invalid_argument("TPL header pointer has no qualified native record domain");
    if (!header && FindGameByteDomain(pointer, bytes) != GameByteDomain::WiiSerialized)
        throw std::invalid_argument("TPL pixel pointer no longer refers to original serialized bytes");
    return pointer;
}

void BindNativeTPLImage(void* palette)
{
    Image image(palette);
    const auto version = image.Word(0), count = image.Word(4), table = image.Word(8);
    if (version != 0x0020af30)
        throw std::invalid_argument("TPL palette has no original retail version");
    // The retail binding loop has a16-bit index. Empty/rebind/overflow and other
    // texture formats remain explicit unqualified inputs in this native seam.
    if (!count || count > 0xffffu || table % 4)
        throw std::invalid_argument("TPL descriptor count/alignment remains unqualified");
    image.Range(table, std::size_t(count) * 8);
    MetadataVector<Record> records;
    MetadataVector<Descriptor> descriptors;
    MetadataVector<Pixels> pixels;
    records.push_back({0, 12, RecordKind::Palette, {version, count, image.Address(table, std::size_t(count) * 8)}});
    records.push_back({table, std::size_t(count) * 8, RecordKind::Descriptors, {}});
    descriptors.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i)
    {
        const auto offset = table + std::size_t(i) * 8;
        const auto texture = image.Word(offset), clut = image.Word(offset + 4);
        if (clut)
            throw std::invalid_argument("TPL CLUT transport remains unqualified");
        if (!texture) { descriptors.push_back({0, 0}); continue; }
        if (texture % 4)
            throw std::invalid_argument("TPL texture header requires original word alignment");
        image.Range(texture, 36);
        descriptors.push_back({image.Address(texture, 36), 0});
        bool known = false;
        for (const auto& record : records)
            if (record.kind == RecordKind::Texture && record.offset == texture) known = true;
        if (known) continue;
        const auto height = image.Half(texture), width = image.Half(texture + 2);
        const auto format = image.Word(texture + 4), data = image.Word(texture + 8);
        if (format != 5 || !height || !width || image.Byte(texture + 33)
            || image.Byte(texture + 34) || image.Byte(texture + 35))
            throw std::invalid_argument("TPL native binding currently requires original RGB5A3 nonmip unpacked0 data");
        const auto bytes = ((std::size_t(width) + 3) & ~std::size_t(3))
            * ((std::size_t(height) + 3) & ~std::size_t(3)) * 2;
        image.Range(data, bytes);
        pixels.push_back({data, bytes});
        records.push_back({texture, 36, RecordKind::Texture,
            {height, width, format, image.Address(data, bytes), image.Word(texture + 12),
             image.Word(texture + 16), image.Word(texture + 20), image.Word(texture + 24),
             image.Word(texture + 28)}});
    }
    for (std::size_t i = 0; i < records.size(); ++i)
    {
        for (std::size_t j = 0; j < i; ++j)
            if (Overlap(records[i].offset, records[i].bytes, records[j].offset, records[j].bytes))
                throw std::invalid_argument("TPL overlapping structural records remain unqualified");
        for (const auto& pixel : pixels)
            if (Overlap(records[i].offset, records[i].bytes, pixel.offset, pixel.bytes))
                throw std::invalid_argument("TPL pixels overlapping converted headers remain unqualified");
    }
    MetadataVector<GameByteWriteReservation> reservations;
    reservations.reserve(records.size());
    // Validate every original relative pointer and reserve every native-domain
    // publication before any raw byte is changed. No expanded game owner exists.
    for (const auto& record : records)
    {
        reservations.emplace_back(image.At(record.offset), record.bytes);
        if (!reservations.back().Tracked())
            throw std::logic_error("TPL conversion lost its actual source allocation");
    }
    for (const auto& record : records)
    {
        const auto at = record.offset;
        if (record.kind == RecordKind::Palette)
        {
            image.PutWord(at, record.words[0]);
            image.PutWord(at + 4, record.words[1]);
            image.PutWord(at + 8, record.words[2]);
        }
        else if (record.kind == RecordKind::Descriptors)
        {
            for (std::size_t i = 0; i < descriptors.size(); ++i)
            {
                image.PutWord(at + i * 8, descriptors[i].texture);
                image.PutWord(at + i * 8 + 4, descriptors[i].clut);
            }
        }
        else
        {
            image.PutHalf(at, static_cast<std::uint16_t>(record.words[0]));
            image.PutHalf(at + 2, static_cast<std::uint16_t>(record.words[1]));
            for (std::size_t i = 2; i < 9; ++i) image.PutWord(at + i * 4 - 4, record.words[i]);
            image.PutByte(at + 35, 1); // Original TPLBind marks actual data relocation.
        }
    }
    for (auto& reservation : reservations) reservation.Complete(GameByteDomain::NativeHeader);
}
}

extern "C" void TPLBind(void* palette)
{
    mscharged::platform::BindNativeTPLImage(palette);
}
