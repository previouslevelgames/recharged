#include "platform/localization_data.h"

#include "NL/nlLocalization.h"

#include <array>
#include <cstring>
#include <stdexcept>
#include <type_traits>

namespace mscharged {
namespace {

constexpr std::size_t WiiHeaderSize = 20;
constexpr std::size_t WiiLookupSize = 8;

static_assert(sizeof(LOCHeader) == WiiHeaderSize);
static_assert(offsetof(LOCHeader, Version) == 4);
static_assert(offsetof(LOCHeader, Language) == 8);
static_assert(offsetof(LOCHeader, StringCount) == 12);
static_assert(offsetof(LOCHeader, Flags) == 16);
static_assert(sizeof(nlLocalization::StringLookup) == WiiLookupSize);
static_assert(offsetof(nlLocalization::StringLookup, hash) == 0);
static_assert(offsetof(nlLocalization::StringLookup, StringOffset) == 4);
static_assert(sizeof(unsigned short) == 2);
static_assert(std::is_trivially_copyable_v<LOCHeader>);
static_assert(std::is_trivially_copyable_v<nlLocalization::StringLookup>);

std::uint32_t ReadWiiWord(const unsigned char* bytes)
{
    return (std::uint32_t(bytes[0]) << 24) |
           (std::uint32_t(bytes[1]) << 16) |
           (std::uint32_t(bytes[2]) << 8) | bytes[3];
}

void RequireHeader(const void* buffer, std::size_t size)
{
    if (buffer == nullptr || size < WiiHeaderSize)
        throw std::runtime_error("Wii localization transport requires a complete 20-byte header");
}

} // namespace

void PrepareWiiLocalizationHeader(void* buffer, std::size_t size)
{
    RequireHeader(buffer, size);
    const auto* bytes = static_cast<const unsigned char*>(buffer);
    LOCHeader header;
    std::memcpy(header.Thumbprint, bytes, sizeof(header.Thumbprint));
    header.Version = ReadWiiWord(bytes + 4);
    header.Language = ReadWiiWord(bytes + 8);
    header.StringCount = ReadWiiWord(bytes + 12);
    header.Flags = ReadWiiWord(bytes + 16);
    // memcpy begins the native trivial record lifetime in the same allocation.
    std::memcpy(buffer, &header, sizeof(header));
}

void PrepareWiiLocalizationTable(void* buffer, std::size_t size,
                                std::uint32_t string_count)
{
    RequireHeader(buffer, size);
    if (string_count > (size - WiiHeaderSize) / WiiLookupSize)
        throw std::runtime_error("Wii localization lookup table exceeds its supplied file span");
    const std::size_t first_string = WiiHeaderSize + std::size_t(string_count) * WiiLookupSize;
    if ((size - first_string) % sizeof(unsigned short) != 0)
        throw std::runtime_error("Wii localization UTF16 pool has an incomplete code unit");

    auto* bytes = static_cast<unsigned char*>(buffer);
    for (std::uint32_t i = 0; i < string_count; ++i)
    {
        auto* entry_bytes = bytes + WiiHeaderSize + std::size_t(i) * WiiLookupSize;
        nlLocalization::StringLookup entry;
        entry.hash = ReadWiiWord(entry_bytes);
        entry.StringOffset = ReadWiiWord(entry_bytes + 4);
        std::memcpy(entry_bytes, &entry, sizeof(entry));
    }
    for (std::size_t at = first_string; at < size; at += sizeof(unsigned short))
    {
        const unsigned short unit = (unsigned short)((unsigned(bytes[at]) << 8) | bytes[at + 1]);
        std::memcpy(bytes + at, &unit, sizeof(unit));
    }
}

} // namespace mscharged
