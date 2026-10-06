#pragma once

#include "NL/gl/glModel.h"
#include <cstddef>
#include <cstdint>

namespace mscharged::platform
{
inline constexpr std::size_t RLGModelBytes = 12;
inline constexpr std::size_t RLGStreamBytes = 8;
inline constexpr std::size_t RLGPacketBytes = 48;

// Raw input remains Wii ordered. These functions transport record values only;
// source RLGReader still owns allocation, traversal, registration and selection.
std::uint32_t ReadRLGWord(const void* data);
unsigned long RLGNativeRecordBytes(std::size_t rawBytes,
    std::size_t rawStride, std::size_t nativeStride);
void DecodeRLGModels(glModel* output, const void* raw, std::size_t count);
void DecodeRLGStreams(glModelStream* output, const void* raw, std::size_t count);
void DecodeRLGPackets(glModelPacket* output, const void* raw, std::size_t rawBytes);

// Raw record pointer fields temporarily carry the unchanged serialized offset.
// Only the original Fixup traversal rebases them into native storage. A stream
// offset addresses raw8-byte records and must map to actual native record width.
std::uintptr_t RLGNativeStreamOffset(const void* encodedOffset);
}
