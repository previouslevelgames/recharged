#if !defined(MSCHARGED_NATIVE) || !defined(MSCHARGED_GAME_MODULE)
#error Trigger file transport belongs beneath the original game-module loader
#endif

#include "platform/trigger_file_transport.h"
#include "platform/game_allocation_ownership.h"

#include <bit>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <utility>

namespace mscharged::platform
{
namespace
{
constexpr std::size_t HeaderBytes = 12;
constexpr std::size_t AnimRecordBytes = 8;
constexpr std::size_t TriggerRecordBytes = 12;

std::uint16_t ReadBig16(const unsigned char* bytes) noexcept
{
    return std::uint16_t((std::uint16_t(bytes[0]) << 8) | bytes[1]);
}

std::uint16_t ReadNative16(const unsigned char* bytes) noexcept
{
    std::uint16_t value;
    std::memcpy(&value, bytes, sizeof(value));
    return value;
}

void Swap16(unsigned char* bytes, std::size_t count)
{
    if constexpr (std::endian::native == std::endian::little)
        for (std::size_t offset = 0; offset < count; offset += 2)
            std::swap(bytes[offset], bytes[offset + 1]);
}

void Swap32(unsigned char* bytes, std::size_t count)
{
    if constexpr (std::endian::native == std::endian::little)
        for (std::size_t offset = 0; offset < count; offset += 4)
        {
            std::swap(bytes[offset], bytes[offset + 3]);
            std::swap(bytes[offset + 1], bytes[offset + 2]);
        }
}

GameByteWriteReservation Reserve(void* address, std::size_t bytes)
{
    GameByteWriteReservation ticket(address, bytes);
    if (!ticket.Tracked())
        throw std::invalid_argument("Trigger file conversion lost its source allocation");
    return ticket;
}
}

void PrepareBinaryTriggerFile(void* data, unsigned long size)
{
    auto* bytes = static_cast<unsigned char*>(data);
    if (!bytes || size < HeaderBytes)
        throw std::invalid_argument("Trigger file is shorter than its original header");
    GameCompletedSpan completed{};
    if (!FindGameCompletedSpan(bytes, size, completed))
        throw std::invalid_argument("Trigger file is not a completed original load");
    if (std::memcmp(bytes, "NLBT", 4) != 0)
        throw std::invalid_argument("Trigger file thumbprint is not NLBT");

    const auto headerDomain = FindGameByteDomain(bytes + 4, HeaderBytes - 4);
    const bool native = headerDomain == GameByteDomain::NativeHeader;
    if (!native && headerDomain != GameByteDomain::WiiSerialized)
        throw std::invalid_argument("Trigger file header has an incompatible byte domain");
    auto read16 = native ? ReadNative16 : ReadBig16;
    const std::uint16_t version = read16(bytes + 4);
    const std::size_t animCount = read16(bytes + 6);
    const std::size_t bytecodeOffset = read16(bytes + 8);
    if (version != 1)
        throw std::invalid_argument("Trigger file version is not 1");
    const std::size_t firstTrigger = HeaderBytes + animCount * AnimRecordBytes;
    if (bytecodeOffset < firstTrigger || bytecodeOffset > size
        || (bytecodeOffset - firstTrigger) % TriggerRecordBytes != 0)
        throw std::invalid_argument("Trigger file record extents are invalid");
    const std::size_t triggerCount = (bytecodeOffset - firstTrigger) / TriggerRecordBytes;
    if (native)
    {
        if (bytecodeOffset > HeaderBytes
            && FindGameByteDomain(bytes + HeaderBytes, bytecodeOffset - HeaderBytes)
                != GameByteDomain::NativePayload)
            throw std::invalid_argument("Trigger file records are only partially converted");
        return;
    }
    if (bytecodeOffset > HeaderBytes
        && FindGameByteDomain(bytes + HeaderBytes, bytecodeOffset - HeaderBytes)
            != GameByteDomain::WiiSerialized)
        throw std::invalid_argument("Trigger file records have an incompatible byte domain");

    // Every anim record must address trigger records inside the record area.
    for (std::size_t i = 0; i < animCount; ++i)
    {
        const auto* record = bytes + HeaderBytes + i * AnimRecordBytes;
        if (std::size_t(ReadBig16(record + 6)) + ReadBig16(record + 4) > triggerCount)
            throw std::invalid_argument("Trigger file anim record leaves its trigger records");
    }

    // Reserve all metadata before changing a byte; the thumbprint and the
    // bytecode keep their serialized domain.
    auto header = Reserve(bytes + 4, HeaderBytes - 4);
    GameByteWriteReservation records;
    if (bytecodeOffset > HeaderBytes)
        records = Reserve(bytes + HeaderBytes, bytecodeOffset - HeaderBytes);
    Swap16(bytes + 4, HeaderBytes - 4);
    for (std::size_t i = 0; i < animCount; ++i)
    {
        auto* record = bytes + HeaderBytes + i * AnimRecordBytes;
        Swap32(record, 4);
        Swap16(record + 4, 4);
    }
    Swap32(bytes + firstTrigger, triggerCount * TriggerRecordBytes);
    if (records.Tracked()) records.Complete(GameByteDomain::NativePayload);
    header.Complete(GameByteDomain::NativeHeader);
}

void CopyBinaryTriggerBytecode(void* output, const void* source, std::size_t bytes)
{
    if (!bytes) return;
    if (FindGameByteDomain(source, bytes) != GameByteDomain::WiiSerialized)
        throw std::invalid_argument("Trigger bytecode copy requires completed serialized bytes");
    GameByteWriteReservation write(output, bytes);
    std::memcpy(output, source, bytes);
    write.Complete(GameByteDomain::WiiSerialized);
}
}
