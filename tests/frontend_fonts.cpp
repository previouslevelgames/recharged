#include "frontend_font_fixture.h"
#include <fstream>
#include <iostream>
#include <iterator>

using namespace mscharged::resources;
using namespace font_fixture;
namespace
{
unsigned checks = 0;
void Check(bool good, const char* message) { ++checks; if (!good) throw std::runtime_error(message); }
template<class F> void Reject(F action)
{ ++checks; try { action(); } catch (const std::exception&) { return; } throw std::runtime_error("Invalid frontend visual resource accepted"); }
Blob Read(std::string name)
{ std::ifstream stream(name, std::ios::binary); Check(bool(stream), "Cannot read owned visual resource"); return {std::istreambuf_iterator<char>(stream), {}}; }
}
int main(int argc, char** argv)
{
    try
    {
        auto bytes = font_fixture::Localization(); auto loc = ReadLocalization(bytes, 0x7a947b29); bytes.clear();
        Check(loc->Get(1) == u"A\u00e9" && loc->Get(2) == u"B\U0001f600", "NLOC code-unit offsets or UTF-16 ownership changed");
        Check(Utf16ToUtf8(loc->Get(1)) == "A\xc3\xa9" && Utf16ToUtf8(loc->Get(2)) == "B\xf0\x9f\x98\x80", "UTF-16 conversion differs");
        Reject([&] { loc->Get(3); }); Reject([&] { ReadLocalization(font_fixture::Localization(), 0); });
        for (unsigned size = 0; size < 50; ++size)
        { auto bad = font_fixture::Localization(); bad.resize(size); Reject([&] { ReadLocalization(bad, 0x7a947b29); }); }
        for (auto [offset, value] : {std::pair{0u, 0u}, {4u, 2u}, {12u, 65537u}, {16u, 2u}, {28u, 1u}, {32u, 200u}})
        { auto bad = font_fixture::Localization(); Set(bad, offset, value); Reject([&] { ReadLocalization(bad, 0x7a947b29); }); }
        for (const std::u16string& bad : {std::u16string{0xd800}, std::u16string{0xdc00}, std::u16string{0xd800, u'a'}})
            Reject([&] { Utf16ToUtf8(bad); });
        auto font_bytes = Font(); auto font = ReadFrontendFont(font_bytes, "fe/fonts/fixture", "alias"); font_bytes.clear();
        Check(ReadFrontendFont(Font(), "fe/fonts/fixture", "ScRaTcHy36")->alias == 0xcd74f509,
            "Frontend font alias was not normalized to its resource hash");
        Check(font->glyphs.size() == 5 && font->pages.size() == 2 && font->kerning.size() == 2 && font->alias == FrontendNameHash("alias"), "Font inventory differs");
        Check(font->Glyph('A').x == 16 && font->Glyph('A').y == 0 && font->Glyph('B').x == 0 && font->Glyph('B').y == 16
            && font->Glyph(' ').page == 1 && font->Glyph(' ').x == 0 && font->Glyph(' ').y == 0, "Original packed rows/pages differ");
        Check(font->Glyph(233).font_char == 128 && font->Glyph(300).unicode == '?', "Extended font index or fallback differs");
        Check(font->CharacterWidth(font->Glyph('B'), &font->Glyph('A')) == 10
            && font->CharacterWidth(font->Glyph('A'), &font->Glyph(233)) == 9, "Original font-index kerning semantics differ");
        auto layout = LayoutFrontendText(font, u"AB\n\u00e9 "); font.reset();
        Check(layout.quads.size() == 4 && layout.width == 19 && layout.height == 30, "Font line metrics differ");
        Check(layout.quads[0].left == -1 && layout.quads[0].top == -2 && layout.quads[0].right == 14
            && layout.quads[0].u0 == .5f && layout.quads[0].u1 == 31.0f / 32, "Original packed UVs or native baseline differ");
        Check(layout.quads[2].top == 13 && layout.quads[3].page == 1, "Multiline baseline or page selection differs");
        Check(LayoutFrontendText(layout.font, u"\U0001f600").quads.size() == 1, "Supplementary character fallback was split");
        Check(LayoutFrontendText(layout.font, u"").quads.empty(), "Empty text generated glyphs");
        for (const std::u16string& bad : {std::u16string(u"{clr:ffffff}A"), std::u16string(u"A\tB"), std::u16string(4097, u'A'), std::u16string{0xd800}})
            Reject([&] { LayoutFrontendText(layout.font, bad); });
        const auto good = Font();
        for (auto size : {0u, 15u, 31u, 60u, 95u, 100u, 1600u, 3231u, unsigned(good.size() - 1)})
        { auto bad = good; bad.resize(size); Reject([&] { ReadFrontendFont(bad, "fe/fonts/fixture", "alias"); }); }
        for (auto [offset, value] : {std::pair{0u, 64u}, {4u, 34u}, {8u, 0xffffffffu}, {12u, 0u}, {36u, 0u}, {40u, 0xffffffffu}, {48u, 3u}, {60u, 3u}, {96u, 2u}, {100u, 9u}})
        { auto bad = good; Set(bad, offset, value); Reject([&] { ReadFrontendFont(bad, "fe/fonts/fixture", "alias"); }); }
        for (auto [before, after] : {std::pair{"Version 1.1", "Version 1.2"}, {"color", "splitfx"}, {"english", "inorder"},
            {"PageCount 2", "PageCount 1"}, {"Glyph A Width 10", "Glyph A Width 256"}, {"Kern A B -2 B -2", "Kern A B -2 B -3"},
            {"Glyph ?", "Glyph Z"}, {"Glyph B", "Glyph A"}, {"END", "UNKNOWN"}})
        { auto description = std::string(Descriptor()); description.replace(description.find(before), std::strlen(before), after);
            auto bad = Font("fe/fonts/fixture", description.c_str()); Reject([&] { ReadFrontendFont(bad, "fe/fonts/fixture", "alias"); }); }
        // Standalone texture and RLT inventory paths share the same decoder.
        const auto page = ReadTexture(Bytes(good).subspan(96, 1568), 42);
        Check(page.id == 42 && page.gx_format == 9 && page.palette_entries == 256 && page.pixels.size() == 1024, "Standalone CI8 texture decoder differs");
        if (argc == 2)
        {
            const std::string base = argv[1];
            for (auto [name, hash] : {std::pair{"english", 0x7a947b29u}, {"nafrench", 0x30d469c4u}, {"naspanish", 0x2f242024u}})
            {
                for (const char* suffix : {".loc", "_game.loc"})
                {
                    auto owned = ReadLocalization(Read(base + '/' + name + suffix), hash);
                    Check(!owned->strings.empty(), "Owned localization is empty");
                    for (const auto& [id, text] : owned->strings) { Utf16ToUtf8(text); ++checks; }
                    std::cout << name << suffix << ": " << owned->strings.size() << " strings\n";
                }
            }
            for (auto [name, count] : {std::pair{"eurfonttext18", 267u}, {"eurfontheading36", 117u}})
            {
                auto owned = ReadFrontendFont(Read(base + "/fonts/" + name + ".res"), std::string("fe/fonts/") + name, name);
                Check(owned->glyphs.size() == count, "Owned font glyph inventory changed");
                for (const auto& [unicode, glyph] : owned->glyphs)
                { if (unicode != '{') Check(LayoutFrontendText(owned, std::u16string(1, unicode)).quads.size() == 1, "Owned glyph failed layout"); }
                std::cout << name << ": " << owned->glyphs.size() << " glyphs, " << owned->pages.size() << " pages\n";
            }
        }
        std::cout << checks << " frontend font/localization checks passed\n";
    }
    catch (const std::exception& e) { std::cerr << "FAILED: " << e.what() << " (check " << checks << ")\n"; return 1; }
}
