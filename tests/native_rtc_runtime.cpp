#include "platform/rtc_policy.h"
#include "platform/system.h"
#include "platform/interrupt_controller.h"
#include <aurora/aurora.h>
#include <dolphin/os.h>
#include <revolution/sc.h>
#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace aurora { extern AuroraConfig g_config; }
void AuroraOSShutdown();
extern "C" {
void __OSInitSram();
BOOL __OSSyncSram();
BOOL __OSGetRTCFlags(u32*);
}
namespace {
using namespace mscharged::platform;
unsigned checks{};
void Check(bool okay,const char* text) { ++checks; if(!okay) throw std::runtime_error(text); }
std::array<unsigned char,68> Read(const std::filesystem::path& path) {
    std::ifstream file(path,std::ios::binary); std::array<unsigned char,68> bytes{};
    Check(bool(file.read(reinterpret_cast<char*>(bytes.data()),bytes.size())) && file.peek()==EOF,
          "RTC runtime image is not exactly the declared 68 bytes");
    return bytes;
}
void Run(const std::filesystem::path& directory) {
    aurora::g_config.mem1Size=MEM1_DEFAULT_SIZE;
    aurora::g_config.mem2Size=64u*1024u*1024u;
    OSInit(); InitializeNativeInterruptController();
    Check(OSGetArenaLo() && OSGetMEM2ArenaLo(),"Sole SDK OS arenas are unavailable");
    Check(!__OSSyncSram(),"Original SRAM cache initialized before real EXI attachment/read");
    const auto path=directory/"native-rtc.bin";
    InitializeNativeRTC(path,CreateVirginRTCImage());
    Check(!__OSSyncSram(),"Native first-run policy seeded original cache synchronization");
    // Independent explicit image oracle: all virgin payload fields are zero;
    // original four zero checksum words produce sum=0, inverse=FFFC.
    std::array<unsigned char,68> expected{};
    expected[2]=0xff; expected[3]=0xfc;
    Check(Read(path)==expected,"First-run hardware bytes differ from the declared native policy");
    __OSInitSram();
    Check(__OSSyncSram(),"Whole original SRAM initialization failed its actual EXI read");
    u32 flags=0xdeadbeef;
    Check(__OSGetRTCFlags(&flags) && flags==0,"Real native RTC read differs from no produced event");
    for(int channel=0;channel<4;++channel)
        Check(OSGetWirelessID(channel)==0,"Virgin legacy wireless ID changed source zero register");
    Check(Read(path)==expected,"Original cold read/GBS policy changed virgin backing");

    // Same setup as the real host: explicitly absent IPL.IDL, then source SC
    // initialization and its actual getter. No destination/default is seeded.
    mscharged::ConfigureNativeSystemSettings({1,0,0,1,1});
    mscharged::ConfigureNativeSystemSimpleAddress(std::nullopt);
    mscharged::ConfigureNativeSystemIdleMode(nullptr,0);
    Check(SCCheckStatus()==SC_STATUS_BUSY,"Staging absent records claimed initialized SC");
    SCInit();
    SCIdleModeInfo untouched{0xa5,0x3c}; SCGetIdleMode(&untouched);
    Check(untouched.wc24==0xa5 && untouched.slotLight==0x3c,
          "Absent original idle query wrote caller bytes");
    SCIdleModeInfo source_zero_default{}; SCGetIdleMode(&source_zero_default);
    Check(source_zero_default.wc24==0 && source_zero_default.slotLight==0,
          "Absent idle query changed the original shutdown zero/default branch input");

    // The real original source requests the change. A later explicit virgin
    // policy must preserve the existing hardware, not reset that source state.
    OSSetWirelessID(3,0x1357);
    Check(__OSSyncSram() && OSGetWirelessID(3)==0x1357,"Original persistent ID update failed");
    expected[54]=0x13; expected[55]=0x57;
    Check(Read(path)==expected,"Source changed more than its original SRAM payload");
    ShutdownNativeRTC();
    Check(__OSSyncSram(),"Hardware retirement rewrote the original cached source flag");
    InitializeNativeRTC(path,CreateVirginRTCImage());
    Check(Read(path)==expected,"Repeated native policy overwrote existing retained hardware image");
    __OSInitSram();
    Check(__OSSyncSram() && OSGetWirelessID(3)==0x1357,"Source reload lost the actual retained image");
    ShutdownNativeRTC();
    mscharged::ShutdownNativeSystemSettings();
    ShutdownNativeInterruptController(); AuroraOSShutdown();
    std::cout<<"native_rtc_runtime: "<<checks<<" checks; explicit virgin image, original EXI/init, retained image and absent source SC idle query; no main/reset claim\n";
}
}
int main(int argc,char** argv) {
    try { Check(argc==2,"Supply a disposable existing directory"); Run(argv[1]); return 0; }
    catch(const std::exception& error) { std::cerr<<"native_rtc_runtime: "<<error.what()<<'\n'; return 1; }
}
