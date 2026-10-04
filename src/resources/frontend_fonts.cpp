#include "resources/frontend_fonts.h"
#include <algorithm>
#include <charconv>
#include <set>
#include <sstream>

namespace mscharged::resources
{
namespace
{
std::uint32_t Scalar(std::u16string_view text, std::size_t& i)
{
    const auto first = text[i++];
    if (first >= 0xd800 && first <= 0xdbff)
    {
        Require(i < text.size() && text[i] >= 0xdc00 && text[i] <= 0xdfff, "Invalid UTF-16 surrogate pair");
        return 0x10000 + (std::uint32_t(first - 0xd800) << 10) + (text[i++] - 0xdc00);
    }
    Require(first < 0xdc00 || first > 0xdfff, "Unpaired UTF-16 low surrogate");
    return first;
}
int Number(std::string_view token, int low, int high)
{
    int value;
    const auto parsed = std::from_chars(token.data(), token.data() + token.size(), value);
    Require(parsed.ec == std::errc{} && parsed.ptr == token.data() + token.size() && value >= low && value <= high,
            "Invalid NLG integer field");
    return value;
}
std::uint16_t Character(const std::string& token)
{
    // Original parser treats a single token byte (including '0'..'9') literally.
    const auto value = token.size() == 1 ? static_cast<unsigned char>(token[0]) : Number(token, 32, 65535);
    Require(value >= 32 && value != 127 && (value < 0xd800 || value > 0xdfff), "Invalid NLG glyph character");
    return value;
}
std::map<std::uint32_t, Bytes> Bundle(Bytes bytes)
{
    Require(bytes.size() <= MaximumAssetBytes, "Font bundle exceeds its size limit");
    const auto sector = U32(bytes, 0), count = U32(bytes, 4);
    Require(sector == 32 && count && count <= 33, "Unsupported font bundle directory");
    const auto directory_offset = std::uint64_t(U32(bytes, 8)) * sector;
    const auto data_offset = std::uint64_t(U32(bytes, 12)) * sector;
    Require(directory_offset >= 16 && directory_offset <= bytes.size() && data_offset <= bytes.size()
        && data_offset >= directory_offset + std::uint64_t(count) * 12, "Invalid font bundle directory range");
    auto directory = Slice(bytes, directory_offset, count * 12);
    std::map<std::uint32_t, Bytes> result;
    std::vector<std::pair<std::size_t, std::size_t>> ranges;
    for (unsigned i = 0; i < count; ++i)
    {
        const auto offset = std::uint64_t(U32(directory, i * 12 + 4)) * sector;
        const auto size = U32(directory, i * 12 + 8);
        Require(offset >= data_offset && offset <= bytes.size() && size, "Invalid font bundle entry range");
        auto entry = Slice(bytes, offset, size);
        Require(result.emplace(U32(directory, i * 12), entry).second, "Duplicate font bundle entry");
        ranges.emplace_back(offset, offset + size);
    }
    std::sort(ranges.begin(), ranges.end());
    for (unsigned i = 1; i < ranges.size(); ++i)
        Require(ranges[i].first >= ranges[i - 1].second, "Overlapping font bundle entries");
    return result;
}
}
std::uint32_t FrontendNameHash(std::string_view name)
{
    std::uint32_t hash = 0xffffffff;
    for (unsigned char c : name) hash = hash * 33 + c;
    return hash;
}
const std::u16string& Localization::Get(std::uint32_t id) const
{
    const auto entry = strings.find(id);
    if (entry == strings.end()) throw std::out_of_range("Localization ID is absent");
    return entry->second;
}
std::string Utf16ToUtf8(std::u16string_view text)
{
    Require(text.size() <= MaximumAssetBytes / 2, "UTF-16 text exceeds its size limit");
    std::string out;
    for (std::size_t i = 0; i < text.size();)
    {
        const auto scalar = Scalar(text, i);
        if (scalar < 0x80) out.push_back(char(scalar));
        else if (scalar < 0x800)
        { out.push_back(char(0xc0 | (scalar >> 6))); out.push_back(char(0x80 | (scalar & 63))); }
        else if (scalar < 0x10000)
        { out.push_back(char(0xe0 | (scalar >> 12))); out.push_back(char(0x80 | ((scalar >> 6) & 63))); out.push_back(char(0x80 | (scalar & 63))); }
        else
        { out.push_back(char(0xf0 | (scalar >> 18))); out.push_back(char(0x80 | ((scalar >> 12) & 63)));
          out.push_back(char(0x80 | ((scalar >> 6) & 63))); out.push_back(char(0x80 | (scalar & 63))); }
    }
    return out;
}
std::shared_ptr<const Localization> ReadLocalization(Bytes bytes, std::uint32_t language)
{
    Require(bytes.size() <= MaximumAssetBytes && bytes.size() % 2 == 0, "Invalid localization size");
    Require(U32(bytes, 0) == 0x4e4c4f43 && U32(bytes, 4) == 1, "Invalid NLOC header");
    Require(U32(bytes, 8) == language, "Localization language does not match its request");
    const auto count = U32(bytes, 12);
    Require(count && count <= 65536, "Invalid localization string count");
    const auto table = Slice(bytes, 20, std::size_t(count) * 8);
    const auto strings = bytes.subspan(20 + table.size());
    auto result = std::make_shared<Localization>();
    result->language = language; result->flags = U32(bytes, 16);
    Require(result->flags <= 1, "Unsupported localization flags");
    std::size_t copied = 0;
    for (unsigned i = 0; i < count; ++i)
    {
        const auto hash = U32(table, i * 8), offset = U32(table, i * 8 + 4);
        Require(i == 0 || U32(table, (i - 1) * 8) < hash, "Localization lookup is not strictly sorted");
        Require(offset < strings.size() / 2, "Localization string offset is out of range");
        std::u16string text;
        for (std::size_t p = std::size_t(offset) * 2;; p += 2)
        {
            const auto unit = U16(strings, p);
            if (!unit) break;
            Require((copied += 2) <= MaximumAssetBytes, "Localization decoded string budget exceeded");
            text.push_back(char16_t(unit));
        }
        // Validate without changing the retained UTF-16 representation.
        for (std::size_t p = 0; p < text.size();) Scalar(text, p);
        result->strings.emplace(hash, std::move(text));
    }
    return result;
}
const FontGlyph& FrontendFont::Glyph(std::uint16_t unicode) const
{
    auto found = glyphs.find(unicode);
    if (found == glyphs.end()) found = glyphs.find('?');
    if (found == glyphs.end()) throw std::runtime_error("Font has no fallback glyph");
    return found->second;
}
std::uint32_t FrontendFont::CharacterWidth(const FontGlyph& glyph, const FontGlyph* previous) const
{
    std::int64_t width = int(glyph.advance) + int(glyph.offset);
    if (previous && previous->has_kerning)
    {
        const auto kern = kerning.find((std::uint32_t(previous->font_char) << 16) | glyph.font_char);
        if (kern != kerning.end()) width += kern->second;
    }
    const float scaled = float(width) * spacing;
    Require(width >= 0 && std::isfinite(scaled) && scaled >= 0 && scaled < 65536,
            "Font advance cannot use the bounded native text profile");
    return std::uint32_t(scaled);
}
std::shared_ptr<const FrontendFont> ReadFrontendFont(Bytes bytes, std::string_view texture_base, std::string_view alias)
{
    Require(!texture_base.empty() && texture_base.size() < 240 && !alias.empty() && alias.size() < 256,
            "Invalid font name");
    const auto entries = Bundle(bytes);
    const auto description = entries.find(FrontendNameHash(texture_base));
    Require(description != entries.end(), "Font description is absent from its bundle");
    Require(description->second.size() <= 1024 * 1024, "Font descriptor exceeds its size limit");
    std::string text(description->second.begin(), description->second.end());
    Require(text.find('\0') == std::string::npos, "Embedded NUL in font descriptor");
    std::istringstream stream(text); std::string line;
    // FE font resources reference lowercase aliases (including "scratchy36"),
    // although FontLoading passes "Scratchy36". Texture keys remain unchanged.
    std::string normalized_alias(alias);
    for (char& c : normalized_alias) if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
    auto font = std::make_shared<FrontendFont>(); font->alias = FrontendNameHash(normalized_alias);
    bool version = false, pages = false, metrics = false, spacing = false, end = false, kern_started = false;
    unsigned page_count = 0, page = 0, x = 0, y = 0, render_height = 0, render_ascent = 0;
    while (std::getline(stream, line))
    {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        Require(line.size() <= 16384, "Font descriptor line exceeds its size limit");
        std::istringstream words(line); std::vector<std::string> tokens;
        for (std::string token; words >> token;) tokens.push_back(std::move(token));
        if (tokens.empty()) continue;
        const auto& key = tokens[0];
        if (key == "END") { Require(tokens.size() == 1, "Invalid font descriptor terminator"); end = true; break; }
        if (key == "NLG" || key == "Font" || key == "Set" || key == "QEffect") continue; // Baked artwork metadata.
        if (key == "Version")
        {
            Require(!version && tokens.size() == 2, "Duplicate or malformed font version");
            if (tokens[1] != "1.1") throw UnsupportedResource("Only the original NLG 1.1 packed glyph profile is selected");
            version = true;
        }
        else if (key == "PageSize")
        {
            Require(!pages && tokens.size() == 8 && tokens[2] == "PageCount" && tokens[4] == "TexType"
                && tokens[6] == "Distribution", "Invalid NLG page metadata");
            if (tokens[5] != "color" || tokens[7] != "english") throw UnsupportedResource("Unsupported NLG texture or distribution profile");
            font->page_size = Number(tokens[1], 1, 1024); page_count = Number(tokens[3], 1, 16); pages = true;
        }
        else if (key == "Height")
        {
            Require(!metrics && tokens.size() == 10 && tokens[2] == "RenderHeight" && tokens[4] == "Ascent"
                && tokens[6] == "RenderAscent" && tokens[8] == "IL", "Invalid NLG height metadata");
            font->height = Number(tokens[1], 1, 1024); render_height = Number(tokens[3], 1, 255);
            font->ascent = Number(tokens[5], 0, 1024); render_ascent = Number(tokens[7], 0, 255);
            font->internal_leading = Number(tokens[9], 0, 1024); metrics = true;
        }
        else if (key == "CharSpacing")
        {
            Require(!spacing && tokens.size() == 4 && tokens[2] == "LineHeight", "Invalid NLG spacing metadata");
            font->spacing = Number(tokens[1], 1, 1000) / 100.0f;
            font->line_height = Number(tokens[3], 1, 1000) / 100.0f; spacing = true;
        }
        else if (key == "PageBreak") { Require(tokens.size() == 1 && pages, "Invalid NLG page break"); ++page; }
        else if (key == "Glyph")
        {
            Require(version && pages && metrics && spacing && !kern_started && tokens.size() == 6 && tokens[2] == "Width",
                    "Invalid NLG glyph or metadata ordering");
            FontGlyph glyph; glyph.unicode = Character(tokens[1]); glyph.advance = Number(tokens[3], 0, 255);
            glyph.width = Number(tokens[4], 1, 255); glyph.offset = Number(tokens[5], -128, 127);
            if (x + glyph.width > font->page_size)
            { x = 0; y += render_height; if (y + render_height > font->page_size) { x = y = 0; ++page; } }
            Require(page < page_count && x + glyph.width <= font->page_size && y + render_height <= font->page_size,
                    "Glyph exceeds its texture page");
            glyph.x = x; glyph.y = y; glyph.page = page; glyph.height = render_height; glyph.ascent = render_ascent;
            Require(font->glyphs.size() < 4096 && font->glyphs.emplace(glyph.unicode, glyph).second, "Duplicate or excessive NLG glyphs");
            x += glyph.width;
        }
        else if (key == "Kern")
        {
            kern_started = true;
            Require(tokens.size() >= 4 && tokens.size() % 2 == 0, "Invalid NLG kerning row");
            const auto first = Character(tokens[1]);
            Require(font->glyphs.count(first), "Kerning refers to an absent base glyph"); font->glyphs.at(first).has_kerning = true;
            for (unsigned i = 2; i < tokens.size(); i += 2)
            {
                Require(font->kerning.size() < 65536, "Excessive NLG kerning pairs");
                const auto value = Number(tokens[i + 1], -255, 255);
                const auto [entry, inserted] = font->kerning.emplace((std::uint32_t(first) << 16) | Character(tokens[i]), value);
                // eurfonttext18 repeats identical pairs. Conflicting duplicate
                // values have no deterministic original bsearch meaning.
                Require(inserted || entry->second == value, "Conflicting NLG kerning pair");
            }
        }
        else throw UnsupportedResource("Unknown NLG descriptor directive: " + key);
    }
    Require(end && version && pages && metrics && spacing && font->glyphs.count('?'), "Incomplete font descriptor");
    for (char c; stream.get(c);) Require(c == '\r' || c == '\n' || c == ' ' || c == '\t', "Data after font descriptor terminator");
    unsigned extended = 0x80;
    for (auto& [unicode, glyph] : font->glyphs) glyph.font_char = unicode < 0x80 ? unicode : extended++;
    Require(entries.size() == page_count + 1, "Font bundle has unexplained records");
    for (unsigned i = 0; i < page_count; ++i)
    {
        const auto id = FrontendNameHash(std::string(texture_base) + '_' + std::to_string(i + 1));
        const auto entry = entries.find(id); Require(entry != entries.end(), "Font texture page is missing");
        auto texture = ReadTexture(entry->second, id);
        Require(texture.width == font->page_size && texture.height == font->page_size && texture.levels == 1,
                "Font texture dimensions disagree with its metrics");
        font->pages.push_back(std::move(texture));
    }
    return font;
}
FontLayout LayoutFrontendText(std::shared_ptr<const FrontendFont> font, std::u16string_view text)
{
    Require(bool(font) && text.size() <= 4096 && font->height && font->page_size, "Invalid or excessive font layout");
    FontLayout result; result.font = std::move(font); const auto& face = *result.font;
    float x = 0, baseline = face.ascent; const FontGlyph* previous = nullptr;
    const float row = face.height * face.line_height;
    Require(std::isfinite(row) && row > 0 && row < 65536, "Invalid font line height");
    result.height = text.empty() ? 0 : row;
    for (std::size_t i = 0; i < text.size();)
    {
        const auto scalar = Scalar(text, i);
        if (scalar == '\n') { x = 0; baseline += row; result.height += row; previous = nullptr; continue; }
        if (scalar < 32 || scalar == 127 || scalar == '{')
            throw UnsupportedResource("Control characters and original FE text escape sequences are not selected");
        const auto& glyph = face.Glyph(scalar > 0xffff ? '?' : scalar);
        const float advance = face.CharacterWidth(glyph, previous);
        Require(glyph.page < face.pages.size() && glyph.width && glyph.height && glyph.x + glyph.width <= face.page_size
            && glyph.y + glyph.height <= face.page_size, "Invalid font glyph layout");
        const float left = x + glyph.offset, top = baseline - glyph.ascent;
        const float inverse = 1.0f / face.page_size;
        result.quads.push_back({glyph.page, left, top, left + glyph.width - 1, top + glyph.height - 1,
            glyph.x * inverse, glyph.y * inverse, (glyph.x + glyph.width - 1) * inverse, (glyph.y + glyph.height - 1) * inverse});
        x += advance; result.width = std::max(result.width, x); previous = &glyph;
    }
    return result;
}
}
