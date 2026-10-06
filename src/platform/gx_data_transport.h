#pragma once

#include "NL/nlColour.h"
#include <dolphin/gx/GXStruct.h>

namespace mscharged::platform {
// Source packed RGBA is a four-byte numeric Wii word, independent of host endian.
inline u32 ColourToWiiWord(const nlColour& colour)
{
    return (u32(colour.c[0]) << 24) | (u32(colour.c[1]) << 16)
        | (u32(colour.c[2]) << 8) | u32(colour.c[3]);
}
inline GXColor ColourFromWiiWord(u32 word)
{
    return {u8(word >> 24), u8(word >> 16), u8(word >> 8), u8(word)};
}
// Original GX texture creation/replacement retains raw Wii-ordered palette bytes.
inline u16 ReadWiiPaletteWord(const u16* palette)
{
    const u8* bytes = reinterpret_cast<const u8*>(palette);
    return u16((u16(bytes[0]) << 8) | bytes[1]);
}
} // namespace mscharged::platform
