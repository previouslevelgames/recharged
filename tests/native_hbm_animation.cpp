#include "NL/MemAlloc.h"
#include "platform/alarms.h"
#include "platform/arc_data_transport.h"
#include "platform/game_allocation_ownership.h"
#include "platform/interrupt_controller.h"
#include "platform/native_hbm_animation.h"
#include "platform/video_device.h"
#include "revolution/hbm/nw4hbm/lyt/common.h"
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
float ChargedTestHBMHermite(float, const nw4hbm::lyt::res::HermiteKey*, unsigned);
unsigned short ChargedTestHBMStep(float, const nw4hbm::lyt::res::StepKey*, unsigned);

namespace {
using namespace mscharged::platform;
using namespace nw4hbm::lyt;
unsigned checks;
void Check(bool ok, const char* why)
{
    ++checks;
    if (!ok) throw std::runtime_error(why);
}
template<class F> void Reject(F call)
{
    ++checks;
    try { call(); } catch (const std::invalid_argument&) { return; }
    throw std::runtime_error("Invalid animation representation was accepted");
}
void Word(unsigned char* p, std::uint32_t v) { p[0]=v>>24; p[1]=v>>16; p[2]=v>>8; p[3]=v; }
void Half(unsigned char* p, std::uint16_t v) { p[0]=v>>8; p[1]=v; }
void Float(unsigned char* p, float v) { Word(p,std::bit_cast<std::uint32_t>(v)); }
template<class T> void Native(unsigned char* p, T v) { std::memcpy(p,&v,sizeof(v)); }
constexpr std::size_t FileBytes=128, FileAt=96, ArchiveBytes=256;
using Bytes=std::array<unsigned char,ArchiveBytes>;

Bytes Archive(std::uint32_t channel=0x524c5041, unsigned char contentType=0)
{
    Bytes a{}; auto* p=a.data();
    Word(p,0x55aa382d); Word(p+4,32); Word(p+8,48); Word(p+12,96);
    Word(p+32,0x01000000); Word(p+40,3);
    Word(p+44,1); Word(p+48,FileAt); Word(p+52,FileBytes);
    Word(p+56,6); Word(p+60,FileAt+FileBytes); Word(p+64,4);
    std::memcpy(p+68,"\0anim\0tail\0",11);
    p+=FileAt;
    Word(p,0x524c414e); Half(p+4,0xfeff); Half(p+6,8);
    Word(p+8,FileBytes); Half(p+12,16); Half(p+14,1);
    Word(p+16,0x70616931); Word(p+20,112);
    Half(p+24,10); p[26]=1; p[27]=0xa7; Half(p+28,0); Half(p+30,1); Word(p+32,20);
    Word(p+36,24); // block + 24 is content at 40
    std::memcpy(p+40,"independent-curve",18); p[60]=1; p[61]=contentType; p[62]=0xb2; p[63]=0xc3;
    Word(p+64,28); // content + 28 is info at 68
    Word(p+68,channel); p[72]=1; p[73]=0xd4; p[74]=0xe5; p[75]=0xf6;
    Word(p+76,12); // info + 12 is target at 80
    p[80]=2; p[81]=1; p[82]=2; p[83]=0x89; Half(p+84,3); p[86]=0xab; p[87]=0xcd;
    Word(p+88,12); // target + 12 is first key at 92
    Float(p+92,0); Float(p+96,2); Float(p+100,1);
    Float(p+104,4); Float(p+108,6); Float(p+112,1);
    Float(p+116,10); Float(p+120,0); Float(p+124,-0.0f);
    return a;
}

// Fixed, independently specified offsets, rather than the transport's parser.
// Everything not listed here, including padding/names/signatures, stays raw.
std::array<unsigned char,FileBytes> Oracle(const Bytes& archive, std::uint32_t channel)
{
    std::array<unsigned char,FileBytes> a{};
    std::memcpy(a.data(),archive.data()+FileAt,a.size()); auto* p=a.data();
    Native<std::uint16_t>(p+4,0xfeff); Native<std::uint16_t>(p+6,8);
    Native<std::uint32_t>(p+8,FileBytes); Native<std::uint16_t>(p+12,16); Native<std::uint16_t>(p+14,1);
    Native<std::uint32_t>(p+20,112); Native<std::uint16_t>(p+24,10);
    Native<std::uint16_t>(p+28,0); Native<std::uint16_t>(p+30,1); Native<std::uint32_t>(p+32,20);
    Native<std::uint32_t>(p+36,24); Native<std::uint32_t>(p+64,28); Native<std::uint32_t>(p+68,channel);
    Native<std::uint32_t>(p+76,12); Native<std::uint16_t>(p+84,3); Native<std::uint32_t>(p+88,12);
    Native<float>(p+92,0); Native<float>(p+96,2); Native<float>(p+100,1);
    Native<float>(p+104,4); Native<float>(p+108,6); Native<float>(p+112,1);
    Native<float>(p+116,10); Native<float>(p+120,0); Native<float>(p+124,-0.0f);
    return a;
}
void Publish(unsigned char* raw, const Bytes& bytes)
{
    GameByteWriteReservation write(raw,bytes.size());
    std::memcpy(raw,bytes.data(),bytes.size()); write.Complete(GameByteDomain::WiiSerialized);
}
void CheckView(unsigned char* raw, const Bytes& authored, std::uint32_t channel)
{
    auto* file=raw+FileAt;
    auto* head=static_cast<const res::BinaryFileHeader*>(NativeHBMAnimationHeader(file));
    Check(detail::TestFileHeader(*head) && detail::TestFileHeader(*head,0x524c414e),"Original animation header rejected valid RLAN0.8");
    Check(!detail::TestFileHeader(*head,0x524c5954),"Original signature predicate accepted RLYT for RLAN");
    const auto expected=Oracle(authored,channel);
    const auto* bytes=reinterpret_cast<const unsigned char*>(head);
    for (std::size_t i=0;i<FileBytes;++i) Check(bytes[i]==expected[i],"Native scalar/offset/padding byte differs from independent oracle");
    auto* block=detail::ConvertOffsToPtr<res::AnimationBlock>(head,head->headerSize);
    auto* contentOffsets=detail::ConvertOffsToPtr<u32>(block,block->animContOffsetsOffset);
    auto* content=detail::ConvertOffsToPtr<res::AnimationContent>(block,contentOffsets[0]);
    auto* infoOffsets=detail::ConvertOffsToPtr<u32>(content,sizeof(*content));
    auto* info=detail::ConvertOffsToPtr<res::AnimationInfo>(content,infoOffsets[0]);
    auto* targetOffsets=detail::ConvertOffsToPtr<u32>(info,sizeof(*info));
    auto* target=detail::ConvertOffsToPtr<res::AnimationTarget>(info,targetOffsets[0]);
    auto* keys=detail::ConvertOffsToPtr<res::HermiteKey>(target,target->keysOffset);
    Check(reinterpret_cast<const unsigned char*>(keys)==bytes+92 && target->keyNum==3 && info->kind==channel,
        "Original relative offset bases or four-byte channel changed");
    Check(ChargedTestHBMHermite(-1,keys,3)==2 && ChargedTestHBMHermite(11,keys,3)==0,"Original curve clamps changed");
    Check(ChargedTestHBMHermite(2,keys,3)==4 && ChargedTestHBMHermite(5.5f,keys,3)==5.90625f
        && ChargedTestHBMHermite(7,keys,3)==3.75f && ChargedTestHBMHermite(8.5f,keys,3)==1.21875f,
        "Actual original Hermite evaluation differs from fixed analytical values");
    Check(ChargedTestHBMHermite(3.9995f,keys,3)==6 && ChargedTestHBMHermite(2,keys+1,1)==6,
        "Original tolerance/single-key behavior changed");
    GameNativeBackingSpan backing{}; GameNativeBackingSourceSpan source{};
    Check(FindGameNativeBacking(file,FileBytes,backing) && backing.allocation.base==raw,
        "Animation view lost original raw allocation");
    Check(FindGameNativeBackingSource(head,FileBytes,source) && source.source==file && source.source_bytes==FileBytes,
        "Animation view has wrong exact raw source extent");
    Check(NativeHBMAnimationHeader(file)==head,"Repeated ingress replaced live typed metadata");
    Check(std::memcmp(raw,authored.data(),ArchiveBytes)==0,"Native view or original curve modified serialized bytes");
}
void PureCurves()
{
    const res::HermiteKey duplicate[]={{0,0,0},{2,3,0},{2,5,0},{4,7,0}};
    Check(ChargedTestHBMHermite(2,duplicate,4)==5 && ChargedTestHBMHermite(3,duplicate,4)==6,
        "Original duplicate-frame selection changed");
    // This tests the original Step function, not unsupported step-key ingress.
    const res::StepKey step[]={{0,10,0},{4,20,0},{8,30,0}};
    Check(ChargedTestHBMStep(-1,step,3)==10 && ChargedTestHBMStep(9,step,3)==30
        && ChargedTestHBMStep(1,step,3)==10 && ChargedTestHBMStep(4,step,3)==20,
        "Original step clamp/selection changed");
    Check(ChargedTestHBMStep(3.9995f,step,3)==20 && ChargedTestHBMStep(7.9995f,step,3)==30
        && ChargedTestHBMStep(99,step+1,1)==20,"Original step tolerance/single-key behavior changed");
}
}

