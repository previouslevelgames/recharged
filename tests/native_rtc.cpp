#include "platform/rtc_device.h"
#include "platform/interrupts.h"
#include <aurora/aurora.h>
#include <dolphin/os.h>
#include <dolphin/exi.h>
#include <algorithm>
#include <fstream>
#include <iostream>
#include <thread>
#include <stdexcept>
#include <type_traits>
namespace aurora { extern AuroraConfig g_config; }
void AuroraOSShutdown();
extern "C" {
void __OSInitSram();
BOOL __OSSyncSram();
BOOL __OSGetRTCFlags(u32*);
BOOL __OSClearRTCFlags();
BOOL __OSReadROM(void*,s32,s32);
BOOL ChargedRTCCommitBaseForFixture();
}
static_assert(sizeof(OSSram)==20 && sizeof(OSSramEx)==44);
static_assert(offsetof(OSSram,counterBias)==12 && offsetof(OSSram,flags)==19);
static_assert(offsetof(OSSramEx,wirelessPadID)==28 && offsetof(OSSramEx,gbs)==40);
static_assert(sizeof(u32)==4 && sizeof(u16)==2);
namespace {
using namespace mscharged::platform;
unsigned checks{};
void Check(bool okay,const char* text) {++checks;if(!okay)throw std::runtime_error(text);}
template<class F>void Reject(F fn,const char* text) {bool failed{};try{fn();}catch(const std::exception&){failed=true;}Check(failed,text);}
std::array<u8,68> Backing(const std::filesystem::path& file) {
    std::array<u8,68> result{};std::ifstream input(file,std::ios::binary);
    Check(bool(input.read(reinterpret_cast<char*>(result.data()),result.size())) && input.peek()==EOF,
          "actual native backing length/read differs");return result;
}
void Run(const std::filesystem::path& directory) {
    aurora::g_config.mem1Size=MEM1_DEFAULT_SIZE;
    aurora::g_config.mem2Size=64u*1024u*1024u;OSInit();
    Check(OSGetArenaLo() && OSGetMEM2ArenaLo(),"actual sole SDK arenas missing");
    const auto file=directory/"rtc.bin";
    u32 flags=0x13572468;
    Check(!__OSGetRTCFlags(&flags) && flags==0x13572468 && !__OSClearRTCFlags(),
          "unconfigured virtual hardware fabricated RTC flags/clear");
    __OSInitSram();Check(!__OSSyncSram(),"failed actual read fabricated source SRAM synchronization");
    Reject([&]{InitializeNativeRTC(file,std::nullopt);},"missing backing selected an implicit SRAM/default image");
    NativeRTCImage initial;
    for(unsigned i=0;i<64;++i)initial.sram[i]=i*17+3;
    initial.sram[12]=0x12;initial.sram[13]=0x34;initial.sram[14]=0xab;initial.sram[15]=0xcd;
    initial.sram[16]=0x87;initial.sram[17]=0x65;initial.sram[18]=0x24;initial.sram[19]=0x9b;
    initial.sram[60]=0x12;initial.sram[61]=0x34; // Valid original GBS mode.
    initial.flags=0xf0a56789;
    InitializeNativeRTC(file,initial);
    Reject([&]{InitializeNativeRTC(file,initial);},"second virtual device replaced live owner");
    Check(!__OSSyncSram(),"hardware attachment seeded the original Scb cache flag");
    __OSInitSram();Check(__OSSyncSram(),"actual original64-byte EXI SRAM read did not complete");
    Check(__OSGetRTCFlags(&flags) && flags==initial.flags,"original flag read lost register bits");
    auto bytes=Backing(file);
    Check(std::equal(initial.sram.begin(),initial.sram.end(),bytes.begin()),"source initial SRAM read altered backing");
    Check(bytes[64]==0xf0 && bytes[65]==0xa5 && bytes[66]==0x67 && bytes[67]==0x89,
          "persistent hardware flag endian representation differs");
    for(unsigned channel=0;channel<4;++channel) {
        const auto offset=48+channel*2;
        const u16 expected=(u16(initial.sram[offset])<<8)|initial.sram[offset+1];
        Check(OSGetWirelessID(channel)==expected,"original Wii16 wireless ID read differs");
        const u16 next=0x1234+channel*0x1111;
        OSSetWirelessID(channel,next);
        Check(__OSSyncSram() && OSGetWirelessID(channel)==next,"original ID write/cache status differs");
        bytes=Backing(file);
        Check(bytes[offset]==u8(next>>8) && bytes[offset+1]==u8(next),"actual source write did not persist Wii16 ID");
    }
    Check(ChargedRTCCommitBaseForFixture() && __OSSyncSram(),"paired original base SRAM lock/commit failed");
    bytes=Backing(file);
    // Independent original four-word checksum:1234+ABCD+8765+2498=169FE.
    Check(bytes[19]==0x98 && bytes[0]==0x69 && bytes[1]==0xfe && bytes[2]==0x95 && bytes[3]==0xfe,
          "original flags/checksum16 wrapping/wire bytes differ");
    Check(__OSClearRTCFlags() && __OSGetRTCFlags(&flags) && flags==0,
          "real original flag clear did not change actual hardware register");
    bytes=Backing(file);
    Check(std::all_of(bytes.begin()+64,bytes.end(),[](u8 b){return b==0;}),"RTC clear did not persist all32 register bits");
    const auto before=ReadNativeRTCImage();
    Check(EXILock(0,1,nullptr),"actual EXI lock for source contention failed");
    OSSetWirelessID(0,0x9abc);
    Check(!__OSSyncSram() && ReadNativeRTCImage().sram==before.sram,
          "failed locked source write fabricated sync/store");
    Reject([]{ShutdownNativeRTC();},"device retired with borrowed lock/source retry");
    Check(EXIUnlock(0) && __OSSyncSram() && OSGetWirelessID(0)==0x9abc,
          "original EXIUnlock→source WriteSramCallback retry did not complete");
    bytes=Backing(file);Check(bytes[48]==0x9a && bytes[49]==0xbc,"actual original lock retry did not persist bytes");
    std::filesystem::create_directory(file.string()+".commit");
    const auto durable=Backing(file);OSSetWirelessID(1,0xeeee);
    Check(!__OSSyncSram() && OSGetWirelessID(1)==0xeeee,"source failed-write cache quirk was altered");
    Check(Backing(file)==durable,"failed backing commit changed persistent hardware image");
    OSSetWirelessID(1,0xeeee);Check(!__OSSyncSram(),"same-value retail branch silently repaired failed synchronization");
    std::filesystem::remove(file.string()+".commit");
    OSSetWirelessID(1,0xeeed);Check(__OSSyncSram(),"actual subsequent changed-value write did not commit");
    bool foreign{};std::thread thread([&] {u32 sentinel=0x11223344;foreign=!__OSGetRTCFlags(&sentinel)&&sentinel==0x11223344;});thread.join();
    Check(foreign,"foreign source RTC query mutated owner/output");
    Check(!EXILock(1,1,nullptr) && !EXILock(0,0,nullptr),"unsupported EXI device claimed lock success");
    Check(EXILock(0,1,nullptr) && EXISelect(0,1,3),"actual supported EXI selection failed");
    u32 command=0xA0000100;
    Check(EXIImm(0,&command,4,EXI_WRITE,nullptr)&&EXISync(0),"real SRAM write command did not transfer/synchronize");
    const auto unchanged=ReadNativeRTCImage();std::array<u8,65> too_many{};
    Check(!EXIImmEx(0,too_many.data(),too_many.size(),EXI_WRITE) && EXIDeselect(0) && EXIUnlock(0) &&
          ReadNativeRTCImage().sram==unchanged.sram,"unsupported extent partially wrote actual device backing");
    too_many.fill(0x5a);
    Check(!__OSReadROM(too_many.data(),1,0) && std::all_of(too_many.begin(),too_many.end(),[](u8 b){return b==0x5a;}),
          "unsupported ROM request fabricated success/source bytes");
    const auto retained=ReadNativeRTCImage();ShutdownNativeRTC();
    flags=0x7badbeef;Check(!__OSGetRTCFlags(&flags)&&flags==0x7badbeef,"retired RTC endpoint exposed stale hardware");
    Check(__OSSyncSram(),"host retirement changed the original cached sync flag");
    InitializeNativeRTC(file,std::nullopt);__OSInitSram();
    Check(__OSSyncSram() && ReadNativeRTCImage().sram==retained.sram && ReadNativeRTCImage().flags==retained.flags,
          "real retained backing/reload changed source image");
    ShutdownNativeRTC();AuroraOSShutdown();
    std::cout<<"native_rtc: "<<checks<<" checks; whole original source EXI read/write/flags/source retry and persistent failures; no production defaults\n";
}
}
int main(int argc,char** argv) {
    try {Check(argc==2,"supply disposable existing backing directory");Run(argv[1]);return 0;}
    catch(const std::exception& error){std::cerr<<"native_rtc: "<<error.what()<<'\n';return 1;}
}
