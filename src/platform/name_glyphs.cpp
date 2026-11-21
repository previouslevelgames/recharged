#include "platform/name_glyphs.h"

#include <mutex>
#include <set>

namespace mscharged::platform
{
namespace
{
std::mutex gMutex;
std::set<std::uint32_t> gGlyphs;
std::uint64_t gVersion = 0;
} // namespace

bool ShippedFontHas(std::uint32_t c)
{
    // Latin-1 and Latin Extended, Greek, Cyrillic, the general punctuation,
    // currency and letterlike symbols: the ranges the fonts are built with.
    return (c >= 0x20 && c <= 0x7E) || (c >= 0xA0 && c <= 0x24F) || (c >= 0x370 && c <= 0x3FF)
        || (c >= 0x400 && c <= 0x52F) || (c >= 0x2000 && c <= 0x206F) || (c >= 0x20A0 && c <= 0x20CF)
        || (c >= 0x2100 && c <= 0x214F);
}

void NoteNameGlyphs(std::string_view text)
{
    std::set<std::uint32_t> found;
    for (std::size_t i = 0; i < text.size();)
    {
        const auto byte = static_cast<unsigned char>(text[i]);
        if (byte < 0x80)
        {
            ++i;
            continue;
        }
        // A UTF-8 sequence of 2 or 3 bytes (the code points ImGui draws);
        // 4-byte ones (emoji) and broken bytes are passed over.
        std::uint32_t c = 0;
        std::size_t length = 0;
        if ((byte & 0xE0) == 0xC0) { c = byte & 0x1F; length = 2; }
        else if ((byte & 0xF0) == 0xE0) { c = byte & 0x0F; length = 3; }
        else { ++i; continue; }
        if (i + length > text.size()) break;
        bool valid = true;
        for (std::size_t n = 1; n < length; ++n)
        {
            const auto next = static_cast<unsigned char>(text[i + n]);
            if ((next & 0xC0) != 0x80) { valid = false; break; }
            c = (c << 6) | (next & 0x3F);
        }
        i += valid ? length : 1;
        if (!valid || c < 0xA0 || (c >= 0xD800 && c <= 0xDFFF) || ShippedFontHas(c)) continue;
        found.insert(c);
    }
    if (found.empty()) return;
    std::lock_guard<std::mutex> lock(gMutex);
    bool added = false;
    for (const auto c : found)
    {
        if (gGlyphs.size() >= kMaxNameGlyphs) break;
        added |= gGlyphs.insert(c).second;
    }
    if (added) ++gVersion;
}

std::uint64_t NameGlyphsVersion()
{
    std::lock_guard<std::mutex> lock(gMutex);
    return gVersion;
}

std::vector<std::uint32_t> NameGlyphs()
{
    std::lock_guard<std::mutex> lock(gMutex);
    return {gGlyphs.begin(), gGlyphs.end()};
}
} // namespace mscharged::platform
