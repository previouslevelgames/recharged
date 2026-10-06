#include "platform/rlg_record_abi.h"

#include <cstring>
#include <limits>
#include <new>
#include <stdexcept>

namespace mscharged::platform
{
namespace
{
std::uint16_t Half(const unsigned char* data)
{
    return std::uint16_t((std::uint16_t(data[0]) << 8) | data[1]);
}
template<class T> T* OffsetPointer(std::uint32_t word)
{
    return reinterpret_cast<T*>(std::uintptr_t(word));
}
}

std::uint32_t ReadRLGWord(const void* raw)
{
    const auto* data = static_cast<const unsigned char*>(raw);
    return (std::uint32_t(data[0]) << 24) | (std::uint32_t(data[1]) << 16)
        | (std::uint32_t(data[2]) << 8) | std::uint32_t(data[3]);
}

unsigned long RLGNativeRecordBytes(std::size_t rawBytes,
    std::size_t rawStride, std::size_t nativeStride)
{
    if (!rawStride || nativeStride < rawStride)
        throw std::invalid_argument("Invalid native RLG record geometry");
    const std::size_t count = rawBytes / rawStride;
    const std::size_t tail = rawBytes % rawStride;
    if (count > (std::numeric_limits<std::size_t>::max() - tail) / nativeStride)
        throw std::length_error("Native RLG record byte extent overflow");
    const std::size_t bytes = count * nativeStride + tail;
    // glResourceAlloc's actual size carrier remains unsigned long (LLP64 too).
    if (bytes > std::numeric_limits<unsigned long>::max())
        throw std::length_error("Native RLG bytes exceed the original allocation size carrier");
    return static_cast<unsigned long>(bytes);
}

void DecodeRLGModels(glModel* output, const void* raw, std::size_t count)
{
    const auto* data = static_cast<const unsigned char*>(raw);
    for (std::size_t i = 0; i < count; ++i, data += RLGModelBytes)
    {
        new (output + i) glModel;
        output[i].id = ReadRLGWord(data);
        output[i].numPackets = ReadRLGWord(data + 4);
        output[i].packets = OffsetPointer<glModelPacket>(ReadRLGWord(data + 8));
    }
}

void DecodeRLGStreams(glModelStream* output, const void* raw, std::size_t count)
{
    const auto* data = static_cast<const unsigned char*>(raw);
    for (std::size_t i = 0; i < count; ++i, data += RLGStreamBytes)
    {
        new (output + i) glModelStream;
        output[i].address = OffsetPointer<void>(ReadRLGWord(data));
        output[i].index = data[4];
        output[i].stride = data[5];
        output[i].id = data[6];
        output[i].unknown07 = data[7];
    }
}

void DecodeRLGPackets(glModelPacket* output, const void* raw, std::size_t rawBytes)
{
    const auto* data = static_cast<const unsigned char*>(raw);
    const std::size_t count = rawBytes / RLGPacketBytes;
    for (std::size_t i = 0; i < count; ++i, data += RLGPacketBytes)
    {
        new (output + i) glModelPacket;
        auto& packet = output[i];
        packet.indexBuffer = OffsetPointer<u16>(ReadRLGWord(data));
        packet.numVertices = ReadRLGWord(data + 4);
        packet.numUniqueVertices = Half(data + 8);
        // Copy the source byte representation, including high-bit signed char.
        std::memcpy(&packet.primType, data + 10, 1);
        packet.numStreams = data[11];
        packet.streams = OffsetPointer<glModelStream>(ReadRLGWord(data + 12));
        packet.materialProgram = OffsetPointer<void>(ReadRLGWord(data + 16));
        packet.unknown14 = ReadRLGWord(data + 20);
        packet.matrix = ReadRLGWord(data + 24);
        packet.rasterState = ReadRLGWord(data + 28);
        packet.materialParameters = OffsetPointer<void>(ReadRLGWord(data + 32));
        packet.displayList = OffsetPointer<DisplayList>(ReadRLGWord(data + 36));
        packet.skinnedVertices = ReadRLGWord(data + 40);
        packet.skinnedNormals = ReadRLGWord(data + 44);
    }
    // Original LoadPackets copies its complete raw byte count, unlike the
    // complete-record-only copies in LoadModels and LoadStreams.
    const std::size_t tail = rawBytes % RLGPacketBytes;
    if (tail)
        std::memcpy(reinterpret_cast<unsigned char*>(output) + count * sizeof(glModelPacket),
            data, tail);
}

std::uintptr_t RLGNativeStreamOffset(const void* encodedOffset)
{
    const std::uintptr_t offset = reinterpret_cast<std::uintptr_t>(encodedOffset);
    if (offset % RLGStreamBytes)
        throw std::invalid_argument("Raw RLG stream offset does not address a record");
    if (offset / RLGStreamBytes > std::numeric_limits<std::uintptr_t>::max() / sizeof(glModelStream))
        throw std::length_error("Native RLG stream offset overflow");
    return (offset / RLGStreamBytes) * sizeof(glModelStream);
}
}
