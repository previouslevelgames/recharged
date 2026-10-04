#pragma once
#include "resources/frontend_fonts.h"
#include <cstring>

namespace font_fixture
{
using Blob = std::vector<std::uint8_t>;
inline void Set(Blob& b, std::size_t p, std::uint32_t value)
{ for (int shift : {24, 16, 8, 0}) b.at(p++) = value >> shift; }
inline Blob Localization(std::uint32_t language = 0x7a947b29)
{
    Blob bytes(52); Set(bytes, 0, 0x4e4c4f43); Set(bytes, 4, 1); Set(bytes, 8, language);
    Set(bytes, 12, 2); Set(bytes, 16, 1); Set(bytes, 20, 1); Set(bytes, 24, 0); Set(bytes, 28, 2); Set(bytes, 32, 3);
    for (auto [index, value] : {std::pair{18u, 0x41u}, {19u, 0xe9u}, {21u, 0x42u}, {22u, 0xd83du}, {23u, 0xde00u}})
    { bytes[index * 2] = value >> 8; bytes[index * 2 + 1] = value; }
    return bytes;
}
inline const char* Descriptor()
{
    return "NLG Font Description file\r\nVersion 1.1\r\n"
        "PageSize 32 PageCount 2 TexType color Distribution english\r\n"
        "Height 12 RenderHeight 16 Ascent 9 RenderAscent 11 IL 1\r\n"
        "CharSpacing 100 LineHeight 125\r\n"
        "Glyph ? Width 8 16 1\r\nGlyph A Width 10 16 -1\r\n"
        "Glyph B Width 12 16 0\r\nGlyph 233 Width 11 16 1\r\n"
        "Glyph 32 Width 7 16 0\r\nKern A B -2 B -2\r\nKern 233 A -1\r\nEND\r\n";
}
inline Blob Font(std::string_view base = "fe/fonts/fixture", const char* description = Descriptor())
{
    const unsigned count = 3, dir = 32, start = 96, texture_size = 32 + 1024 + 512;
    const auto description_offset = start + texture_size * 2;
    Blob bytes(description_offset + std::strlen(description)); Set(bytes, 0, 32); Set(bytes, 4, count); Set(bytes, 8, 1); Set(bytes, 12, 3);
    for (unsigned i = 0; i < 2; ++i)
    {
        const auto offset = start + texture_size * i;
        Set(bytes, dir + 12 * i, mscharged::resources::FrontendNameHash(std::string(base) + '_' + std::to_string(i + 1)));
        Set(bytes, dir + 12 * i + 4, offset / 32); Set(bytes, dir + 12 * i + 8, texture_size);
        Set(bytes, offset, 1); Set(bytes, offset + 4, 8); bytes[offset + 15] = bytes[offset + 17] = 32; Set(bytes, offset + 20, 256);
        // Opaque white atlas on page 0; opaque red on page 1. No retail art.
        std::fill_n(bytes.begin() + offset + 32, 1024, 1);
        bytes[offset + 32 + 1024 + 2] = i ? 0xfc : 0xff; bytes[offset + 32 + 1024 + 3] = i ? 0 : 0xff;
    }
    Set(bytes, 56, mscharged::resources::FrontendNameHash(base)); Set(bytes, 60, description_offset / 32);
    Set(bytes, 64, std::strlen(description));
    std::memcpy(bytes.data() + description_offset, description, std::strlen(description));
    return bytes;
}
}
