#include "NL/MemAlloc.h"
#include "platform/arc_data_transport.h"
#include "platform/game_allocation_ownership.h"
#include "platform/native_hbm_font.h"
#include "platform/alarms.h"
#include "platform/interrupt_controller.h"
#include "platform/video_device.h"
#include "revolution/hbm/nw4hbm/ut/ResFont.h"
#include <aurora/aurora.h>
#include <aurora/video.h>
#include <dolphin/os.h>
#include <dolphin/vi.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <stdexcept>

namespace aurora { extern AuroraConfig g_config; }
extern void AuroraOSShutdownMemory();

namespace {
unsigned checks;
void Check(bool ok, const char* why) {
    ++checks;
    if (!ok) throw std::runtime_error(why);
}
template<class F> void Reject(F call) {
    ++checks;
    try { call(); } catch (const std::invalid_argument&) { return; }
    throw std::runtime_error("Invalid font representation was accepted");
}
void W(unsigned char* p, std::uint32_t v) { p[0]=v>>24; p[1]=v>>16; p[2]=v>>8; p[3]=v; }
void H(unsigned char* p, std::uint16_t v) { p[0]=v>>8; p[1]=v; }
constexpr std::size_t FontBytes=656;
std::array<unsigned char,FontBytes> Font() {
    std::array<unsigned char,FontBytes> a{};
    auto* p=a.data();
    W(p,0x52464e54); H(p+4,0xfeff); H(p+6,0x104); W(p+8,a.size()); H(p+12,16); H(p+14,4);
    W(p+16,0x46494e46); W(p+20,32);
    p[24]=1; p[25]=6; H(p+26,1); p[28]=1; p[29]=3; p[30]=4; p[31]=1;
    W(p+32,56); W(p+36,616); W(p+40,640); p[44]=4; p[45]=4; p[46]=3;
    // The original retail assertions require at least a 32x32, 512-byte sheet.
    W(p+48,0x54474c50); W(p+52,560);
    p[56]=4; p[57]=4; p[58]=3; p[59]=4; W(p+60,512); H(p+64,1); H(p+66,0);
    H(p+68,6); H(p+70,6); H(p+72,32); H(p+74,32); W(p+76,96);
    for (unsigned i=0;i<512;++i) p[96+i]=static_cast<unsigned char>(i*7);
    W(p+608,0x43574448); W(p+612,24); H(p+616,0); H(p+618,1);
    p[624]=1; p[625]=3; p[626]=4; p[627]=2; p[628]=4; p[629]=6;
    W(p+632,0x434d4150); W(p+636,24); H(p+640,'A'); H(p+642,'B'); H(p+644,0); H(p+652,0);
    return a;
}
std::array<unsigned char,800> Archive() {
    std::array<unsigned char,800> a{};
    auto* p=a.data();
    W(p,0x55aa382d); W(p+4,32); W(p+8,48); W(p+12,96);
    W(p+32,0x01000000); W(p+40,3);
    W(p+44,1); W(p+48,96); W(p+52,FontBytes);
    W(p+56,6); W(p+60,96+FontBytes); W(p+64,4);
    std::memcpy(p+68,"\0font\0tail\0",11);
    const auto font=Font(); std::memcpy(p+96,font.data(),font.size());
    return a;
}
}