int main()
{
    try {
        // CPU hardware owner, used by the genuine original debug providers.
        // This supplies neither a Layout allocator nor a HOME singleton.
        aurora::g_config.mem1Size=MEM1_DEFAULT_SIZE; aurora::g_config.mem2Size=64u*1024u*1024u;
        OSInit(); InitializeNativeInterruptController(); InitializeNativeAlarms();
        ConfigureNativeVideoHardware(VI_TVMODE_NTSC_INT,false); VIInit();
        alignas(64) std::array<std::byte,32768> arena{};
        MemoryAllocator owner{}; owner.Initialize(arena.data(),arena.size());
        const auto initial=owner.TotalFreeMemory(); std::uint64_t previous=0;
        PureCurves();
        for (unsigned iteration=0;iteration<2;++iteration) {
            auto* raw=static_cast<unsigned char*>(owner.Allocate(ArchiveBytes,32,false));
            Check(raw!=nullptr,"Original allocation failed"); auto* file=raw+FileAt;
            NativeARCFileSpan span{}; GameNativeBackingSpan backing{};
            Check(!FindNativeARCFileSpan(file,span),"Uncompleted ARC became readable");
            Reject([&]{NativeHBMAnimationHeader(file);});
            for (const auto channel: {0x524c5041u,0x524c5643u,0x524c4d43u}) {
                const auto authored=Archive(channel,channel==0x524c4d43);
                Publish(raw,authored);
                Check(!FindGameNativeBacking(file,FileBytes,backing),"New producer kept stale animation metadata");
                Check(FindNativeARCFileSpan(file,span) && span.base==file && span.bytes==FileBytes,
                    "Animation inherited archive tail rather than its exact file extent");
                if (channel==0x524c5041) {
                    Check(span.archive.allocation.incarnation!=previous,"Reload retained a retired incarnation");
                    previous=span.archive.allocation.incarnation;
                } else Check(span.archive.allocation.incarnation==previous,"In-place new read changed live allocation incarnation");
                Check(!FindNativeARCFileSpan(file+1,span) && !span.base,"Interior raw file pointer became an exact resource");
                CheckView(raw,authored,channel);
            }
            for (unsigned bad=0;bad<2;++bad) {
                auto authored=Archive(); Half(authored.data()+FileAt+(bad?6:4),bad?7:0xfffe);
                Word(authored.data()+FileAt+20,0xffffffff); // unreachable malformed block
                Publish(raw,authored);
                const auto* head=static_cast<const res::BinaryFileHeader*>(NativeHBMAnimationHeader(file));
                Check(!detail::TestFileHeader(*head),"Original header rejection was bypassed");
                Check(head->byteOrder==(bad?0xfeff:0xfffe) && head->version==(bad?7:8),"Rejected header values changed");
                Check(std::memcmp(raw,authored.data(),ArchiveBytes)==0,"Rejected header modified raw source bytes");
            }
            for (unsigned bad=0;bad<6;++bad) {
                auto authored=Archive(); auto* p=authored.data()+FileAt;
                switch (bad) {
                case 0: Word(p+8,FileBytes+4); break;
                case 1: Word(p+88,0xfffffff0); break;
                case 2: Half(p+84,0); break;
                case 3: Half(p+28,1); break;
                case 4: Word(p+68,0x524c5450); break;
                case 5: p[82]=1; break;
                }
                Publish(raw,authored); Reject([&]{NativeHBMAnimationHeader(file);});
                Check(!FindGameNativeBacking(file,FileBytes,backing),"Rejected representation published a native view");
                Check(std::memcmp(raw,authored.data(),ArchiveBytes)==0,"Rejected representation modified source bytes");
            }
            auto ambiguous=Archive(); Word(ambiguous.data()+60,FileAt); Publish(raw,ambiguous);
            Check(!FindNativeARCFileSpan(file,span) && !span.base,"Ambiguous FST start became authoritative");
            Reject([&]{NativeHBMAnimationHeader(file);});
            const auto authored=Archive(); Publish(raw,authored);
            const auto* last=NativeHBMAnimationHeader(file); owner.Free(raw);
            GameNativeBackingSourceSpan retired{};
            Check(!FindGameNativeBacking(file,FileBytes,backing) && !FindNativeARCFileSpan(file,span)
                && !FindGameNativeBackingSource(last,FileBytes,retired),"Free retained readable raw or native animation metadata");
        }
        Check(owner.TotalFreeMemory()==initial,"Original allocator did not recover all source storage");
        aurora_shutdown_video_hardware(); ShutdownNativeAlarms(); ShutdownNativeInterruptController(); AuroraOSShutdownMemory();
        std::printf("Original HBM RLAN/curves: %u checks passed\n",checks); return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr,"HBM animation check %u: %s\n",checks,e.what()); return 1;
    }
}
