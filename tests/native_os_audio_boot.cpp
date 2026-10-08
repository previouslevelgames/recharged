#include "platform/os_audio_boot_device.h"
#include "platform/os_audio_boot_abi.h"
#include "platform/os_boot_environment.h"
#include "platform/os_boot_environment_abi.h"
#include "platform/ax_bootstrap_device.h"
#include "platform/dsp_control_abi.h"
#include "platform/dsp_memory_abi.h"
#include "platform/interrupt_controller.h"
#include "platform/interrupts.h"
#include "dsp_boot_data.h"
#include <aurora/aurora.h>
#include <dolphin/os.h>
#include <SDL3/SDL_loadso.h>
extern "C" {
#include <revolution/dsp.h>
}
#include <algorithm>
#include <array>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace aurora { extern AuroraConfig g_config; }
void AuroraOSShutdown();

namespace {
using namespace mscharged::platform;
unsigned checks{},aram_irqs{},source_task_inits{};
void Check(bool good,const char* text) {++checks;if(!good)throw std::runtime_error(text);}
template<class F>void Throws(F&& fn,const char* text) {bool failed{};try{fn();}catch(const std::exception&){failed=true;}Check(failed,text);}
template<class F>F Load(SDL_SharedObject* image,const char* name) {
    auto result=SDL_LoadFunction(image,name);if(!result)throw std::runtime_error(SDL_GetError());return reinterpret_cast<F>(result);
}
void PutWord(unsigned char* p,unsigned index,unsigned value) {p[index*2]=value>>8;p[index*2+1]=value;}
void AramIRQ(s16,OSContext*) {++aram_irqs;ChargedDSPControlWrite(ChargedDSPControlRead()|0x20);}
void TaskInit(DSPTask*) {++source_task_inits;}

void Run(int argc,char** argv) {
    Check(argc==3&&std::strlen(argv[2])==64,"whole original OS image/hash required");
    // SDK memory/clock only; this fixture needs no GX device or SDL window.
    aurora::g_config.mem1Size=MEM1_DEFAULT_SIZE;
    aurora::g_config.mem2Size=64u*1024u*1024u;
    OSInit();
    Check(OSGetArenaLo()&&OSGetMEM2ArenaLo(),"actual oneSDK arenas unavailable");
    auto* image=SDL_LoadObject(argv[1]);Check(image!=nullptr,"whole OS source image failed to load");
    auto Init=Load<void(*)()>(image,"__OSInitAudioSystem");
    auto Stop=Load<void(*)()>(image,"__OSStopAudioSystem");
    auto boot=Load<OriginalDSPBootData(*)()>(image,"OriginalDSPBootDataForFixture")();
    auto firmware=Load<OriginalDSPBootData(*)()>(image,"OriginalAXFirmwareForOSBootFixture")();
    Check(boot.count==128&&firmware.count==8192,"actual source boot/AX byte geometry differs");
    InitializeNativeInterruptController();const auto memory=AttachNativeDSPMEM1();
    const auto mail=AttachNativeDSPMailboxes();const auto control=AttachNativeDSPControl(mail);
    auto* work=static_cast<unsigned char*>(OSPhysicalToCached(0x01000000));
    Check(work&&OSCachedToPhysical(work)==0x01000000,"actual fixed MEM1 alias missing");
    const auto work_pin=PinNativeDSPMemory(work,1024,false);
    auto* rom=static_cast<unsigned char*>(OSAllocFromArenaLo(8192+4096+8192,32));
    Check(rom!=nullptr,"actual SDK fixture backing missing");
    for(unsigned i=0;i<4096;++i)PutWord(rom,i,i*0x351du+0x2abcu);
    for(unsigned i=0;i<2048;++i)PutWord(rom+8192,i,i*0x187bu+0x6341u);
    std::memcpy(rom+12288,firmware.bytes,firmware.count);
    const auto rom_pin=PinNativeDSPMemory(rom,8192+4096+8192,false);
    const auto physical=ChargedDSPTaskMemoryWord(rom,8192+4096+8192,0);
    DSPTask source_task{};Throws([]{ChargedNativeOSInIPL();},"unconfigured native OS environment implied outsideIPL success");
    const NativeOSAudioGPIO input{0x5a5a5a5a,0xa55aa55a,0x6b6b6b6b};
    std::array<unsigned char,1024> original;
    for(unsigned i=0;i<original.size();++i)original[i]=(i*29+0x5d)&255;

    // Each case has a single retained chip/owner. Missing real banks fail at
    // genuine source instruction reads, not an artificial readiness predicate.
    for(unsigned variant=0;variant<4;++variant) {
        const bool irom=variant>0,drom=variant>1;
        std::copy(original.begin(),original.end(),work);
        ConfigureNativeOSBootEnvironment(variant==3?NativeOSBootEnvironment::IPLDiagnostic:NativeOSBootEnvironment::DesktopApplication);
        DSPInstructionCore chip(mail,control);
        if(irom)chip.LoadInstructionROM(memory,physical);
        if(drom)chip.LoadCoefficientROM(memory,physical+8192);
        // Deliberately nonzero prior RAM: only actual original SRRI may clear it.
        chip.LoadDataMemory(memory,physical,2,0x0ce4);const auto marker=chip.DataWord(0x0ce4);
        DSPInstructionRegisters entry{};entry.status=variant&1?0:0xffff;
        entry.accumulator={0x123456789aULL,0xabcdef0123ULL};
        entry.address={0x1357,0x2468,0xaaaa,0xbbbb};entry.index={0x1234,0x5678,0x9abc,0xdef0};
        entry.wrap={0x1111,0x2222,0x3333,0x4444};entry.stack={0xbeef,0xdead,0xabcd,0};
        NativeOSAudioBootDevice device(memory,mail,control,chip,input,entry);
        Throws([]{RetireNativeOSBootEnvironment();},"native OS environment retired with a live device lease");
        Throws([]{ConfigureNativeOSBootEnvironment(NativeOSBootEnvironment::DesktopApplication);},"native OS environment changed while already configured");
        const auto begin=OSGetTick();
        if(!drom) {
            Throws([&]{Init();},"original OS boot silently succeeded without supplied authentic bank");
            const auto registers=chip.Registers();const auto state=device.Status();
            Check(state.phase==NativeOSAudioBootPhase::Faulted&&state.aram_transfers==2&&state.reset_uploads==1,
                  "missing-ROM failure did not retain actual source requests");
            Check(registers.pc==(irom?0x26:0x1c)&&registers.instructions==(irom?12302:9),
                  "whole source boot failed at a fabricated boundary");
            Check(chip.DataWord(0x0ce4)==(irom?0:marker),"missing bank changed unexecuted source RAM clear");
            Check(std::equal(boot.bytes,boot.bytes+128,work)&&
                  std::memcmp(static_cast<unsigned char*>(OSGetArenaHi())-128,original.data(),128)==0,
                  "actual original backup/code writes were omitted on failed boot");
            Check(!GetNativeDSPMailboxStatus().dsp_mail_full&&!DSPCheckInit()&&!__DSP_curr_task,
                  "missing ROM fabricated source DSP initialization/mail/task");
            ChargedDSPControlWrite(4);device.Close();
            // Host fixture recovery after an explicit source exception is not
            // presented as a successful original backup restoration.
            std::copy(original.begin(),original.end(),work);RetireNativeOSBootEnvironment();continue;
        }
        Init();const auto state=device.Status();
        Check(state.phase==NativeOSAudioBootPhase::ImageLoaded&&state.reset_uploads==2&&
              state.aram_transfers==2&&state.executed_instructions==16404,
              "whole original OS source did not drive exact real requests/program completion");
        Check((std::uint32_t)(OSGetTick()-begin)>=2194,"original OS tick waits were skipped");
        Check(std::equal(original.begin(),original.end(),work),"source work backup/restore or untouched tail changed");
        const auto gpio=state.gpio;
        if(variant==3) {
            Check(state.gpio_writes==0&&gpio.diflags==input.diflags&&gpio.direction==input.direction&&gpio.input==input.input,
                  "original IPL branch modified GPIO");
        } else {
            const auto flags=(input.diflags&~0x180u)|0x100;
            const auto direction=(((input.direction&~0x3ffc0u)|0xffc0)&~0x3fu&~0x7fc0000u)|0xe|0x4b00000;
            const auto pins=(input.input&~0xd0000000u)|0xc0000000;
            Check(state.gpio_writes==6&&gpio.diflags==flags&&gpio.direction==direction&&gpio.input==pins,
                  "literal source GPIO read/modify/write masks/order differ");
        }
        for(unsigned i=0;i<4096;++i)Check(chip.DataWord(i)==0,"actual completed original boot did not clear every retained DRAM word");
        Check(chip.DataWord(0x1000)==0x6341&&chip.InstructionWord(0x8000)==0x2abc,
              "reset discarded actual supplied ROM bank ownership");
        Check((GetNativeDSPControlStatus().csr&0x0804)==0x0804&&
              !(GetNativeDSPControlStatus().csr&0x06a1)&&!GetNativeDSPMailboxStatus().dsp_mail_full&&
              !DSPCheckInit()&&!__DSP_curr_task,
              "original OS completion invented SDK DSP init or failed its actual reset/mail acknowledgments");
        // Exact real ARAM cause/mask/controller/ack lifecycle; not a manual IRQ.
        __OSSetInterruptHandler(6,AramIRQ);__OSUnmaskInterrupts(0x02000000);
        const auto prior_irqs=aram_irqs;
        device.WriteDSPPair(16,0x01000000);device.WriteDSPPair(18,0);device.WriteDSPPair(20,32);
        Check(GetNativeDSPControlStatus().csr&0x20,"actual checked ARAM copy failed to latch cause");
        ServiceNativeInterruptController();
        Check(aram_irqs==prior_irqs+1&&!(GetNativeDSPControlStatus().csr&0x20),
              "real ARAM owner IRQ/W1C failed or repeated");
        __OSMaskInterrupts(0x02000000);__OSSetInterruptHandler(6,nullptr);
        Throws([&]{device.WriteDSPPair(20,31);},"unsupported ARAM count accepted");
        Throws([&]{device.ReadIPC(0);},"unsupported IPC register accepted");
        Throws([&]{device.WriteDSP(9,0);},"unsupported ARAM geometry accepted");
        std::exception_ptr foreign;std::thread t([&]{try{device.ReadDSP(5);}catch(...){foreign=std::current_exception();}});t.join();
        Check(foreign!=nullptr,"foreign source MMIO owner accepted");
        Stop();Check(device.Status().reset_uploads==3&&device.Status().executed_instructions==16404,
                     "original OS stop reset/transport was replaced with a ready wrapper");
        for(unsigned i=0;i<4096;++i)Check(chip.DataWord(i)==0,"original stop/reset discarded actual cleared DRAM");
        Check(std::equal(original.begin(),original.end(),work),"original stop unexpectedly changed work backing");
        device.Close();RetireNativeOSBootEnvironment();Throws([]{ChargedNativeOSInIPL();},"retired native OS environment retained success");Throws([&]{ChargedOSAudioDSPRead(5);},"retired OS boot owner remained usable");
        if(variant==3) {
            // The later ORIGINAL DSPInit/task loader reuses this same chip.
            // Only its actual AX21-instruction prefix/IRQ is qualified here;
            // generated ROM does not authorize authentic active voice readiness.
            NativeAXBootstrapDevice ax(memory,mail,control,physical+12288,NativeAXFrameMode::BootstrapOnly,chip);
            DSPInit();auto& task=source_task;task.prio=0xf0;task.iramMmemAddr=rom+12288;task.iramMmemLen=8192;
            task.startVector=0x10;task.initCallback=TaskInit;
            DSPAddTask(&task);ServiceNativeInterruptController();
            Check(source_task_inits==1&&__DSP_curr_task==&task&&ax.Status().firmware_instructions==21,
                  "later original source loader did not execute real initialization callback on retained chip");
            Check(chip.DataWord(0x0ce4)==0&&chip.DataWord(0x0ce5)==0x8000,
                  "later AX loader discarded actual cold clear or failed original gain stores");
            Check(chip.DataWord(0x1000)==0x6341&&chip.InstructionWord(0x8000)==0x2abc,
                  "later AX loader discarded actual supplied banks");
            ChargedDSPControlWrite(4);ax.Close();
            Check(chip.DataWord(0x0ce4)==0,"native processor retirement discarded borrowed chip RAM");
            // Actual source task/global cleanup is not implemented by this leaf.
            // Keep this last source task alive through host device teardown.
        }
    }

    ReleaseNativeDSPMemory(rom_pin);ReleaseNativeDSPMemory(work_pin);
    DetachNativeDSPControl();DetachNativeDSPMailboxes();DetachNativeDSPMEM1();
    ShutdownNativeInterruptController();SDL_UnloadObject(image);AuroraOSShutdown();
    std::cout<<"native_os_audio_boot: "<<checks<<" checks; whole original init/stop, missing-bank stops, generated-bank literal RAM clear and retained-chip source loader; authentic ROM/active kernel/game audio remains held\n";
}
}
int main(int argc,char** argv) {try{Run(argc,argv);return 0;}catch(const std::exception& e){std::cerr<<"native_os_audio_boot: "<<e.what()<<"\n";return 1;}}
