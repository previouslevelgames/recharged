#include "platform/dsp_instruction_core.h"
#include "platform/dsp_control_abi.h"
#include "platform/interrupt_controller.h"
#include "platform/interrupts.h"
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
#include <exception>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {
using namespace mscharged::platform;
unsigned checks,observed;
std::thread::id owner;
void Check(bool result,const char* message) {++checks;if(!result)throw std::runtime_error(message);}
template<class F> void Throws(F&& function,const char* message) {
    bool held=false;try{function();}catch(const std::exception&){held=true;}Check(held,message);
}
struct SDKLifetime {bool live{};~SDKLifetime(){if(live)aurora_shutdown();}};
struct ModuleLease {
    std::string path;std::vector<SDL_SharedObject*> handles;
    static BOOL Retain(void* context) noexcept {
        auto& self=*static_cast<ModuleLease*>(context);auto* handle=SDL_LoadObject(self.path.c_str());
        if(!handle)return FALSE;
        try{self.handles.push_back(handle);}catch(...){SDL_UnloadObject(handle);return FALSE;}return TRUE;
    }
    static void Release(void* context) noexcept {
        auto& self=*static_cast<ModuleLease*>(context);auto* handle=self.handles.back();
        self.handles.pop_back();SDL_UnloadObject(handle);
    }
};
// Hardware observer only. It is never the original DSP task handler or an AX
// callback; no task exists and no source bootstrap was substituted to create one.
void Observe(__OSInterrupt interrupt,OSContext* context) {
    Check(interrupt==__OS_INTERRUPT_DSP_DSP && std::this_thread::get_id()==owner,
          "actual hardware interrupt escaped the CPU owner/line");
    Check(context && OSGetCurrentContext() && !NativeInterruptsEnabled(),"actual interrupt context/exclusion absent");
    Check(!__DSP_curr_task && !DSPCheckInit(),"fixture must not create original task or initialization state");
    Check(ChargedDSPControlRead()==0x0180,"independent CPU CSR cause/mask word differs before acknowledgment");
    Check(DSPCheckMailFromDSP(),"IRQ observer got no actually executed firmware mail");
    Check(reinterpret_cast<std::uintptr_t>(DSPReadMailFromDSP())==0xdcd10000ULL,
          "source reader disagrees with executed firmware immediate");
    // Same hardware expression requested by original __DSPHandler. This only
    // tests the register; actual handler stays unentered without real boot/task.
    ChargedDSPControlWrite((ChargedDSPControlRead()&~(DSP_CSR_AIDINT|DSP_CSR_ARINT))|DSP_CSR_DSPINT);
    Check(ChargedDSPControlRead()==0x0100,"actual W1C acknowledgment changed mask or retained cause");
    ++observed;
}
void Run(int argc,char** argv) {
    Check(argc==3 && std::strlen(argv[2])==64,"fixture requires actual source image/hash identity");
    owner=std::this_thread::get_id();SDKLifetime sdk;
    const auto directory=std::filesystem::absolute("sdk-data").string();std::filesystem::create_directories(directory);
    AuroraConfig config{};config.appName="Actual DSP control qualifier";
    config.userPath=config.cachePath=directory.c_str();config.resourcesPath=".";
    config.desiredBackend=BACKEND_NULL;config.windowWidth=320;config.windowHeight=240;
    config.windowPosX=config.windowPosY=-1;config.mem1Size=MEM1_DEFAULT_SIZE;
    config.mem2Size=64u*1024u*1024u;config.logLevel=LOG_WARNING;
    Check(aurora_initialize(argc,argv,&config).window!=nullptr,"actual SDK window missing");sdk.live=true;OSInit();
    ModuleLease lease{std::filesystem::absolute(argv[1]).string(),{}};
    auto* initial=SDL_LoadObject(lease.path.c_str());Check(initial!=nullptr,"original data module failed to load");
    auto function=reinterpret_cast<OriginalDSPData(*)()>(SDL_LoadFunction(initial,"OriginalDSPDataForFixture"));
    Check(function!=nullptr,"genuine source data export absent");const auto data=function();
    Check(data.code_bytes==8192 && data.dram_bytes==64 && data.initial_vector==0x10,
          "original firmware/data/vector contract changed");
    OSNativeStaticMemoryOwner code_owner{argv[2],"axDspSlave",data.code,data.code_bytes,FALSE,
        &lease,ModuleLease::Retain,ModuleLease::Release};
    OSNativeStaticMemoryOwner dram_owner{argv[2],"__AXDramImage",data.dram,data.dram_bytes,TRUE,
        &lease,ModuleLease::Retain,ModuleLease::Release};
    const auto code=OSNativeRegisterStaticMemory(&code_owner),dram=OSNativeRegisterStaticMemory(&dram_owner);
    SDL_UnloadObject(initial);
    const auto memory=AttachNativeDSPMEM1();
    const auto code_pin=PinNativeDSPMemory(data.code,data.code_bytes,false),dram_pin=PinNativeDSPMemory(data.dram,data.dram_bytes,true);
    InitializeNativeInterruptController();const auto mailboxes=AttachNativeDSPMailboxes();
    Throws([&]{ChargedDSPControlRead();},"detached CPU read acquired fake register readiness");
    const auto control=AttachNativeDSPControl(mailboxes);
    Check(ChargedDSPControlRead()==0x0004,"explicit halted hardware context/mask fields changed");
    Check(AttachNativeDSPControl(mailboxes).generation==control.generation,"same lifetime lost idempotent attachment");
    Throws([&]{ShutdownNativeInterruptController();},"live physical mask owner was silently dropped");
    const auto previous=__OSSetInterruptHandler(__OS_INTERRUPT_DSP_DSP,Observe);
    Check(previous==nullptr && !__DSP_curr_task && !DSPCheckInit(),"unexpected fixture/source initial state");
    DSPInstructionCore core(mailboxes,control);
    core.LoadInstructionMemory(memory,code.physical_address,data.code_bytes,0);
    core.LoadDataMemory(memory,dram.physical_address,data.dram_bytes,0x0cd2);
    core.BeginExecution({data.initial_vector,0,0,{0,0},0});
    const auto before=core.Registers();Throws([&]{core.Step();},"HALT failed to gate real instruction execution");
    Check(core.Registers().pc==before.pc && core.Registers().instructions==before.instructions,"halted instruction advanced");
    // Diagnostic CPU clears HALT without claiming ROM boot. Source mask starts
    // disabled; the firmware must still execute on its actual device worker.
    ChargedDSPControlWrite(0);
    const BOOL enabled=OSDisableInterrupts();
    std::exception_ptr failure;
    std::thread device([&]{try{for(unsigned i=0;i<21;++i)core.Step();}catch(...){failure=std::current_exception();}});
    device.join();if(failure)std::rethrow_exception(failure);
    Check(core.Registers().pc==0x0030 && core.Registers().instructions==21,"actual firmware IFX instruction/order changed");
    Check(observed==0 && !__DSP_curr_task && !DSPCheckInit(),"device worker ran a source/fixture callback or initialized DSP");
    Check(ChargedDSPControlRead()==0x0080,"masked actual IFX request lost its latched CSR cause");
    Check(GetNativeInterruptControllerStatus().pending_mask==0,"physically masked DSP request reached CPU level");
    Check(!ServiceNativeInterruptController(),"masked/disabled CPU delivered an IRQ");
    constexpr u32 line=0x01000000;
    __OSUnmaskInterrupts(line);
    Check(ChargedDSPControlRead()==0x0180 && GetNativeInterruptControllerStatus().pending_mask==line,
          "actual OS unmask did not mirror source CSR enable/retained cause");
    Check(!ServiceNativeInterruptController() && observed==0,"global source critical section delivered device callback");
    __OSMaskInterrupts(line);
    Check(ChargedDSPControlRead()==0x0080 && GetNativeInterruptControllerStatus().pending_mask==0,
          "actual OS mask cleared cause or failed to deassert physical level");
    __OSUnmaskInterrupts(line);OSRestoreInterrupts(enabled);
    Check(ServiceNativeInterruptController() && observed==1,"owner did not receive actual instruction-generated IRQ");
    Check(!ServiceNativeInterruptController() && !DSPCheckMailFromDSP(),"acknowledged level/mail was redelivered");
    Check(GetNativeInterruptControllerStatus().pending_mask==0,"source-style CSR acknowledgment did not reach OS line");

    // Independent source OS DSP-group mapping includes all three enable fields.
    __OSUnmaskInterrupts(0x06000000);
    Check(ChargedDSPControlRead()==0x0150,"OS group mapping differs from source bits4/6/8");
    __OSMaskInterrupts(0x04000000);
    Check(ChargedDSPControlRead()==0x0140,"OS AI mask affected wrong physical field");
    const auto old_current=OSSetInterruptMask(0x02000000);
    Check(ChargedDSPControlRead()==0x0100,"current source mask failed to gate ARAM enable field");
    OSSetInterruptMask(old_current);
    Check(ChargedDSPControlRead()==0x0140,"current mask restore lost native hardware fields");
    __OSMaskInterrupts(0x02000000);
    Check(ChargedDSPControlRead()==0x0100,"original group mask restoration changed DSP enable");
    DSPBackendWriteInterruptRequest(control,1);
    Check(ChargedDSPControlRead()==0x0180,"real device request did not latch after acknowledgment");
    DSPBackendWriteInterruptRequest(control,0);
    Check(ChargedDSPControlRead()==0x0180,"zero IFX write invented a CPU acknowledgment");
    Throws([&]{DSPBackendWriteInterruptRequest(control,2);},"unknown IFX request bits silently succeeded");
    DSPAssertInt();
    Check(ChargedDSPControlRead()==0x0182 && NativeInterruptsEnabled(),
          "original DSPAssertInt failed preserved cause/opposite request/critical restore");
    Throws([&]{core.Step();},"unsupported DSP external-vector stack execution silently advanced");
    Check(core.Registers().pc==0x30 && core.Registers().instructions==21,"unsupported external interrupt advanced firmware");
    ChargedDSPControlWrite(0x0100|0x0080|0x0004);
    Check(ChargedDSPControlRead()==0x0104 && GetNativeInterruptControllerStatus().pending_mask==0,
          "CPU ack/HALT retained opposite IRQ or changed enable");
    Throws([&]{core.Step();},"native HALT failed after real firmware request");
    for(auto word:std::array<std::uint16_t,3>{0x0101,0x0904,0x0304}) {
        Throws([&]{ChargedDSPControlWrite(word);},"unsupported reset/bootstrap/DMA control returned success");
        Check(ChargedDSPControlRead()==0x0104,"failed bootstrap/control request changed physical word");
    }
    std::thread nonowner([&]{
        Throws([&]{ChargedDSPControlWrite(0);},"foreign CPU wrote native CSR");
        Throws([&]{ServiceNativeInterruptController();},"device worker executed CPU handler");
    });nonowner.join();

    // Real complete original DSPInit is reached, including version/handler/OS
    // mask requests. Its first actual ROM-bootstrap write stops before its
    // init flag or task state assignments. No AX predecessor is substituted.
    Throws([&]{DSPInit();},"original DSPInit acquired fake ROM/control readiness");
    Check(!DSPCheckInit() && !__DSP_curr_task && !__DSP_first_task && !__DSP_last_task,
          "unsupported bootstrap fabricated original initialization/task state");
    Check(__OSGetInterruptHandler(__OS_INTERRUPT_DSP_DSP)==__DSPHandler,
          "source handler registration request was replaced or bypassed");
    Check(!NativeInterruptsEnabled(),"fixture silently repaired interrupted original critical-section state");
    // Fixture-only recovery after the deliberate unsupported exception. This
    // is not a proposed game error path, source cleanup or bootstrap success.
    OSRestoreInterrupts(TRUE);
    __OSSetInterruptHandler(__OS_INTERRUPT_DSP_DSP,previous);
    DetachNativeDSPControl();
    Throws([&]{DSPBackendWriteInterruptRequest(control,1);},"drained generation still raised hardware IRQ");
    Throws([&]{core.Step();},"instruction context executed after its physical control lifetime ended");
    Check(!DSPCheckInit() && GetNativeInterruptControllerStatus().pending_mask==0,"drain invented source readiness or retained line");
    DetachNativeDSPMailboxes();ShutdownNativeInterruptController();
    ReleaseNativeDSPMemory(dram_pin);ReleaseNativeDSPMemory(code_pin);
    OSNativeReleaseStaticMemory(dram);OSNativeReleaseStaticMemory(code);
    Check(lease.handles.empty(),"actual firmware/static data owner remained after device drain");
    DetachNativeDSPMEM1();
    std::cout<<"original_ax_irq: 21 real firmware instructions -> latched CSR -> actual owner IRQ; original DSPInit held at bootstrap\n";
}
}
int main(int argc,char** argv) {
    try{Run(argc,argv);std::cout<<"native_dsp_control: "<<checks<<" checks passed\n";return 0;}
    catch(const std::exception& failure){std::cerr<<"native_dsp_control: "<<failure.what()<<"\n";return 1;}
}
