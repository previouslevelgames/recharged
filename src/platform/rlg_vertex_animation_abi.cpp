#include "platform/rlg_record_abi.h"
#include "platform/game_allocation_ownership.h"

#include <cstring>
#include <limits>
#include <stdexcept>

namespace mscharged::platform
{
void DecodeRLGVertexAnimWords(void* nativeWords, const void* raw, std::size_t count)
{
    if (!count) return; // Original zero-byte memcpy reads no source bytes.
    if (count > std::numeric_limits<std::size_t>::max() / sizeof(std::uint32_t))
        throw std::length_error("Vertex-animation word extent overflow");
    const auto bytes = count * sizeof(std::uint32_t);
    if (FindGameByteDomain(raw, bytes) != GameByteDomain::WiiSerialized)
        throw std::invalid_argument("Vertex-animation words require completed Wii bytes");
    const auto* source = static_cast<const unsigned char*>(raw);
    auto* output = static_cast<unsigned char*>(nativeWords);
    for (std::size_t i = 0; i < count; ++i)
    {
        const auto value = ReadRLGWord(source + i * sizeof(std::uint32_t));
        std::memcpy(output + i * sizeof(std::uint32_t), &value, sizeof(value));
    }
}

void ValidateRLGVertexAnimData(const void* raw, std::size_t rawBytes)
{
    if (rawBytes < 24 || FindGameByteDomain(raw, rawBytes) != GameByteDomain::WiiSerialized)
        throw std::invalid_argument("Vertex-animation chunk requires its completed Wii payload");
    const auto* data = static_cast<const unsigned char*>(raw);
    const std::size_t streams = ReadRLGWord(data + 20);
    if (streams > (rawBytes - 24) / sizeof(std::uint32_t))
        throw std::invalid_argument("Vertex-animation stream IDs leave the authored chunk");
    // Keep the original Wii u32 multiplication and overflow representation.
    const std::uint32_t vertices = ReadRLGWord(data + 4) * ReadRLGWord(data + 8)
                                * ReadRLGWord(data + 12);
    if (vertices > rawBytes - 24 - streams * sizeof(std::uint32_t))
        throw std::invalid_argument("Vertex-animation vertices leave the authored chunk");
}

}
