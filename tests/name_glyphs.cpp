// The characters of names the shipped font lacks (platform/name_glyphs).
#include "platform/name_glyphs.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>

using namespace mscharged::platform;

namespace
{
int failures = 0;
void Check(bool ok, const char* what)
{
    if (!ok)
    {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}
bool Has(std::uint32_t c)
{
    const auto glyphs = NameGlyphs();
    return std::find(glyphs.begin(), glyphs.end(), c) != glyphs.end();
}
} // namespace

int main()
{
    Check(NameGlyphsVersion() == 0 && NameGlyphs().empty(), "nothing noted at first");
    // ASCII, Latin, Greek, Cyrillic and ™ are the shipped font's.
    NoteNameGlyphs("Mario \xC5\x81\xC3\xA9 \xCE\xA9 \xD0\x96 \xE2\x84\xA2 {\"name\":\"x\"}");
    Check(NameGlyphsVersion() == 0 && NameGlyphs().empty(), "the shipped font's characters are not noted");
    // ★, マリオ, 마리오: noted once each, the version grows once.
    NoteNameGlyphs("\xE2\x98\x85 \xE3\x83\x9E\xE3\x83\xAA\xE3\x82\xAA \xEB\xA7\x88\xEB\xA6\xAC\xEC\x98\xA4 \xE2\x98\x85");
    Check(NameGlyphsVersion() == 1, "one new version for the new characters");
    Check(NameGlyphs().size() == 7, "seven characters");
    Check(Has(0x2605) && Has(0x30DE) && Has(0xB9C8), "star, katakana MA, Hangul MA");
    NoteNameGlyphs("\xE3\x83\x9E\xE2\x98\x85");
    Check(NameGlyphsVersion() == 1, "known characters change nothing");
    // A 4-byte character (emoji) and broken bytes are passed over.
    NoteNameGlyphs("\xF0\x9F\x98\x80 \xE3\x83 \xFF abc");
    Check(NameGlyphsVersion() == 1 && NameGlyphs().size() == 7, "emoji and broken bytes are passed over");
    Check(ShippedFontHas('A') && ShippedFontHas(0x0141) && !ShippedFontHas(0x2605), "ShippedFontHas");
    if (failures) return EXIT_FAILURE;
    std::puts("name glyphs: ok");
    return EXIT_SUCCESS;
}
