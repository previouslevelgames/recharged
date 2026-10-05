#include "NL/nlFont.h"
#include "NL/nlBundleFile.h"
#include "NL/nlFile.h"
#include "NL/nlFileGC.h"
#include "NL/MemAlloc.h"
#include "runtime/startup.h"
#include "runtime/startup_files.h"
#include <aurora/aurora.h>
#include <aurora/dvd.h>
#include <SDL3/SDL.h>
#include <array>
#include <bit>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace aurora { extern AuroraConfig g_config; }
void AuroraOSShutdown();
namespace
{
unsigned checks = 0;
void Check(bool value, const char* reason)
{ ++checks; if (!value) throw std::runtime_error(reason); }
struct Host
{
    bool dvd = false, files = false;
    std::vector<std::uint64_t> standard = std::vector<std::uint64_t>(4*1024*1024);
    std::vector<std::uint64_t> virtual_arena = std::vector<std::uint64_t>(8*1024*1024);
    explicit Host(const char* path)
    {
        Check(SDL_Init(0), "SDL host initialization failed");
        aurora::g_config.mem1Size = MEM1_DEFAULT_SIZE;
        aurora::g_config.mem2Size = 0;
        mscharged::InitializeStartupOS();
        Check(aurora_dvd_open(path), "Cannot mount actual Wii data partition"); dvd = true;
        StandardAllocator.Initialize(standard.data(), standard.size()*8);
        VirtualAllocator.Initialize(virtual_arena.data(), virtual_arena.size()*8);
        gMemoryInitialized = 1;
        nlInitFileSystem(); files = true;
    }
    ~Host()
    {
        if (files) mscharged::ResetStartupFiles();
        if (dvd) aurora_dvd_close();
        mscharged::ResetStartupMemory(); AuroraOSShutdown(); SDL_Quit();
    }
};
struct Memory
{
    unsigned standard = StandardAllocator.TotalFreeMemory();
    unsigned virtual_arena = VirtualAllocator.TotalFreeMemory();
    unsigned standard_largest = StandardAllocator.LargestFreeBlock();
    unsigned virtual_largest = VirtualAllocator.LargestFreeBlock();
    void Same() const
    {
        Check(StandardAllocator.TotalFreeMemory() == standard && VirtualAllocator.TotalFreeMemory() == virtual_arena,
            "Original font allocations did not recover their owning game arenas");
        Check(StandardAllocator.LargestFreeBlock() == standard_largest && VirtualAllocator.LargestFreeBlock() == virtual_largest,
            "Original font lifetime fragmented recovered game arenas");
    }
};
struct ReadCallback
{
    unsigned calls = 0;
    void* buffer = nullptr;
    unsigned long size = 0;
    std::thread::id thread = std::this_thread::get_id();
    static void Complete(void* bytes, unsigned long size, BundleAsyncParam context)
    {
        auto& self = *reinterpret_cast<ReadCallback*>(context);
        Check(context > UINT32_MAX, "Actual original font read lost a native pointer context");
        Check(std::this_thread::get_id() == self.thread, "Font read callback ran on DVD worker");
        Check(bytes == self.buffer && size == self.size, "Font callback buffer/read size differs");
        ++self.calls;
    }
};
void Await(ReadCallback& callback)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!callback.calls)
    {
        nlServiceFileSystem();
        Check(std::chrono::steady_clock::now() < deadline, "Actual original font read timed out");
        if (!callback.calls) SDL_Delay(1);
    }
    Check(callback.calls == 1, "Original font read callback count changed");
}
struct Glyph
{
    unsigned code, advance, width, height, ascent, page, kern;
    int offset;
    std::array<std::uint32_t,4> uv;
};
struct Pair { unsigned a, b; int kern; };
struct Query { std::vector<unsigned short> text; unsigned width, single, wrap, expected_width, lines, height; };
void Escapes()
{
    const std::pair<std::u16string,ESCAPE_TYPE> cases[] = {
        {u"{nbs}",ESC_NON_BREAKING_SPACE},{u"{p}",ESC_PARAGRAPH},
        {u"{{}",ESC_OPENBRACE},{u"{clr:ff0011}",ESC_COLOUR},
        {u"{four}",ESC_UNKNOWN}
    };
    for(const auto& [text,type]:cases)
    {
        const auto* begin=reinterpret_cast<const unsigned short*>(text.c_str());
        nlEscapeSequence sequence(begin);
        Check(sequence.GetType()==type && sequence.m_pEnd==begin+text.size(),
            "Original escape BE lookup/token endpoint changed");
        if(type==ESC_COLOUR)
            Check(std::memcmp(sequence.m_Extended,u"ff0011",7*sizeof(unsigned short))==0,
                "Original extended colour token copy changed");
    }
}
struct Oracle
{
    unsigned pages, page_size, texture_type, distribution, height, ascent, leading;
    std::uint32_t spacing, lineheight;
    std::vector<Glyph> glyphs;
    std::vector<Pair> pairs;
    std::vector<Query> queries;
    std::vector<std::uint32_t> pages_hash, effect_hash;
    explicit Oracle(const char* path)
    {
        std::ifstream in(path); unsigned count;
        in >> pages >> page_size >> texture_type >> distribution >> height >> ascent >> leading >> spacing >> lineheight;
        for (unsigned i=0;i<pages;++i) { std::uint32_t a,b; in >> a >> b; pages_hash.push_back(a);effect_hash.push_back(b); }
        in >> count;
        while (count--) { Glyph g;in >> g.code >> g.advance >> g.width >> g.height >> g.ascent >> g.offset >> g.page >> g.kern;
            for(auto& value:g.uv) in>>value;glyphs.push_back(g); }
        in >> count; while(count--){ Pair p;in>>p.a>>p.b>>p.kern;pairs.push_back(p); }
        in >> count; while(count--) { Query q;unsigned size;in >> q.width >> q.single >> q.wrap >> q.expected_width >> q.lines >> q.height >> size;
            while(size--){unsigned c;in>>c;q.text.push_back(c);}q.text.push_back(0);queries.push_back(q); }
        Check(bool(in), "Cannot read independent font descriptor/bit oracle");
    }
    void Font(const nlFont& font, std::uint32_t alias) const
    {
        Check(font.m_PageCount==pages && font.m_PageSize==page_size && font.m_TextureType==texture_type
            && font.m_Distribution==distribution, "Original font descriptor page/type fields differ");
        Check(font.m_Metrics.FontName==alias && font.m_Metrics.Height==height && font.m_Metrics.Ascent==ascent
            && font.m_Metrics.InternalLeading==leading, "Original font alias/metrics differ");
        Check(std::bit_cast<std::uint32_t>(font.m_Metrics.Spacing)==spacing
            && std::bit_cast<std::uint32_t>(font.m_Metrics.LineHeight)==lineheight, "Original font metric floats differ in bits");
        for(unsigned i=0;i<pages;++i){Check(font.m_TextureHandles[i]==pages_hash[i], "Original overlapping page name formatting changed texture request");
            if(texture_type==SplitFX)Check(font.m_EffectTextureHandles[i]==effect_hash[i], "Original effect texture request changed");}
        unsigned extended=0;
        for(const auto& expected:glyphs)
        {
            unsigned short c=expected.code;
            if(c>=0x7f){c=font.GetExtendedFontChar(c);++extended;Check(c>=0x80,"Original extended glyph lookup failed");}
            const auto& actual=font.GetGlyphInfo(c);
            if (!(actual.UnicodeChar==expected.code && actual.Advance==expected.advance && actual.RenderWidth==expected.width
                && actual.RenderHeight==expected.height && actual.RenderAscent==expected.ascent && actual.Offset==expected.offset
                && actual.Page==expected.page && actual.HasKernPairs==expected.kern))
                std::cerr << "Glyph " << expected.code << " actual " << actual.UnicodeChar << ' ' << unsigned(actual.Advance)
                    << ' ' << unsigned(actual.RenderWidth) << ' ' << unsigned(actual.RenderHeight) << ' ' << unsigned(actual.RenderAscent)
                    << ' ' << int(actual.Offset) << ' ' << unsigned(actual.Page) << ' ' << unsigned(actual.HasKernPairs)
                    << " expected " << expected.advance << ' ' << expected.width << ' ' << expected.height << ' ' << expected.ascent
                    << ' ' << expected.offset << ' ' << expected.page << ' ' << expected.kern << '\n';
            Check(actual.UnicodeChar==expected.code && actual.Advance==expected.advance && actual.RenderWidth==expected.width
                && actual.RenderHeight==expected.height && actual.RenderAscent==expected.ascent && actual.Offset==expected.offset
                && actual.Page==expected.page && actual.HasKernPairs==expected.kern, "Original glyph descriptor/packing differs");
            Check(std::array<std::uint32_t,4>{std::bit_cast<std::uint32_t>(actual.uv.x),std::bit_cast<std::uint32_t>(actual.uv.y),
                std::bit_cast<std::uint32_t>(actual.uvEnd.x),std::bit_cast<std::uint32_t>(actual.uvEnd.y)}==expected.uv,
                "Original packed/authored glyph UV differs in float bits");
        }
        Check(font.m_ExtendedGlyphCount==extended && font.m_KernTableSize==pairs.size(), "Original glyph/kerning table counts differ");
        for(unsigned i=0;i<pairs.size();++i)
        {
            const auto& pair=pairs[i];const auto& actual=font.m_pKernTable[i];
            Check(actual.s.A==pair.a && actual.s.B==pair.b && actual.Kern==pair.kern
                && nlFontWord(actual)==((pair.a<<16)|pair.b), "Original kerning sort/key/data differs from Wii BE oracle");
            if(pair.a<0x7f && pair.b<0x7f)
            {
                auto& glyph=font.GetGlyphInfo(pair.b);
                const auto expected=static_cast<std::uint32_t>(static_cast<float>(static_cast<std::uint32_t>(glyph.Advance+glyph.Offset+pair.kern))*font.m_Metrics.Spacing);
                Check(font.GetCharWidth(pair.b,pair.a)==expected,"Original native kerning binary search differs");
            }
        }
        Check(font.GetExtendedFontChar(0xffff)==0x3f,"Original missing extended glyph fallback changed");
        for(const auto& q:queries)
        {
            FontCharString text(q.text.data(),&font,(unsigned short*)0);
            Check(text.m_InternalBuffer==1 && reinterpret_cast<std::uintptr_t>(text.m_pString)>UINT32_MAX,
                "Original FontCharString ownership/pointer width changed");
            Check(font.GetStringWidth(text,q.single,q.width,q.wrap)==q.expected_width,"Original string width differs from independent equation");
            Check(font.GetStringLineCount(text,q.width,q.wrap)==q.lines,"Original line-count source quirk changed");
            Check(font.GetStringHeight(text,q.width,q.wrap)==q.height,"Original string-height source formula changed");
        }
        const unsigned short input[]={'A',0x1234,0};unsigned short storage[3]{};
        FontCharString external(input,&font,storage);
        Check(!external.m_InternalBuffer && external.m_pString==storage && storage[0]=='A' && storage[1]=='?' && storage[2]==0,
            "Original caller-owned text/remapping changed");
        FontCharString empty((const unsigned short*)u"",&font,(unsigned short*)0);
        Check(font.GetStringLineCount(empty,100,true)==1,"Original empty-string line-count quirk changed");
    }
};
void Execute(const char* disc,const char* bundle_path,const char* descriptor,const char* alias_name,const char* oracle_path)
{
    Host host(disc);Memory initial;Escapes();
    Oracle oracle(oracle_path);
    const auto alias=nlStringHash(alias_name);
    for(bool async:{false,true})
    {
        Memory before;
        {
            BundleFile bundle;Check(bundle.Open(bundle_path,false),"Original font bundle open failed");
            BundleFileDirectoryEntry record{};Check(bundle.GetFileInfo(descriptor,&record,true),"Original descriptor lookup failed");
            char* bytes=static_cast<char*>(nlMalloc(record.m_length,32,true));
            if(async)
            {
                ReadCallback callback;callback.buffer=bytes;callback.size=record.m_length;
                bundle.ReadFileAsync(descriptor,bytes,record.m_length,ReadCallback::Complete,reinterpret_cast<BundleAsyncParam>(&callback));
                Await(callback);
            }
            else bundle.ReadFileByIndex(bundle.FindHashIndex(BundleFile::HashFilename(descriptor),true),bytes,record.m_length);
            auto* font=new(8,false)nlFont;
            Check(font->Load(descriptor,bytes,alias)==1,"Original font Load result changed");
            Check(std::string(font->m_FontName)==descriptor,"Original descriptor name changed");
            // The caller switches arenas before all frees, as original async
            // completion can outlive the allocator that submitted the read.
            CurrentAllocator=&VirtualAllocator;nlFree(bytes);
            oracle.Font(*font,alias);delete font;
            CurrentAllocator=&StandardAllocator;
        }
        before.Same();
    }
    initial.Same();
}
}
int main(int argc,char** argv)
{
    try
    {
        if(argc!=6)throw std::runtime_error("Expected disc,bundle,descriptor,alias,oracle");
        Execute(argv[1],argv[2],argv[3],argv[4],argv[5]);
        std::cout<<"Original font parser/metrics/kerning: "<<checks<<" assertions passed\n";return 0;
    }
    catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
