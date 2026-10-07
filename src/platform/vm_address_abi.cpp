#include "platform/vm_address_abi.h"
#include "platform/game_allocation_ownership.h"
#include "Game/InterpreterCore.h"
#include <dolphin/os.h>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace mscharged::platform
{
std::uint32_t EncodeVMAddress(const void* address)
{
    if (!address) return 0;
    const auto physical = OSCachedToPhysical(const_cast<void*>(address));
    if (physical & 0xe0000000u)
        throw std::out_of_range("VM address has no Wii cached-word representation");
    return physical | 0x80000000u;
}
void* DecodeVMAddress(std::uint32_t word)
{
    if (!word) return nullptr;
    if ((word & 0xe0000000u) != 0x80000000u)
        throw std::out_of_range("VM address word is outside Wii cached memory");
    return OSPhysicalToCached(word & 0x1fffffffu);
}
std::uint32_t ReadVMNativeWord(const void* bytes)
{
    std::uint32_t value;
    std::memcpy(&value, bytes, sizeof(value));
    return value;
}
namespace
{
constexpr std::size_t HeaderBytes = 72;
struct Image
{
    unsigned char* bytes;
    std::size_t available;
    bool native;
    std::uint32_t fields[12];
    std::size_t functions, tweaks, globals, data, code, strings, end;
    std::uint32_t Word(std::size_t offset) const
    {
        if (offset > available || 4 > available - offset)
            throw std::out_of_range("VM serialized word leaves source completion");
        if (native) return ReadVMNativeWord(bytes + offset);
        const auto* p = bytes + offset;
        return std::uint32_t(p[0]) << 24 | std::uint32_t(p[1]) << 16
            | std::uint32_t(p[2]) << 8 | p[3];
    }
    std::uint16_t Half(std::size_t offset) const
    {
        if (offset > available || 2 > available - offset)
            throw std::out_of_range("VM serialized halfword leaves source completion");
        if (native)
        {
            std::uint16_t value;
            std::memcpy(&value, bytes + offset, sizeof(value));
            return value;
        }
        return std::uint16_t(bytes[offset]) << 8 | bytes[offset + 1];
    }
    std::size_t Add(std::size_t offset, std::size_t count) const
    {
        if (offset > available || count > available - offset)
            throw std::out_of_range("VM segment leaves source logical completion");
        return offset + count;
    }
    void PutWord(std::size_t offset, std::uint32_t value) const
    {
        std::memcpy(bytes + offset, &value, sizeof(value));
    }
    void PutHalf(std::size_t offset, std::uint16_t value) const
    {
        std::memcpy(bytes + offset, &value, sizeof(value));
    }
};
void ValidateTweaks(const Image& image, bool convert)
{
    std::size_t cursor = image.tweaks;
    const auto count = image.fields[10] - image.fields[7];
    for (std::uint32_t i = 0; i < count; ++i)
    {
        if (cursor == image.globals)
            throw std::out_of_range("VM tweak record leaves source segment");
        const auto flags = image.bytes[cursor++];
        const auto* end = static_cast<const unsigned char*>(std::memchr(
            image.bytes + cursor, 0, image.globals - cursor));
        if (!end) throw std::out_of_range("VM tweak name leaves source segment");
        cursor = std::size_t(end - image.bytes) + 1;
        for (unsigned bit = 0; bit != 4; ++bit)
            if (flags & (1u << bit))
            {
                if (cursor > image.globals || 4 > image.globals - cursor)
                    throw std::out_of_range("VM tweak value leaves source segment");
                if (convert) image.PutWord(cursor, image.Word(cursor));
                cursor += 4;
            }
    }
    // Authored alignment/padding remains untouched and part of its segment.
}
Image Describe(void* input)
{
    GameCompletedSpan source{};
    if (!FindGameCompletedSpan(input, HeaderBytes, source))
        throw std::invalid_argument("VM bytecode has no actual completed NL source span");
    const auto begin = reinterpret_cast<std::uintptr_t>(source.base);
    const auto address = reinterpret_cast<std::uintptr_t>(input);
    if (address < begin || address - begin > source.bytes)
        throw std::out_of_range("VM source pointer leaves logical completion");
    const auto domain = FindGameByteDomain(input, HeaderBytes);
    if (domain != GameByteDomain::WiiSerialized && domain != GameByteDomain::NativeHeader)
        throw std::invalid_argument("VM header has no serialized/native-header domain");
    Image image{static_cast<unsigned char*>(input), source.bytes - (address - begin),
                domain == GameByteDomain::NativeHeader, {}};
    for (unsigned i = 0; i != 12; ++i) image.fields[i] = image.Word(i * 4);
    if (image.fields[0] != 0xe11c2112u)
        throw std::invalid_argument("VM bytecode signature is not retail bytecode");
    if (image.fields[3] % 4 || image.fields[4] % 4 || image.fields[5] % 2
        || image.fields[7] > image.fields[3] / 4
        || image.fields[7] > image.fields[8] || image.fields[8] > image.fields[9]
        || image.fields[9] > image.fields[10] || image.fields[11] > image.fields[7])
        throw std::invalid_argument("VM source scalar/segment representation is invalid");
    image.functions = HeaderBytes;
    if (image.fields[1] > (image.available - HeaderBytes) / 12)
        throw std::out_of_range("VM function table leaves source completion");
    image.tweaks = image.Add(image.functions, std::size_t(image.fields[1]) * 12);
    image.globals = image.Add(image.tweaks, image.fields[2]);
    image.data = image.Add(image.globals, image.fields[3]);
    image.code = image.Add(image.data, image.fields[4]);
    image.strings = image.Add(image.code, image.fields[5]);
    image.end = image.Add(image.strings, image.fields[6]);
    if (image.globals % alignof(std::uint32_t) || image.data % alignof(std::uint32_t)
        || image.code % alignof(std::uint16_t)
        || address % alignof(ByteCodeHeader))
        throw std::invalid_argument("VM source segments require an unqualified alignment transport");
    if (image.end > HeaderBytes)
    {
        const auto bodyDomain = FindGameByteDomain(image.bytes + HeaderBytes, image.end - HeaderBytes);
        if (bodyDomain != (image.native ? GameByteDomain::NativePayload : GameByteDomain::WiiSerialized))
            throw std::invalid_argument("VM body is not uniformly completed in its actual domain");
    }
    // The original source writes these actual pointer fields only after the
    // serialized conversion. Its cached offsets remain32-bit address words.
    const auto codeWord = image.Word(64);
    const std::size_t segments[6] = {image.functions,image.tweaks,image.globals,
                                     image.data,image.code,image.strings};
    for (unsigned i = 0; i != 6; ++i)
    {
        const auto word = image.Word(48 + i * 4);
        // Original LoadByteCode tests only CodeSegment. When it is zero the
        // source overwrites all six slots, regardless of stale authored words.
        if (codeWord)
        {
            if (!image.native)
                throw std::invalid_argument("VM raw header has an authored code address");
            if (word != EncodeVMAddress(image.bytes + segments[i]))
                throw std::invalid_argument("VM relocated header leaves its actual source image");
        }
    }
    for (std::uint32_t i = 0; i < image.fields[1]; ++i)
    {
        const auto entry = image.functions + std::size_t(i) * 12;
        const auto offset = image.Word(entry + 4);
        std::size_t byteOffset;
        if (image.native && codeWord)
        {
            const auto pointer = reinterpret_cast<std::uintptr_t>(DecodeVMAddress(offset));
            const auto code = address + image.code;
            if (pointer < code || pointer - code >= image.fields[5])
                throw std::invalid_argument("VM cached function address leaves original code segment");
            byteOffset = pointer - code;
        }
        else byteOffset = offset;
        if (byteOffset % 2 || byteOffset >= image.fields[5])
            throw std::invalid_argument("VM function offset leaves its original code segment");
        if (i && image.Word(entry - 12) >= image.Word(entry))
            throw std::invalid_argument("VM source function hashes are not strictly sorted");
    }
    ValidateTweaks(image, false);
    // Verify every source pointer producer is represented before touching bytes.
    // No copied image or game-side owner is created by this transport.
    for (auto offset : segments) EncodeVMAddress(image.bytes + offset);
    if (image.end) EncodeVMAddress(image.bytes + image.end);
    return image;
}
}
void PrepareVMBytecode(void* input)
{
    const auto image = Describe(input);
    if (image.native) return;
    GameByteWriteReservation header(input, HeaderBytes);
    GameByteWriteReservation payload;
    if (image.end > HeaderBytes)
        payload = GameByteWriteReservation(image.bytes + HeaderBytes, image.end - HeaderBytes);
    if (!header.Tracked() || (image.end > HeaderBytes && !payload.Tracked()))
        throw std::logic_error("VM native conversion lost its original allocation owner");
    // All format/domain/address checks precede this original in-place byte seam.
    ValidateTweaks(image, true);
    for (std::uint32_t i = 0; i < image.fields[1]; ++i)
    {
        const auto entry = image.functions + std::size_t(i) * 12;
        image.PutWord(entry, image.Word(entry));
        image.PutWord(entry + 4, image.Word(entry + 4));
        image.PutHalf(entry + 8, image.Half(entry + 8));
    }
    for (unsigned segment = 0; segment != 2; ++segment)
    {
        const auto start = segment == 0 ? image.globals : image.data;
        const auto size = image.fields[3 + segment];
        for (std::size_t offset = start; offset < start + size; offset += 4)
            image.PutWord(offset, image.Word(offset));
    }
    for (std::size_t offset = image.code; offset < image.strings; offset += 2)
        image.PutHalf(offset, image.Half(offset));
    for (unsigned i = 0; i != 12; ++i) image.PutWord(i * 4, image.fields[i]);
    if (image.end > HeaderBytes) payload.Complete(GameByteDomain::NativePayload);
    header.Complete(GameByteDomain::NativeHeader);
}
}
