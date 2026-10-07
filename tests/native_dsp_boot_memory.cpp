#include "platform/dsp_boot_memory.h"
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

namespace {
using namespace mscharged::platform;
unsigned checks;
void Check(bool value,const char* detail) {++checks;if(!value)throw std::runtime_error(detail);}
template<class F> void Throws(F&& fn,const char* detail) {
    bool failed=false;try{fn();}catch(const std::exception&){failed=true;}Check(failed,detail);
}
void Run(int argc,char** argv) {
    Check(argc==3 && std::strlen(argv[2])==64,"source boot leaf/hash required");
    const auto path=std::filesystem::absolute("sdk-data").string();std::filesystem::create_directories(path);
    AuroraConfig config{};config.appName="DSP boot DMA memory qualifier";
    config.userPath=config.cachePath=path.c_str();config.resourcesPath=".";config.desiredBackend=BACKEND_NULL;
    config.windowWidth=320;config.windowHeight=240;config.windowPosX=config.windowPosY=-1;
    config.mem1Size=MEM1_DEFAULT_SIZE;config.mem2Size=64u*1024u*1024u;config.logLevel=LOG_WARNING;
    Check(aurora_initialize(argc,argv,&config).window!=nullptr,"actual oneSDK initialization failed");OSInit();
    auto* source=SDL_LoadObject(argv[1]);Check(source!=nullptr,"actual source boot leaf missing");
    auto reader=reinterpret_cast<OriginalDSPBootData(*)()>(SDL_LoadFunction(source,"OriginalDSPBootDataForFixture"));
    Check(reader!=nullptr,"actual source initializer getter missing");auto boot=reader();
    Check(boot.count==128,"actual source program geometry changed");
    std::array<unsigned char,128> source_program;std::memcpy(source_program.data(),boot.bytes,boot.count);
    SDL_UnloadObject(source);
    constexpr std::uint32_t Main=0x01000000;
    auto* work=static_cast<unsigned char*>(OSPhysicalToCached(Main));
    Check(work && OSCachedToPhysical(work)==Main && Main+1024<=OSGetPhysicalMemSize(),
          "actual fixed source work memory alias not supplied by SDK");
    const auto memory=AttachNativeDSPMEM1();InitializeNativeInterruptController();
    const auto mailboxes=AttachNativeDSPMailboxes();const auto control=AttachNativeDSPControl(mailboxes);
    DSPInstructionCore chip(mailboxes,control);NativeDSPBootMemory dma(memory,control,chip);
    Throws([&]{dma.ARAMBytes();},"unwritten native ARAM invented zeros");
    Throws([&]{chip.InstructionWord(0);},"empty chip invented bootstrap instructions");
    Throws([&]{chip.DataWord(0x0ce4);},"boot transfer constructor seeded compressor/RAM");
    for(unsigned i=0;i<1024;++i)work[i]=static_cast<unsigned char>((i*29+0x5d)&255);
    std::copy(source_program.begin(),source_program.end(),work);
    const auto short_pin=PinNativeDSPMemory(work,128,false);
    dma.TransferARAM(Main,0,32);const auto first=dma.ARAMBytes();
    Check(std::equal(first.begin(),first.end(),source_program.begin()),"actual source32-byte ARAM transfer bytes changed");
    Throws([&]{dma.TransferBootstrapInstructions();},"128-byte source program extent was mistaken for1024-byte boot DMA");
    Check(dma.Status().aram_transfers==1 && dma.Status().instruction_transfers==0,
          "failed short boot transfer published completion metadata");
    Throws([&]{chip.InstructionWord(0);},"failed boot DMA installed a partial instruction bank");
    ReleaseNativeDSPMemory(short_pin);auto pin=PinNativeDSPMemory(work,1024,false);
    chip.LoadDataMemory(memory,Main+512,2,0x0ce4);const auto prior=chip.DataWord(0x0ce4);
    Check(prior!=0,"explicit prior DRAM fixture must be nonzero");
    dma.TransferARAM(Main,0,32);dma.TransferBootstrapInstructions();
    const auto actual=dma.Status();Check(actual.aram_transfers==2 && actual.last_aram_bytes==32 &&
          actual.instruction_transfers==1 && actual.last_instruction_bytes==1024,
          "sourceARAM/program/nativebootDMA sizes were merged or truncated");
    for(unsigned word=0;word<512;++word) {
        const auto high=word<64?source_program[word*2]:static_cast<unsigned char>((word*2*29+0x5d)&255);
        const auto low=word<64?source_program[word*2+1]:static_cast<unsigned char>(((word*2+1)*29+0x5d)&255);
        Check(chip.InstructionWord(word)==static_cast<std::uint16_t>(high*256u+low),
              "actual full1024-byte bootstrap bus word/endian/tail differs");
    }
    Check(chip.DataWord(0x0ce4)==prior && chip.Registers().instructions==0 && !DSPCheckInit(),
          "boot IRAM transfer fabricated cleared RAM, program execution or source readiness");
    Throws([&]{chip.InstructionWord(512);},"bootstrap DMA padded unwritten IRAM beyond512 actualwords");
    for(auto request:std::array<std::array<std::uint32_t,3>,7>{{
        {{Main+32,0,32}},{{Main,32,32}},{{Main,0,0}},{{Main,0,128}},
        {{Main,0,0x80000020u}},{{Main,0,31}},{{0xffffffffu,0,32}}}})
        Throws([&]{dma.TransferARAM(request[0],request[1],request[2]);},"unknown ARAM mode/address/count was accepted");
    Check(dma.ARAMBytes()==first && dma.Status().aram_transfers==2,
          "failed ARAM requests changed its independent actual destination");
    std::memset(work,0,1024);Check(dma.ARAMBytes()==first && chip.InstructionWord(0)==0x029f,
          "completed hardware transfers alias their mutable CPU source");
    // The exact retained original entry still rejects actual missing IROM. A
    // successful memory transfer does not authorize a fabricated boot flag/mail.
    ChargedDSPControlWrite(0);chip.BeginExecution({0,0xffff,0,{0,0},0});
    for(unsigned i=0;i<9;++i)chip.Step();const auto stopped=chip.Registers();
    Throws([&]{chip.Step();},"boot memory helper fabricated absent IROM");
    Check(chip.Registers().pc==0x1c && chip.Registers().instructions==9 &&
          chip.Registers().accumulator==stopped.accumulator && chip.DataWord(0x0ce4)==prior,
          "missing IROM altered actual retained boot state/data");
    Throws([&]{dma.TransferARAM(Main,0,32);},"running hardware accepted boot ARAM transfer");
    Throws([&]{dma.TransferBootstrapInstructions();},"running hardware accepted boot IRAM overwrite");
    ChargedDSPControlWrite(4);
    Check(dma.Status().aram_transfers==2 && dma.Status().instruction_transfers==1,
          "running boot DMA rejection changed transfer status");
    std::exception_ptr foreign;std::thread worker([&]{try{dma.TransferARAM(Main,0,32);}catch(...){foreign=std::current_exception();}});worker.join();
    Check(foreign!=nullptr && dma.Status().aram_transfers==2,"foreign boot memory owner was accepted");
    ReleaseNativeDSPMemory(pin);
    Throws([&]{dma.TransferARAM(Main,0,32);},"released bus pin remained usable for boot ARAM");
    Throws([&]{dma.TransferBootstrapInstructions();},"released bus pin remained usable for boot IRAM");
    Check(dma.ARAMBytes()==first && chip.InstructionWord(0)==0x029f && chip.DataWord(0x0ce4)==prior,
          "failed stale transfers changed previously completed hardware memory");
    Throws([&]{ChargedDSPControlWrite(0x0805);},"memory-only gate invented CSR reset/boot service");
    Check(!DSPCheckInit() && !__DSP_curr_task && !GetNativeDSPMailboxStatus().dsp_mail_full &&
          !GetNativeInterruptControllerStatus().pending_mask,
          "memory transfer fabricated original task/callback/mail/IRQ readiness");
    DetachNativeDSPControl();
    Throws([&]{dma.Status();},"retired hardware endpoint remained live");
    DetachNativeDSPMailboxes();ShutdownNativeInterruptController();DetachNativeDSPMEM1();aurora_shutdown();
    std::cout<<"native_dsp_boot_memory: "<<checks<<" checks; two actual32-byte ARAM copies and full1024-byte boot IRAM; actual128B source program retained, no reset/GPIO/cause/ROM/boot readiness\n";
}
}
int main(int argc,char** argv) {
    try {Run(argc,argv);return 0;}catch(const std::exception& error){std::cerr<<"native_dsp_boot_memory: "<<error.what()<<"\n";return 1;}
}
