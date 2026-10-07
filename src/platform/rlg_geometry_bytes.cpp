#include "platform/rlg_geometry_bytes.h"
#include "platform/game_allocation_ownership.h"
#include <cstdint>
#include <cstring>
#include <stdexcept>

namespace mscharged::platform
{
namespace
{
void RequireWiiCopy(const void* source, std::size_t bytes)
{
    if (FindGameByteDomain(source, bytes) != GameByteDomain::WiiSerialized)
        throw std::invalid_argument("RLG source copy requires completed owned Wii bytes");
}
}
void CopyRLGWireVertices(void* output, const void* source, std::size_t bytes)
{
    if (!bytes) return;
    RequireWiiCopy(source, bytes);
    GameByteWriteReservation write(output, bytes);
    std::memcpy(output, source, bytes);
    write.Complete(GameByteDomain::WiiSerialized);
}
void CopyRLGNativeIndices(void* output, const void* source, std::size_t bytes)
{
    if (!bytes) return;
    RequireWiiCopy(source, bytes);
    GameByteWriteReservation write(output, bytes);
    const auto* raw = static_cast<const unsigned char*>(source);
    auto* native = static_cast<unsigned char*>(output);
    for (std::size_t offset = 0; offset + 1 < bytes; offset += sizeof(std::uint16_t))
    {
        const auto value = std::uint16_t((std::uint16_t(raw[offset]) << 8) | raw[offset + 1]);
        std::memcpy(native + offset, &value, sizeof(value));
    }
    // Original memcpy copies an odd terminal byte too; do not reject, round or
    // manufacture an extra index cell when preserving that raw size quirk.
    if (bytes & 1) native[bytes - 1] = raw[bytes - 1];
    write.Complete(GameByteDomain::NativePayload);
}
void CopyRLGNativeMatrices(void* output, const void* source, std::size_t bytes)
{
    if (!bytes) return;
    if (bytes % 64)
        throw std::invalid_argument("RLG matrix copy requires complete original 64-byte records");
    RequireWiiCopy(source, bytes);
    GameByteWriteReservation write(output, bytes);
    if (!write.Tracked())
        throw std::invalid_argument("RLG matrix output has no genuine original owner");
    const auto* raw = static_cast<const unsigned char*>(source);
    auto* native = static_cast<unsigned char*>(output);
    for (std::size_t offset = 0; offset < bytes; offset += sizeof(std::uint32_t))
    {
        const std::uint32_t bits = (std::uint32_t(raw[offset]) << 24)
            | (std::uint32_t(raw[offset + 1]) << 16)
            | (std::uint32_t(raw[offset + 2]) << 8) | raw[offset + 3];
        // No float arithmetic: retain authored zero, infinity and NaN bits.
        std::memcpy(native + offset, &bits, sizeof(bits));
    }
    write.Complete(GameByteDomain::NativeHeader);
}

}
