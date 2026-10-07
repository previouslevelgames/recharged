#if !defined(MSCHARGED_NATIVE) || !defined(MSCHARGED_GAME_MODULE)
#error ARC serialized transport belongs beneath the original game module
#endif

#include "platform/arc_data_transport.h"
#include "platform/game_allocation_ownership.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace
{
using namespace mscharged::platform;
static_assert(sizeof(int) == 4 && sizeof(unsigned int) == 4);
static_assert(std::numeric_limits<int>::min() == -2147483647 - 1);

GameCompletedSpan Completed(const void* pointer, std::size_t bytes)
{
    GameCompletedSpan span;
    if (!bytes || !FindGameCompletedSpan(pointer, bytes, span))
        throw std::invalid_argument("ARC bytes are outside completed original storage");
    return span;
}

void Serialized(const void* pointer, std::size_t bytes)
{
    Completed(pointer, bytes);
    if (FindGameByteDomain(pointer, bytes) != GameByteDomain::WiiSerialized)
        throw std::invalid_argument("ARC numeric bytes are not original Wii serialized data");
}

std::size_t Available(const GameCompletedSpan& span, const void* pointer)
{
    const auto base = reinterpret_cast<std::uintptr_t>(span.base);
    const auto address = reinterpret_cast<std::uintptr_t>(pointer);
    if (address < base || address - base > span.bytes)
        throw std::invalid_argument("ARC pointer exceeds its original logical publication");
    return span.bytes - (address - base);
}
}

extern "C" void* ChargedARCHeader(const void* archive, std::size_t alignment)
{
    Serialized(archive, 32);
    if (!alignment || reinterpret_cast<std::uintptr_t>(archive) % alignment)
        throw std::invalid_argument("ARC header is not aligned for its original native C fields");
    return const_cast<void*>(archive);
}

extern "C" std::uint32_t ChargedReadARCWord(const void* word)
{
    Serialized(word, 4);
    const auto* p = static_cast<const unsigned char*>(word);
    return (std::uint32_t(p[0]) << 24) | (std::uint32_t(p[1]) << 16)
        | (std::uint32_t(p[2]) << 8) | std::uint32_t(p[3]);
}

extern "C" void* ChargedARCByteOffset(const void* pointer, std::intptr_t offset)
{
    const auto span = Completed(pointer, 1);
    const auto base = reinterpret_cast<std::uintptr_t>(span.base);
    const auto address = reinterpret_cast<std::uintptr_t>(pointer);
    const auto available = Available(span, pointer);
    std::uintptr_t target;
    if (offset >= 0)
    {
        const auto forward = static_cast<std::uintptr_t>(offset);
        if (forward > available || address > std::numeric_limits<std::uintptr_t>::max() - forward)
            throw std::invalid_argument("ARC byte offset exceeds completed original storage");
        target = address + forward;
    }
    else
    {
        const auto backward = static_cast<std::uintptr_t>(-(offset + 1)) + 1;
        if (backward > address - base)
            throw std::invalid_argument("ARC byte offset precedes completed original storage");
        target = address - backward;
    }
    return reinterpret_cast<void*>(target);
}

extern "C" void ChargedValidateARCLayout(const void* archive,
    std::int32_t fstStart, std::int32_t fstBytes, std::int32_t fileStart,
    std::size_t fstAlignment)
{
    // Bounds describe the source's fixed32-byte header and12-byte records.
    // No path lookup, directory traversal, file selection or ownership changes.
    const auto span = Completed(archive, 32);
    Serialized(archive, 32);
    const auto available = Available(span, archive);
    if (fstStart < 0 || fstBytes < 12 || fileStart < 0
        || std::size_t(fstStart) > available || std::size_t(fstBytes) > available - std::size_t(fstStart)
        || std::size_t(fileStart) > available)
        throw std::invalid_argument("ARC declared byte ranges exceed their actual logical read");
    const auto* fst = static_cast<const unsigned char*>(ChargedARCByteOffset(archive, fstStart));
    if (!fstAlignment || reinterpret_cast<std::uintptr_t>(fst) % fstAlignment)
        throw std::invalid_argument("ARC records are not aligned for their original native C fields");
    const auto table = Completed(fst, fstBytes);
    if (table.allocation.incarnation != span.allocation.incarnation)
        throw std::invalid_argument("ARC table crosses its original allocation owner");
    Serialized(fst, fstBytes);
    const auto count = ChargedReadARCWord(fst + 8);
    if (count == 0 || count > std::size_t(fstBytes) / 12)
        throw std::invalid_argument("ARC entry count exceeds its original serialized table");
    const auto recordBytes = std::size_t(count) * 12;
    const auto stringBytes = std::size_t(fstBytes) - recordBytes;
    const auto* strings = fst + recordBytes;
    for (std::uint32_t i=0; i<count; ++i)
    {
        const auto* entry = fst + std::size_t(i) * 12;
        const auto typeAndName = ChargedReadARCWord(entry);
        const auto position = ChargedReadARCWord(entry + 4);
        const auto length = ChargedReadARCWord(entry + 8);
        const auto name = typeAndName & 0xFFFFFF;
        if (name >= stringBytes || !std::memchr(strings + name, 0, stringBytes - name))
            throw std::invalid_argument("ARC name exceeds its original bounded string table");
        if ((typeAndName & 0xFF000000) == 0)
        {
            if (position > available || length > available - position)
                throw std::invalid_argument("ARC file span exceeds its actual original logical read");
        }
        else if (position >= count || length > count)
            throw std::invalid_argument("ARC directory indices exceed its serialized record extent");
    }
}