int main() {
    using namespace mscharged::platform;
    using nw4hbm::ut::ResFont;
    try {
        // Original Warning uses the genuine OS clock and VI callback services.
        // This is a CPU hardware owner; no window, GX output or game readiness.
        aurora::g_config.mem1Size=MEM1_DEFAULT_SIZE;
        aurora::g_config.mem2Size=64u*1024u*1024u;
        OSInit();
        InitializeNativeInterruptController();
        InitializeNativeAlarms();
        ConfigureNativeVideoHardware(VI_TVMODE_NTSC_INT,false);
        VIInit();
        alignas(64) std::array<std::byte,32768> arena{};
        MemoryAllocator owner{}; owner.Initialize(arena.data(),arena.size());
        const auto initial=owner.TotalFreeMemory();
        const auto authored=Archive();
        std::uint64_t previous=0;
        for (unsigned iteration=0;iteration<2;++iteration) {
            auto* raw=static_cast<unsigned char*>(owner.Allocate(authored.size(),32,false));
            Check(raw!=nullptr,"Original allocation failed");
            auto* file=raw+96;
            NativeARCFileSpan span{};
            Check(!FindNativeARCFileSpan(file,span),"Uncompleted archive became readable");
            Reject([&]{NativeHBMFontHeader(file);});
            { GameByteWriteReservation write(raw,authored.size());
              std::memcpy(raw,authored.data(),authored.size()); write.Complete(GameByteDomain::WiiSerialized); }
            Check(FindNativeARCFileSpan(file,span) && span.base==file && span.bytes==FontBytes,
                "Borrowed font inherited archive tail instead of exact FST extent");
            Check(span.archive.allocation.incarnation!=previous,"Reload retained old source incarnation");
            previous=span.archive.allocation.incarnation;
            Check(!FindNativeARCFileSpan(file+1,span) && !span.base,"Interior file address was accepted");
            ResFont font;
            Check(font.SetResource(file) && font.IsManaging(file),"Original ResFont did not attach its raw identity");
            Check(font.GetWidth()==4 && font.GetHeight()==4 && font.GetAscent()==3
                && font.GetLineFeed()==6,"Original font metrics changed");
            Check(font.GetCharWidth('A')==4 && font.GetCharWidth('B')==6
                && font.GetCharWidth('?')==6,"Original map or alternate glyph lookup changed");
            nw4hbm::ut::Glyph glyph{}; font.GetGlyph(&glyph,'B');
            Check(glyph.pTexture==file+96 && glyph.texWidth==32 && glyph.texHeight==32
                && glyph.widths.charWidth==6,"Original glyph lost raw sheet or native metrics");
            // Original pointer/alignment assertions still precede the attachment guard.
            // This aligned live buffer passes those checks but is not a font resource.
            alignas(32) std::array<unsigned char,32> malformed{};
            Check(!font.SetResource(malformed.data()) && font.GetWidth()==4,
                "Native ingress moved ahead of the original already-attached guard");
            font.SetLineFeed(9);
            Check(font.GetLineFeed()==9 && std::memcmp(raw,authored.data(),authored.size())==0,
                "Original native metadata mutation overwrote the serialized owner");
            Check(font.RemoveResource()==file && font.IsManaging(nullptr),"Original detach lost the raw owner");
            Check(font.SetResource(file) && font.GetLineFeed()==9,
                "Original RFNU reattachment discarded native metadata mutations");
            Check(font.RemoveResource()==file,"Repeated detach failed");
            GameNativeBackingSpan backing{};
            Check(FindGameNativeBacking(file,FontBytes,backing),"Typed metadata was not owned by the original allocation");
            // A valid enclosing ARC must not permit a font to consume its neighbour.
            { GameByteWriteReservation write(raw,authored.size());
              std::memcpy(raw,authored.data(),authored.size()); W(file+8,FontBytes+32);
              write.Complete(GameByteDomain::WiiSerialized); }
            Check(!FindGameNativeBacking(file,FontBytes,backing),"Source overwrite retained old font backing");
            Reject([&]{font.SetResource(file);});
            Check(font.IsManaging(nullptr),"Rejected font gained a source borrower");
            // Ambiguous exact FST starts cannot identify an authoritative file size.
            { GameByteWriteReservation write(raw,authored.size());
              std::memcpy(raw,authored.data(),authored.size()); W(raw+60,96);
              write.Complete(GameByteDomain::WiiSerialized); }
            Check(!FindNativeARCFileSpan(file,span) && !span.base,"Ambiguous FST file start was accepted");
            owner.Free(raw);
            Check(!FindGameNativeBacking(file,FontBytes,backing) && !FindNativeARCFileSpan(file,span),
                "Original free retained a readable font/archive owner");
        }
        Check(owner.TotalFreeMemory()==initial,"Original allocator did not recover all storage");
        aurora_shutdown_video_hardware();
        ShutdownNativeAlarms();
        ShutdownNativeInterruptController();
        AuroraOSShutdownMemory();
        std::printf("Original HBM font/ARC: %u checks passed\n",checks);
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr,"HBM font check %u: %s\n",checks,e.what()); return 1;
    }
}
