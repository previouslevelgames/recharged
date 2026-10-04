#pragma once
#include "resources/texture_bundle.h"
#include <map>
#include <memory>
#include <string>
#include <string_view>

namespace mscharged::resources
{
// NLOC strings retain UTF-16 code units; wchar_t is not a Wii string type.
struct Localization
{
    std::uint32_t language = 0, flags = 0;
    std::map<std::uint32_t, std::u16string> strings;
    const std::u16string& Get(std::uint32_t id) const;
};
std::shared_ptr<const Localization> ReadLocalization(Bytes bytes, std::uint32_t language);
std::string Utf16ToUtf8(std::u16string_view text);
std::uint32_t FrontendNameHash(std::string_view name);

struct FontGlyph
{
    std::uint16_t unicode = 0, font_char = 0, x = 0, y = 0;
    std::uint8_t page = 0, advance = 0, width = 0, height = 0, ascent = 0;
    std::int8_t offset = 0;
    bool has_kerning = false;
};
struct FrontendFont
{
    std::uint32_t alias = 0;
    std::uint16_t height = 0, ascent = 0, internal_leading = 0, page_size = 0;
    float spacing = 1, line_height = 1;
    std::map<std::uint16_t, FontGlyph> glyphs;
    std::map<std::uint32_t, int> kerning;
    std::vector<Texture> pages;
    const FontGlyph& Glyph(std::uint16_t unicode) const;
    // Original GetCharWidth arithmetic and font-index kerning keys. In
    // particular, extended Unicode kerning is not silently corrected here.
    std::uint32_t CharacterWidth(const FontGlyph& glyph, const FontGlyph* previous) const;
};
// Split decoding supports real original staged description/page reads. This
// descriptor owns its metrics but has no texture pages or graphics readiness.
struct FrontendFontDescription
{
    FrontendFont font;
    std::vector<std::uint32_t> page_hashes;
};
FrontendFontDescription ReadFrontendFontDescription(Bytes, std::string_view texture_base, std::string_view alias);
std::shared_ptr<const FrontendFont> AssembleFrontendFont(FrontendFontDescription, std::vector<Texture> pages);
// Checked sector bundle + original NLG 1.1 packing profile, colour pages.
// Font aliases are ASCII lowercase hashes as stored in FE resources; texture
// names keep their case-sensitive original hashes.
// Other descriptor profiles fail explicitly rather than approximating them.
std::shared_ptr<const FrontendFont> ReadFrontendFont(Bytes bytes, std::string_view texture_base,
                                                   std::string_view alias);
// Original FontManager first-match/first-registered fallback over explicit
// successful descriptor-registration order. Empty input returns no font.
std::shared_ptr<const FrontendFont> FindFrontendFont(
    std::span<const std::shared_ptr<const FrontendFont>> registration_order,
    std::uint32_t alias, bool allow_original_fallback = false);
struct FontQuad
{
    std::uint8_t page = 0;
    float left = 0, top = 0, right = 0, bottom = 0, u0 = 0, v0 = 0, u1 = 0, v1 = 0;
};
struct FontLayout
{
    std::shared_ptr<const FrontendFont> font;
    std::vector<FontQuad> quads;
    float width = 0, height = 0;
};
struct FontLineOptions
{
    // Original DrawString baseline coordinates; PixelCentre is zero on Wii.
    std::array<float, 2> position{0, 0};
    float pixel_centre = 0;
    int length = -1; // Font-character UTF-16 units; -1 draws the full line.
    bool flip_y = false;
    bool paragraphs = false; // Exact original {p} only, prevalidated before pointer parsing.
};
// Shared original measurement. Width and height intentionally use truncated
// character advances; drawing retains the original fractional forward kerning.
// Plain text only. Widths that cannot make bounded original progress fail.
std::uint32_t FrontendStringWidth(const FrontendFont&, std::u16string_view,
    bool single_line = true, std::uint32_t width = 65535, bool word_wrap = true);
std::uint32_t FrontendStringLineCount(const FrontendFont&, std::u16string_view,
    std::uint32_t width = 65535, bool word_wrap = true);
std::uint32_t FrontendStringHeight(const FrontendFont&, std::u16string_view,
    std::uint32_t width = 65535, bool word_wrap = true);
// Original plain colour-font quad progression and ascending page batches.
// Retains checked pages; this does not register an original graphics font.
FontLayout LayoutFrontendTextLine(std::shared_ptr<const FrontendFont>, std::u16string_view,
    const FontLineOptions& options = {});
// Bounded native baseline layout, using original metrics/packing/advances.
// Convenience newline adapter: pixels positive Y down, baseline at font ascent,
// rows Height*LineHeight. Each line uses original DrawString steps. Valid UTF-16
// is converted by code unit like FontCharString (unknown units become '?').
// Textbox wrapping, alignment, escape commands and FE animation are not selected.
FontLayout LayoutFrontendText(std::shared_ptr<const FrontendFont> font, std::u16string_view text);
}
