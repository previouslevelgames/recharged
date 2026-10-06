#pragma once

#include <revolution/thp/THPFile.h>
#include <revolution/thp/THPInfo.h>

#include <cstddef>
#include <cstring>

namespace mscharged::platform {

// Original THP disk records contain big-endian Wii words, not host pointers.
// The original caller supplies complete records using its original NL reads.
inline u32 ReadTHPWord(const void* source) noexcept {
    const auto* bytes = static_cast<const u8*>(source);
    return (static_cast<u32>(bytes[0]) << 24) |
           (static_cast<u32>(bytes[1]) << 16) |
           (static_cast<u32>(bytes[2]) << 8) |
           static_cast<u32>(bytes[3]);
}

inline s32 ReadTHPSignedWord(const void* source) noexcept {
    const u32 word = ReadTHPWord(source);
    s32 value;
    std::memcpy(&value, &word, sizeof(value));
    return value;
}

// The original control record embeds the complete 48-byte THP header directly
// after its native file pointer. Preserve float bits without arithmetic.
inline void ExpandTHPHeader(void* destination, const void* source) noexcept {
    auto* output = static_cast<u8*>(destination);
    const auto* input = static_cast<const u8*>(source);
    std::memcpy(output, input, 4);
    for (std::size_t offset = 4; offset < sizeof(THPHeader); offset += 4) {
        const u32 value = ReadTHPWord(input + offset);
        std::memcpy(output + offset, &value, sizeof(value));
    }
}

inline void ExpandTHPComponents(THPFrameCompInfo& destination, const void* source) noexcept {
    const auto* bytes = static_cast<const u8*>(source);
    destination.numComponents = ReadTHPWord(bytes);
    std::memcpy(destination.frameComp, bytes + 4, sizeof(destination.frameComp));
}

inline void ExpandTHPVideo(THPVideoInfo& destination, const void* source) noexcept {
    const auto* bytes = static_cast<const u8*>(source);
    destination.xSize = ReadTHPWord(bytes);
    destination.ySize = ReadTHPWord(bytes + 4);
    destination.videoType = ReadTHPWord(bytes + 8);
}

inline void ExpandTHPAudio(THPAudioInfo& destination, const void* source) noexcept {
    const auto* bytes = static_cast<const u8*>(source);
    destination.sndChannels = ReadTHPWord(bytes);
    destination.sndFrequency = ReadTHPWord(bytes + 4);
    destination.sndNumSamples = ReadTHPWord(bytes + 8);
    destination.sndNumTracks = ReadTHPWord(bytes + 12);
}

static_assert(sizeof(THPHeader) == 48);
static_assert(sizeof(THPFrameCompInfo) == 20);
static_assert(sizeof(THPVideoInfo) == 12);
static_assert(sizeof(THPAudioInfo) == 16);
static_assert(sizeof(u32) == 4);
static_assert(sizeof(s32) == 4);

} // namespace mscharged::platform
