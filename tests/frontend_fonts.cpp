#include "frontend_font_fixture.h"
#include <algorithm>
#include <bit>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>

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
void Bits(float actual,float expected)
{ Check(std::bit_cast<std::uint32_t>(actual)==std::bit_cast<std::uint32_t>(expected),"Shared original font float bits differ"); }
// Independent one-pass pen oracle, sorted afterward by page. Production uses
// original repeated string traversal per page and never calls this function.
void Oracle(const std::shared_ptr<const FrontendFont>& font,std::u16string_view text,
    const FontLineOptions& options={})
{
    std::vector<FontQuad> quads;
    float pen=options.position[0]+options.pixel_centre;
    const float y=options.position[1]+options.pixel_centre, inverse=1.f/font->page_size;
    const auto count=options.length<0?text.size():std::size_t(options.length);
    const auto index=[&](char16_t ch){return ch<0x80?ch:font->Glyph(ch).font_char;};
    std::uint32_t measured=0;
    for(std::size_t i=0;i<count;++i)
    {
        const auto& g=font->Glyph(text[i]);pen+=g.offset;
        const float top=y+(options.flip_y?int(g.ascent):-int(g.ascent));
        const float u=g.x*inverse,v=g.y*inverse;
        quads.push_back({g.page,pen,top,pen+float(g.width)-1.f,
            top+float(options.flip_y?-(int(g.height)-1):int(g.height)-1),
            u,v,u+(g.width-1)*inverse,v+(g.height-1)*inverse});
        int next=0;
        if(g.has_kerning&&i+1<text.size())
        {const auto found=font->kerning.find((std::uint32_t(index(text[i]))<<16)|index(text[i+1]));if(found!=font->kerning.end())next=found->second;}
        pen+=float(int(g.advance)+next)*font->spacing;
        int previous=0;
        if(i&&font->Glyph(text[i-1]).has_kerning)
        {const auto found=font->kerning.find((std::uint32_t(index(text[i-1]))<<16)|index(text[i]));if(found!=font->kerning.end())previous=found->second;}
        measured+=std::uint32_t(float(int(g.advance)+g.offset+previous)*font->spacing);
    }
    std::stable_sort(quads.begin(),quads.end(),[](auto&a,auto&b){return a.page<b.page;});
    const auto actual=LayoutFrontendTextLine(font,text,options);
    Check(actual.quads.size()==quads.size()&&actual.width==measured,"Original page count or measurement differs");
    for(unsigned i=0;i<quads.size();++i)
    {
        const auto& a=actual.quads[i];const auto& b=quads[i];Check(a.page==b.page,"Original ascending page order differs");
        Bits(a.left,b.left);Bits(a.top,b.top);Bits(a.right,b.right);Bits(a.bottom,b.bottom);
        Bits(a.u0,b.u0);Bits(a.v0,b.v0);Bits(a.u1,b.u1);Bits(a.v1,b.v1);
    }
}
void OriginalSteps(std::shared_ptr<const FrontendFont> font)
{
    for(float spacing:{.25f,1.f,1.3f,1.5f,2.75f})
    {
        auto changed=std::make_shared<FrontendFont>(*font);changed->spacing=spacing;
        for(bool flip:{false,true})
        {
            FontLineOptions options;options.flip_y=flip;options.position={17.125f,-12.5f};options.pixel_centre=.25f;
            for(const std::u16string text:{u"AB",u" A B ",u"\u00e9A B\u00e9",u"\u0100A",u""})
            {
                Oracle(changed,text,options);
                for(unsigned count=0;count<=text.size();++count){options.length=count;Oracle(changed,text,options);}options.length=-1;
            }
        }
    }
    auto changed=std::make_shared<FrontendFont>(*font);changed->spacing=1.5f;
    auto line=LayoutFrontendText(changed,u"AB");Bits(line.quads[1].left,11.f);Bits(line.width,28.f);
    changed->glyphs.at('A').page=1;changed->glyphs.at('B').page=0;
    line=LayoutFrontendTextLine(changed,u"AB");Check(line.quads[0].page==0&&line.quads[1].page==1,"Page batching was replaced with string ordering");
    auto missing=std::make_shared<FrontendFont>(*font);missing->kerning[0x0041005a]=-3;
    Oracle(missing,u"AZ");line=LayoutFrontendTextLine(missing,u"AZ");
    Bits(line.quads[1].left,7);Bits(line.width,15); // Z geometry falls back; its raw ASCII kern key survives.
    auto odd=std::make_shared<FrontendFont>(*font);odd->page_size=31;
    for(auto& page:odd->pages)page.width=page.height=31;
    for(auto& [unicode,glyph]:odd->glyphs){glyph.x=1;glyph.y=2;glyph.width=13;glyph.height=15;}
    Oracle(odd,u"BA\u00e9 A"); // Original UV origin-plus-extent rounding.
    changed=std::make_shared<FrontendFont>(*font);changed->spacing=1;
    changed->glyphs.at('A').advance=5;changed->glyphs.at('A').offset=0;changed->glyphs.at('A').has_kerning=false;
    Check(FrontendStringWidth(*changed,u"AAAA")==20,"Original single-line width differs");
    Check(FrontendStringWidth(*changed,u"AAAA",false,20,false)==0,"Original strict less-than width boundary changed");
    Check(FrontendStringWidth(*changed,u"AAAAA",false,20,false)==10,"Original overflow/revisit width changed");
    Check(FrontendStringLineCount(*changed,u"AAAAA",20,false)==2&&FrontendStringHeight(*changed,u"AAAAA",20,false)==24,"Original multiline metrics differ");
    Check(FrontendStringLineCount(*changed,u"")==1&&FrontendStringHeight(*changed,u"")==12,"Original empty-line metric changed");
    changed->spacing=1.25f;Check(FrontendStringHeight(*changed,u"")==15,"Original height stopped using character spacing");
    changed->spacing=1;
    Reject([&]{FrontendStringWidth(*changed,u"A",false,4);});
    Reject([&]{FrontendStringLineCount(*changed,u"AA",5,false);}); // Original loop has no progress at this width.
    Reject([&]{FrontendStringWidth(*changed,u"AA",false,5,false);});
    for(int length:{-2,3}){FontLineOptions o;o.length=length;Reject([&]{LayoutFrontendTextLine(font,u"AB",o);});}
    for(float value:{std::numeric_limits<float>::infinity(),std::numeric_limits<float>::quiet_NaN(),1e7f})
    {FontLineOptions o;o.position[0]=value;Reject([&]{LayoutFrontendTextLine(font,u"A",o);});}
    for(auto text:{u"{nbs}",u"A\nB",u"A\tB"})
    {Reject([&]{FrontendStringWidth(*font,text);});Reject([&]{LayoutFrontendTextLine(font,text);});}
    for(unsigned mode=0;mode<7;++mode)
    {
        auto bad=std::make_shared<FrontendFont>(*font);
        if(mode==0)bad->glyphs.at('A').font_char='B';
        if(mode==1)bad->glyphs.at('A').page=15;
        if(mode==2)bad->glyphs.at('A').width=0;
        if(mode==3)bad->spacing=std::numeric_limits<float>::quiet_NaN();
        if(mode==4)bad->glyphs.erase('?');
        if(mode==5)bad->kerning[0x00410042]=-1000;
        if(mode==6)bad->pages[0].width=31;
        Reject([&]{LayoutFrontendText(bad,u"AB");});
    }
}
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
        OriginalSteps(font);
        auto layout = LayoutFrontendText(font, u"AB\n\u00e9 "); font.reset();
        Check(layout.quads.size() == 4 && layout.width == 19 && layout.height == 30, "Font line metrics differ");
        Check(layout.quads[0].left == -1 && layout.quads[0].top == -2 && layout.quads[0].right == 14
            && layout.quads[0].u0 == .5f && layout.quads[0].u1 == 31.0f / 32, "Original packed UVs or native baseline differ");
        Check(layout.quads[2].top == 13 && layout.quads[3].page == 1, "Multiline baseline or page selection differs");
        Check(LayoutFrontendText(layout.font, u"\U0001f600").quads.size() == 2, "Original font-character UTF-16 fallback changed");
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
                std::u16string all;
                for(const auto& [unicode,glyph]:owned->glyphs)if(unicode!='{')all.push_back(unicode);
                Oracle(owned,all);std::reverse(all.begin(),all.end());Oracle(owned,all);
                for(bool flip:{false,true}){FontLineOptions options;options.flip_y=flip;options.position={12.5f,-23.25f};options.length=all.size()/2;Oracle(owned,all,options);}
                std::cout << name << ": " << owned->glyphs.size() << " glyphs, " << owned->pages.size() << " pages\n";
            }
        }
        std::cout << checks << " frontend font/localization checks passed\n";
    }
    catch (const std::exception& e) { std::cerr << "FAILED: " << e.what() << " (check " << checks << ")\n"; return 1; }
}
