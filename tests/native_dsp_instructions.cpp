#include "platform/dsp_instruction_core.h"
#include "platform/interrupt_controller.h"
#include "dsp_source_module.h"
#include <aurora/aurora.h>
#include <dolphin/os.h>
#include <dolphin/os/OSNativeMemory.h>
#include <SDL3/SDL_loadso.h>
extern "C" {
#include <revolution/dsp.h>
}
#include <array>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using namespace mscharged::platform;
unsigned checks;
void Check(bool result,const char* message) {
    ++checks;if(!result)throw std::runtime_error(message);
}
template<class F>void Throws(F&& function,const char* message) {
    bool rejected=false;try{function();}catch(const std::exception&){rejected=true;}
    Check(rejected,message);
}
struct SDKLifetime {bool live{};~SDKLifetime(){if(live)aurora_shutdown();}};
struct ModuleLease {
    std::string path;
    std::vector<SDL_SharedObject*> handles;
    static BOOL Retain(void* context) noexcept {
        auto& self=*static_cast<ModuleLease*>(context);
        auto* handle=SDL_LoadObject(self.path.c_str());
        if(!handle)return FALSE;
        try{self.handles.push_back(handle);}catch(...){SDL_UnloadObject(handle);return FALSE;}
        return TRUE;
    }
    static void Release(void* context) noexcept {
        auto& self=*static_cast<ModuleLease*>(context);
        auto* handle=self.handles.back();self.handles.pop_back();SDL_UnloadObject(handle);
    }
};
void Equal(const DSPInstructionRegisters& actual,const DSPInstructionRegisters& expected) {
    Check(actual.pc==expected.pc && actual.status==expected.status && actual.control==expected.control
          && actual.accumulator==expected.accumulator && actual.instructions==expected.instructions,
          "actual firmware instruction differs from independent register-cell trace");
}

