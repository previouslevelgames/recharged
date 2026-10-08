#include "NL/MemAlloc.h"
#include "platform/alarms.h"
#include "platform/arc_data_transport.h"
#include "platform/game_allocation_ownership.h"
#include "platform/interrupt_controller.h"
#include "platform/native_hbm_layout.h"
#include "platform/video_device.h"
#include "revolution/hbm/nw4hbm/lyt/common.h"
#include "revolution/hbm/nw4hbm/ut/Color.h"
#include <aurora/aurora.h>
#include <aurora/video.h>
#include <dolphin/os.h>
#include <dolphin/vi.h>
#include <array>
#include <bit>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <stdexcept>

namespace aurora { extern AuroraConfig g_config; }
extern void AuroraOSShutdownMemory();

namespace {
using namespace mscharged::platform;
using namespace nw4hbm::lyt;
unsigned checks;
void Check(bool ok, const char* why) {
    ++checks;
    if (!ok) throw std::runtime_error(why);
}
template<class F> void Reject(F call) {
    ++checks;
    try { call(); } catch (const std::invalid_argument&) { return; }
    throw std::runtime_error("Invalid layout representation was accepted");
}
void W(unsigned char* p, std::uint32_t v) { p[0]=v>>24; p[1]=v>>16; p[2]=v>>8; p[3]=v; }
void H(unsigned char* p, std::uint16_t v) { p[0]=v>>8; p[1]=v; }
void F(unsigned char* p, float v) { W(p,std::bit_cast<std::uint32_t>(v)); }
constexpr std::size_t FileAt=96, FileBytes=412, ArchiveBytes=544;
using Bytes=std::array<unsigned char,ArchiveBytes>;
Bytes Archive() {
    Bytes a{}; auto* p=a.data();
    W(p,0x55aa382d); W(p+4,32); W(p+8,49); W(p+12,FileAt);
    W(p+32,0x01000000); W(p+40,3);
    W(p+44,1); W(p+48,FileAt); W(p+52,FileBytes);
    W(p+56,8); W(p+60,FileAt+FileBytes); W(p+64,4);
    std::memcpy(p+68,"\0layout\0tail\0",13);
    p+=FileAt;
    W(p,0x524c5954); H(p+4,0xfeff); H(p+6,8); W(p+8,FileBytes); H(p+12,16); H(p+14,8);
    W(p+16,0x6c797431); W(p+20,20); p[24]=1; F(p+28,640); F(p+32,480);
    W(p+36,0x74786c31); W(p+40,32); H(p+44,1); W(p+48,8); p[52]=0x19;
    std::memcpy(p+56,"texture",8);
    W(p+68,0x666e6c31); W(p+72,28); H(p+76,1); W(p+80,8); p[84]=0x27;
    std::memcpy(p+88,"font",5);
    W(p+96,0x6d617431); W(p+100,80); H(p+104,1); W(p+108,16);
    std::memcpy(p+112,"material",9); H(p+132,0xfff9); H(p+134,257);
    const unsigned char rgba[]={0x11,0x22,0x33,0x44}; std::memcpy(p+156,rgba,4); W(p+172,0);
    W(p+176,0x70696331); W(p+180,96); p[184]=3; p[185]=2; p[186]=0x7b;
    std::memcpy(p+188,"root-picture",13); F(p+212,-3.5f); F(p+224,-0.0f);
    F(p+236,1); F(p+240,2); F(p+244,640); F(p+248,480);
    W(p+252,0x11223344); W(p+256,0x000000ff); W(p+260,0xff000080); W(p+264,0xabcdef01); H(p+268,0);
    W(p+272,0x70617331); W(p+276,8);
    W(p+280,0x74787431); W(p+284,124); p[288]=1; p[290]=255;
    std::memcpy(p+292,"child-text",11); F(p+340,1); F(p+344,1);
    H(p+356,8); H(p+358,8); H(p+360,0); H(p+362,0); W(p+368,116);
    W(p+372,0x01020304); W(p+376,0x7f8081fe); F(p+380,12); F(p+384,14); F(p+388,-1); F(p+392,2);
    H(p+396,'A'); H(p+398,0x00e9); H(p+400,0x4e2d); H(p+402,0);
    W(p+404,0x70616531); W(p+408,8);
    return a;
}
void Publish(unsigned char* raw, const Bytes& a) {
    GameByteWriteReservation write(raw,a.size());
    std::memcpy(raw,a.data(),a.size()); write.Complete(GameByteDomain::WiiSerialized);
}
void ReadView(unsigned char* raw, const Bytes& a) {
    const auto* h=static_cast<const res::BinaryFileHeader*>(NativeHBMLayoutHeader(raw+FileAt));
    Check(detail::TestFileHeader(*h,0x524c5954) && !detail::TestFileHeader(*h,0x524c414e),"Original header predicate changed");
    Check(h->fileSize==FileBytes && h->headerSize==16 && h->dataBlocks==8,"Header widths/values changed");
    auto* layout=detail::ConvertOffsToPtr<res::Layout>(h,h->headerSize);
    Check(layout->layoutSize.width==640 && layout->layoutSize.height==480 && layout->originType==1,"Float/byte layout fields changed");
    auto* texture=detail::ConvertOffsToPtr<res::Texture>(h,48);
    Check(texture->nameStrOffset==8 && texture->type==0x19
        && !std::strcmp(detail::ConvertOffsToPtr<char>(texture,texture->nameStrOffset),"texture"),"Original table-relative offset changed");
    auto* material=detail::ConvertOffsToPtr<res::Material>(h,112);
    Check(material->tevCols[0].r==-7 && material->tevCols[0].g==257,"Signed GXColorS10 cells changed");
    Check(material->tevKCols[0].r==0x11 && material->tevKCols[0].g==0x22
        && material->tevKCols[0].b==0x33 && material->tevKCols[0].a==0x44,"Raw GXColor component order changed");
    auto* picture=detail::ConvertOffsToPtr<res::Picture>(h,176);
    Check(picture->translate.x==-3.5f && std::bit_cast<std::uint32_t>(picture->rotate.x)==0x80000000
        && picture->scale.y==2 && picture->flag==3 && picture->alpha==0x7b,"Pane float/control transport changed");
    Check(picture->vtxCols[0]==0x11223344 && picture->vtxCols[1]==0x000000ff,"Typed u32 color word changed");
    nw4hbm::ut::Color color(picture->vtxCols[0]);
    Check(color.r==0x11 && color.g==0x22 && color.b==0x33 && color.a==0x44,"Original nonwhite Color scalar ABI changed");
    auto* text=detail::ConvertOffsToPtr<res::TextBox>(h,280);
    auto* cells=detail::ConvertOffsToPtr<wchar_t>(text,text->textStrOffset);
    Check(sizeof(wchar_t)==2 && text->textBufBytes==8 && text->textStrBytes==8 && text->textStrOffset==116
        && cells[0]=='A' && cells[1]==0x00e9 && cells[2]==0x4e2d && cells[3]==0,"Original Wii16 text span changed");
    Check(text->textCols[0]==0x01020304 && text->textCols[1]==0x7f8081fe && text->charSpace==-1,"Text scalar transport changed");
    GameNativeBackingSourceSpan source{};
    Check(FindGameNativeBackingSource(h,FileBytes,source) && source.source==raw+FileAt
        && source.source_bytes==FileBytes && source.backing.allocation.base==raw,"Typed view lost exact raw owner");
    Check(NativeHBMLayoutHeader(raw+FileAt)==h && !std::memcmp(raw,a.data(),a.size()),"Repeated ingress moved view or mutated raw bytes");
}
}
int main() {
    try {
        // Actual CPU SDK/debug owners. No HOME singleton or Layout allocator.
        aurora::g_config.mem1Size=MEM1_DEFAULT_SIZE; aurora::g_config.mem2Size=64u*1024u*1024u;
        OSInit(); InitializeNativeInterruptController(); InitializeNativeAlarms();
        ConfigureNativeVideoHardware(VI_TVMODE_NTSC_INT,false); VIInit();
        alignas(64) std::array<std::byte,32768> arena{};
        MemoryAllocator owner{}; owner.Initialize(arena.data(),arena.size());
        auto initial=owner.TotalFreeMemory(); std::uint64_t prior=0;
        for (unsigned repeat=0;repeat<2;++repeat) {
            auto* raw=static_cast<unsigned char*>(owner.Allocate(ArchiveBytes,32,false)); Check(raw,"Original allocator failed");
            auto* file=raw+FileAt; NativeARCFileSpan span{}; GameNativeBackingSpan backing{};
            Check(!FindNativeARCFileSpan(file,span),"Capacity became completed ARC data"); Reject([&]{NativeHBMLayoutHeader(file);});
            auto a=Archive(); Publish(raw,a); Check(FindNativeARCFileSpan(file,span) && span.bytes==FileBytes,"File inherited neighbouring bytes");
            Check(span.archive.allocation.incarnation!=prior,"Freed/reloaded source incarnation persisted"); prior=span.archive.allocation.incarnation;
            ReadView(raw,a); const void* before=NativeHBMLayoutHeader(file); GameNativeBackingSourceSpan source{};
            Reject([&]{NativeHBMLayoutHeader(file+1);});
            Publish(raw,a); Check(!FindGameNativeBackingSource(before,16,source),"Overwrite kept retired view");
            for (unsigned bad=0;bad<3;++bad) {
                a=Archive(); auto* p=a.data()+FileAt;
                if (bad==0) W(p,0x524c414e); else H(p+(bad==1?4:6),bad==1?0xfffe:7);
                W(p+20,0xffffffff); Publish(raw,a);
                auto* h=static_cast<const res::BinaryFileHeader*>(NativeHBMLayoutHeader(file));
                Check(!detail::TestFileHeader(*h,0x524c5954),"Original rejected header was bypassed");
                Check(!std::memcmp(raw,a.data(),a.size()),"Rejected header modified source bytes");
            }
            for (unsigned bad=0;bad<7;++bad) {
                a=Archive(); auto* p=a.data()+FileAt;
                switch(bad) {
                case 0:W(p+8,FileBytes+4);break;
                case 1:W(p+20,0xffffffff);break;
                case 2:W(p+48,0xfffffffc);break;
                case 3:W(p+108,12);break;
                case 4:W(p+368,110);break;
                case 5:H(p+358,7);break;
                case 6:W(p+272,0x70616531);break;
                }
                Publish(raw,a); Reject([&]{NativeHBMLayoutHeader(file);});
                Check(!FindGameNativeBacking(file,FileBytes,backing),"Malformed plan published partial backing");
            }
            a=Archive(); W(a.data()+52,12); Publish(raw,a); Reject([&]{NativeHBMLayoutHeader(file);});
            auto* foreign=static_cast<unsigned char*>(owner.Allocate(FileBytes,32,false));
            {GameByteWriteReservation write(foreign,FileBytes);a=Archive();std::memcpy(foreign,a.data()+FileAt,FileBytes);write.Complete(GameByteDomain::WiiSerialized);}
            Reject([&]{NativeHBMLayoutHeader(foreign);}); owner.Free(foreign);
            Publish(raw,a); before=NativeHBMLayoutHeader(file); owner.Free(raw);
            Check(!FindGameNativeBackingSource(before,16,source) && !FindNativeARCFileSpan(file,span),"Free retained resource view");
            Reject([&]{NativeHBMLayoutHeader(file);});
        }
        Check(owner.TotalFreeMemory()==initial,"Source allocator storage did not recover");
        aurora_shutdown_video_hardware(); ShutdownNativeAlarms(); ShutdownNativeInterruptController(); AuroraOSShutdownMemory();
        std::printf("Original HBM BRLYT transport: %u checks passed\n",checks); return 0;
    } catch(const std::exception& e) {
        std::fprintf(stderr,"HBM layout check %u: %s\n",checks,e.what()); return 1;
    }
}