void Run(int argc,char** argv) {
    Check(argc==3 && std::strlen(argv[2])==64,"fixture needs the real source image and SHA256 identity");
    SDKLifetime sdk;
    const auto directory=std::filesystem::absolute("sdk-data").string();
    std::filesystem::create_directories(directory);
    AuroraConfig config{};config.appName="Original AX DSP instruction qualifier";
    config.userPath=config.cachePath=directory.c_str();config.resourcesPath=".";
    config.desiredBackend=BACKEND_NULL;config.windowWidth=320;config.windowHeight=240;
    config.windowPosX=config.windowPosY=-1;config.mem1Size=MEM1_DEFAULT_SIZE;
    config.mem2Size=64u*1024u*1024u;config.logLevel=LOG_WARNING;
    Check(aurora_initialize(argc,argv,&config).window!=nullptr,"actual SDK window missing");
    sdk.live=true;OSInit();
    Check(!DSPCheckInit(),"instruction qualifier must not initialize the original source DSP state");
    ModuleLease lease{std::filesystem::absolute(argv[1]).string(),{}};
    auto* initial=SDL_LoadObject(lease.path.c_str());Check(initial!=nullptr,"actual source module failed to load");
    auto function=reinterpret_cast<OriginalDSPData(*)()>(SDL_LoadFunction(initial,"OriginalDSPDataForFixture"));
    Check(function!=nullptr,"actual original AX data export missing");
    const auto data=function();
    Check(data.code_bytes==8192 && data.dram_bytes==64 && data.initial_vector==0x10
          && data.resume_vector==0x37,"original AX source data/vector contract changed");
    Check(reinterpret_cast<std::uintptr_t>(data.code)>0xffffffffULL,
          "source input must exercise genuine native address width");
    OSNativeStaticMemoryOwner code_owner{argv[2],"axDspSlave",data.code,data.code_bytes,FALSE,
        &lease,ModuleLease::Retain,ModuleLease::Release};
    OSNativeStaticMemoryOwner dram_owner{argv[2],"__AXDramImage",data.dram,data.dram_bytes,TRUE,
        &lease,ModuleLease::Retain,ModuleLease::Release};
    const auto code=OSNativeRegisterStaticMemory(&code_owner);
    const auto dram=OSNativeRegisterStaticMemory(&dram_owner);
    SDL_UnloadObject(initial);
    Check(lease.handles.size()==2,"real SDL lifetime leases must retain both actual source arrays");
    const auto memory=AttachNativeDSPMEM1();
    const auto code_pin=PinNativeDSPMemory(data.code,data.code_bytes,false);
    const auto dram_pin=PinNativeDSPMemory(data.dram,data.dram_bytes,true);
    InitializeNativeInterruptController();const auto mailboxes=AttachNativeDSPMailboxes();
    DSPInstructionCore core(mailboxes);
    Throws([&]{core.Step();},"firmware ran without an explicit execution context");
    core.LoadInstructionMemory(memory,code.physical_address,data.code_bytes,0);
    core.LoadDataMemory(memory,dram.physical_address,data.dram_bytes,0x0cd2);
    // These are independently transcribed original firmware words, not an
    // emulator disassembly/table or a synthesized boot/init program.
    constexpr std::array<std::uint16_t,32> prefix{
        0x1302,0x1303,0x1204,0x1305,0x1306,0x8e00,0x8c00,0x8b00,
        0x0092,0x00ff,0x009e,0x8000,0x00fe,0x0ce5,0x009e,0x8000,
        0x00fe,0x0ce6,0x00fe,0x0ce7,0x00fe,0x0ce8,0x8100,0x00fe,
        0x0ce9,0x8900,0x16fc,0xdcd1,0x16fd,0x0000,0x16fb,0x0001};
    for(unsigned i=0;i<prefix.size();++i)
        Check(core.InstructionWord(0x10+i)==prefix[i],"actual raw firmware word/endian differs from authored bytes");
    for(unsigned i=0;i<32;++i)Check(core.DataWord(0x0cd2+i)==0,"actual DRAM preload changed");
    Throws([&]{core.InstructionWord(0x8000);},"missing boot ROM acquired fake instruction bytes");
    Throws([&]{core.DataWord(0x1000);},"missing coefficient ROM acquired fake data words");
    DSPInstructionRegisters initial_registers{data.initial_vector,0x4000,0x5a,
        {0xab23456789ULL,0xff00001000ULL},0};
    // Supplied nonzero cells prove SET16 preserves the high/low fields; they
    // are diagnostic input, not claimed ROM/reset defaults or a boot handshake.
    core.BeginExecution(initial_registers);
    struct Row {std::uint16_t pc,status;std::uint8_t cr;std::uint64_t a,b;};
    constexpr std::array<Row,20> trace{{
        {0x11,0x4100,0x5a,0xab23456789ULL,0xff00001000ULL},
        {0x12,0x4300,0x5a,0xab23456789ULL,0xff00001000ULL},
        {0x13,0x4300,0x5a,0xab23456789ULL,0xff00001000ULL},
        {0x14,0x4b00,0x5a,0xab23456789ULL,0xff00001000ULL},
        {0x15,0x5b00,0x5a,0xab23456789ULL,0xff00001000ULL},
        {0x16,0x1b00,0x5a,0xab23456789ULL,0xff00001000ULL},
        {0x17,0x1b00,0x5a,0xab23456789ULL,0xff00001000ULL},
        {0x18,0x3b00,0x5a,0xab23456789ULL,0xff00001000ULL},
        {0x1a,0x3b00,0xff,0xab23456789ULL,0xff00001000ULL},
        {0x1c,0x3b00,0xff,0xab80006789ULL,0xff00001000ULL},
        {0x1e,0x3b00,0xff,0xab80006789ULL,0xff00001000ULL},
        {0x20,0x3b00,0xff,0xab80006789ULL,0xff00001000ULL},
        {0x22,0x3b00,0xff,0xab80006789ULL,0xff00001000ULL},
        {0x24,0x3b00,0xff,0xab80006789ULL,0xff00001000ULL},
        {0x26,0x3b00,0xff,0xab80006789ULL,0xff00001000ULL},
        {0x27,0x3b24,0xff,0,0xff00001000ULL},
        {0x29,0x3b24,0xff,0,0xff00001000ULL},
        {0x2a,0x3b24,0xff,0,0},
        {0x2c,0x3b24,0xff,0,0},
        {0x2e,0x3b24,0xff,0,0}}};
    for(unsigned i=0;i<trace.size();++i) {
        const auto& t=trace[i];
        Equal(core.Step(),DSPInstructionRegisters{t.pc,t.status,t.cr,{t.a,t.b},i+1});
        if(i==18)Check(!DSPCheckMailFromDSP(),"high mailbox instruction published before actual low store");
    }
    for(unsigned i=0;i<4;++i)Check(core.DataWord(0x0ce5+i)==0x8000,"original firmware DRAM middle-word store changed");
    Check(core.DataWord(0x0ce9)==0,"original accumulator clear/store failed");
    std::array<unsigned char,64> zeros{};
    Check(std::memcmp(data.dram,zeros.data(),zeros.size())==0,
          "internal DSP DRAM writes invented an unrequested source context save");
    Check(DSPCheckMailFromDSP(),"actual firmware low store did not publish hardware mailbox");
    Check(reinterpret_cast<std::uintptr_t>(DSPReadMailFromDSP())==0xdcd10000ULL,
          "actual original source reader disagrees with firmware-authored mailbox word");
    Check(!DSPCheckMailFromDSP(),"actual original mailbox read did not acknowledge device word");
    const auto before=core.Registers();
    bool held=false;
    try{core.Step();}catch(const DSPUnsupportedInstruction& failure){
        held=failure.pc==0x2e && failure.opcode==0x16fb && failure.operand==0xfffb;
    }
    Check(held,"actual firmware must stop explicitly at unimplemented IRQ device register");
    Equal(core.Registers(),before);
    Check(!DSPCheckInit() && GetNativeInterruptControllerStatus().pending_mask==0,
          "partial firmware execution fabricated source initialization or a device IRQ");

    // Independent pair-mask and safe transport boundaries on real MEM2 bytes.
    auto* bytes=static_cast<unsigned char*>(OSAllocFromMEM2ArenaLo(64,32));
    const auto generated_pin=PinNativeDSPMemory(bytes,64,true);
    const auto physical=OSCachedToPhysical(bytes);
    constexpr std::array<std::uint16_t,6> mode_words{0x8a00,0x8b00,0x8c00,0x8d00,0x8e00,0x8f00};
    constexpr std::array<std::uint16_t,6> statuses{0xdfff,0x2000,0x7fff,0x8000,0xbfff,0x4000};
    for(unsigned i=0;i<mode_words.size();++i) {
        bytes[i*2]=static_cast<unsigned char>(mode_words[i]>>8);
        bytes[i*2+1]=static_cast<unsigned char>(mode_words[i]);
    }
    core.LoadInstructionMemory(memory,physical,12,0x100);
    for(unsigned i=0;i<mode_words.size();++i) {
        DSPInstructionRegisters context{static_cast<std::uint16_t>(0x100+i),
            static_cast<std::uint16_t>((i&1)?0:0xffff),0,{0,0},0};
        core.BeginExecution(context);
        Check(core.Step().status==statuses[i],"mode instruction changed the wrong independent status cell");
    }
    bytes[0]=0x00;bytes[1]=0x92;bytes[2]=0xab;bytes[3]=0x34;
    core.LoadInstructionMemory(memory,physical,4,0x100);core.BeginExecution({0x100,0,0,{0,0},0});
    Check(core.Step().control==0x34,"8-bit config register retained ignored high bits");
    bytes[0]=0x8e;bytes[1]=0x01;
    core.LoadInstructionMemory(memory,physical,2,0x100);core.BeginExecution({0x100,0xffff,0,{0,0},0});
    auto rejected=core.Registers();Throws([&]{core.Step();},"unsupported extended write was silently skipped");Equal(core.Registers(),rejected);
    bytes[0]=0x00;bytes[1]=0x80;bytes[2]=0x12;bytes[3]=0x34;
    core.LoadInstructionMemory(memory,physical,4,0x100);core.BeginExecution({0x100,0,0,{0,0},0});
    rejected=core.Registers();Throws([&]{core.Step();},"unsupported address register acquired invented semantics");Equal(core.Registers(),rejected);
    bytes[0]=0x00;bytes[1]=0x9e;bytes[2]=0x80;bytes[3]=0x00;
    core.LoadInstructionMemory(memory,physical,4,0x100);core.BeginExecution({0x100,0x4000,0,{0,0},0});
    rejected=core.Registers();Throws([&]{core.Step();},"unimplemented SET40 saturation/sign-extension silently ran");Equal(core.Registers(),rejected);
    DSPInstructionCore missing(mailboxes);
    Throws([&]{missing.BeginExecution({0x8000,0,0,{0,0},0});},"missing boot ROM started successfully");
    missing.LoadInstructionMemory(memory,physical,2,0xfff);
    missing.BeginExecution({0xfff,0,0,{0,0},0});
    rejected=missing.Registers();Throws([&]{missing.Step();},"missing next instruction word became a fake zero immediate");Equal(missing.Registers(),rejected);
    Throws([&]{core.LoadInstructionMemory(memory,physical,2,0x1000);},"IRAM transfer crossed its exact word bank");
    Throws([&]{core.LoadDataMemory(memory,physical,3,0);},"odd-byte DMA became an implicit padded word");
    Throws([&]{core.LoadDataMemory(memory,physical,4,0xfff);},"data transfer crossed its exact word bank");
    Throws([&]{core.LoadInstructionMemory(memory,physical,66,0);},"device transfer exceeded genuinely retained backing");
    Throws([&]{core.BeginExecution({0x10,0,0,{0x10000000000ULL,0},0});},"wide host value became a raw hardware accumulator");
    ReleaseNativeDSPMemory(generated_pin);
    ReleaseNativeDSPMemory(dram_pin);ReleaseNativeDSPMemory(code_pin);
    OSNativeReleaseStaticMemory(dram);OSNativeReleaseStaticMemory(code);
    Check(lease.handles.empty(),"drained source instruction transfer retained loader backing");
    Check(core.InstructionWord(0x10)==0x1302,"actual device IRAM was an unowned host pointer after unload");
    DetachNativeDSPMEM1();DetachNativeDSPMailboxes();ShutdownNativeInterruptController();
    std::cout<<"original_ax_prefix: 20 instructions, PC=0x002e, mail=0xdcd10000 from source immediate; hold IFX=0xfffb\n";
}
}
int main(int argc,char** argv) {
    try{Run(argc,argv);std::cout<<"native_dsp_instructions: "<<checks<<" checks passed\n";return 0;}
    catch(const std::exception& failure){std::cerr<<"native_dsp_instructions: "<<failure.what()<<"\n";return 1;}
}
