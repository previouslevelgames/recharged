#include "platform/os_audio_boot_device.h"
#include "platform/os_audio_boot_abi.h"
#include "platform/dsp_control_abi.h"
#include "platform/dsp_mailbox_abi.h"
#include "platform/ai.h"
#include "platform/os_boot_environment.h"
#include "platform/interrupts.h"
#include <dolphin/os.h>
#include <dolphin/ai.h>
#include <stdexcept>
#include <thread>

namespace {
using namespace mscharged::platform;
NativeOSAudioBootDevice* current{};
NativeOSAudioBootDevice& Current() {
    if(!current)throw std::logic_error("original OS audio MMIO has no live native owner");
    return *current;
}
constexpr std::uint16_t Halt=4,Init=0x0800;
}
namespace mscharged::platform {
struct NativeOSAudioBootDevice::State {
    NativeOSBootEnvironmentLease environment;
    NativeDSPMemoryEndpoint memory;
    NativeDSPMailboxEndpoint mailboxes;
    NativeDSPControlEndpoint control;
    DSPInstructionCore& chip;
    NativeDSPBootMemory dma;
    NativeOSAudioGPIO gpio;
    DSPInstructionRegisters entry;
    std::thread::id owner=std::this_thread::get_id();
    NativeOSAudioBootPhase phase=NativeOSAudioBootPhase::Cold;
    std::uint64_t uploads{},aram{},steps{},gpio_writes{};
    std::uint16_t aram_size{};
    std::uint32_t main_address{},aram_address{};
    bool main_written{},aram_written{},attached{},servicing{};
    State(NativeDSPMemoryEndpoint mem,NativeDSPMailboxEndpoint mail,NativeDSPControlEndpoint csr,
          DSPInstructionCore& core,NativeOSAudioGPIO pins,const DSPInstructionRegisters& registers)
        :memory(mem),mailboxes(mail),control(csr),chip(core),dma(mem,csr,core),gpio(pins),entry(registers) {}
    void RequireOwner() const {
        if(owner!=std::this_thread::get_id()||phase==NativeOSAudioBootPhase::Retired)
            throw std::logic_error("native OS audio boot requires its live CPU owner");
        environment.RequireLive();chip.RequireHardwareEndpoints(mailboxes,control);
    }
    void Poll() {
        RequireOwner();
        if(servicing||phase!=NativeOSAudioBootPhase::Executing)return;
        servicing=true;
        try {
            // A hardware safe point advances instructions, never source game
            // time or callbacks. Both source clocks and original polls remain.
            for(unsigned n=0;n<256;++n) {
                if(GetNativeDSPControlStatus().csr&Halt) {
                    phase=NativeOSAudioBootPhase::Halted;break;
                }
                chip.Step();++steps;
            }
            if(GetNativeDSPControlStatus().csr&Halt)phase=NativeOSAudioBootPhase::Halted;
        } catch(...) {phase=NativeOSAudioBootPhase::Faulted;servicing=false;throw;}
        servicing=false;
    }
    static void Service(void* context) {static_cast<State*>(context)->Poll();}
    static void Validate(void* context,std::uint16_t previous,std::uint16_t request) {
        auto& s=*static_cast<State*>(context);s.RequireOwner();
        if(request&2)throw std::logic_error("OS boot PI interrupt execution is unsupported");
        if(request&1) {
            if(!(request&Halt)||(previous&0x0600)||!(request&Init))
                throw std::logic_error("OS boot reset requires HALT/RES and drained real DMA");
            DSPBackendValidateMemory(s.memory,0x01000000,1024,false);
        }
        if(!(request&Halt)&&(previous&Halt)) {
            if((request&Init)||s.phase!=NativeOSAudioBootPhase::ImageLoaded)
                throw std::logic_error("OS boot start lacks a reset-loaded program or requests an unsupported ROM loader");
        }
    }
    static void Apply(void* context,std::uint16_t,std::uint16_t request) {
        auto& s=*static_cast<State*>(context);s.RequireOwner();
        if(request&Halt)s.chip.PauseHaltedExecution();
        if(request&1) {
            DSPBackendBeginBootTransfer(s.control,true);
            try {s.dma.TransferBootstrapInstructions();}
            catch(...) {DSPBackendEndBootTransfer(s.control,true,false);s.phase=NativeOSAudioBootPhase::Faulted;throw;}
            DSPBackendEndBootTransfer(s.control,true,true);
            // Reset retains actual data and ROM banks. It does not implement
            // the chip's undocumented cold register image with guessed zeros.
            DSPBackendResetMailboxes(s.mailboxes);
            ++s.uploads;s.phase=NativeOSAudioBootPhase::ImageLoaded;
        } else if(!(request&Halt)) {
            s.chip.BeginExecution(s.entry);s.phase=NativeOSAudioBootPhase::Executing;
        }
    }
};
NativeOSAudioBootDevice::NativeOSAudioBootDevice(NativeDSPMemoryEndpoint memory,NativeDSPMailboxEndpoint mailboxes,
                                                NativeDSPControlEndpoint control,DSPInstructionCore& chip,
                                                NativeOSAudioGPIO gpio,const DSPInstructionRegisters& entry)
    :state_(std::make_unique<State>(memory,mailboxes,control,chip,gpio,entry)) {
    NativeInterruptGuard exclusion;auto& s=*state_;s.RequireOwner();
    if(current||entry.pc!=0||entry.instructions||entry.loop_depth||entry.stack[3])
        throw std::logic_error("OS audio boot needs a unique explicit reset-vector register image");
    chip.PauseHaltedExecution();
    AttachNativeDSPProcessorControl(control,{State::Validate,State::Apply,&s});
    try {AttachNativeDSPMailboxService(mailboxes,State::Service,&s);}
    catch(...) {DetachNativeDSPProcessorControl(control,&s);throw;}
    current=this;s.attached=true;
}
NativeOSAudioBootDevice::~NativeOSAudioBootDevice() {if(state_&&state_->attached)std::terminate();}
std::uint16_t NativeOSAudioBootDevice::ReadDSP(std::uint32_t reg) {
    auto& s=*state_;s.RequireOwner();
    switch(reg) {
    case 0:return ChargedDSPMailToHigh();
    case 2:return ChargedDSPMailFromHigh();
    case 3:return ChargedDSPMailFromLow();
    case 5:s.Poll();return ChargedDSPControlRead();
    case 9:return s.aram_size;
    case 27:{const auto ai=GetNativeAIStatus();return static_cast<std::uint16_t>((ai.dma_bytes/32)&0x7fff)|(ai.running?0x8000:0);}
    default:throw std::logic_error("original OS audio requested an unsupported DSP register read");
    }
}
void NativeOSAudioBootDevice::WriteDSP(std::uint32_t reg,std::uint16_t value) {
    auto& s=*state_;s.RequireOwner();
    switch(reg) {
    case 0:ChargedDSPMailToWriteHigh(value);return;
    case 5:ChargedDSPControlWrite(value);return;
    case 9:
        if(value!=0x43||!(GetNativeDSPControlStatus().csr&Halt))
            throw std::logic_error("OS boot ARAM size/mode is unsupported outside source halted request43");
        s.aram_size=value;return;
    case 27:{
        const auto ai=GetNativeAIStatus();
        if((value&0x8000)||(value&0x7fff)!=((ai.dma_bytes/32)&0x7fff))
            throw std::logic_error("OS stop requested unsupported AI DMA register changes");
        if(ai.running)AIStopDMA();return;
    }
    default:throw std::logic_error("original OS audio requested an unsupported DSP register write");
    }
}
std::uint32_t NativeOSAudioBootDevice::ReadDSPPair(std::uint32_t reg) {
    state_->RequireOwner();
    if(reg!=2)throw std::logic_error("OS audio mailbox pair read is unsupported");
    const auto high=ReadDSP(reg);const auto low=ReadDSP(reg+1);
    return (std::uint32_t(high)<<16)|low;
}
void NativeOSAudioBootDevice::WriteDSPPair(std::uint32_t reg,std::uint32_t value) {
    auto& s=*state_;s.RequireOwner();
    if(!(GetNativeDSPControlStatus().csr&Halt))
        throw std::logic_error("OS boot ARAM registers require actual halted hardware");
    switch(reg) {
    case 16:if(value!=0x01000000)throw std::logic_error("OS boot main-memory address is unsupported");
        s.main_address=value;s.main_written=true;return;
    case 18:if(value)throw std::logic_error("OS boot ARAM destination is unsupported");
        s.aram_address=value;s.aram_written=true;return;
    case 20:
        if(!s.main_written||!s.aram_written||s.aram_size!=0x43||value!=32)
            throw std::logic_error("OS boot ARAM submission lacks its exact source register values");
        DSPBackendValidateMemory(s.memory,s.main_address,value,false);
        DSPBackendBeginBootTransfer(s.control,false);
        try {s.dma.TransferARAM(s.main_address,s.aram_address,value);}
        catch(...) {DSPBackendEndBootTransfer(s.control,false,false);throw;}
        DSPBackendEndBootTransfer(s.control,false,true);++s.aram;return;
    default:throw std::logic_error("OS audio packed register write is unsupported");
    }
}
std::uint32_t NativeOSAudioBootDevice::ReadIPC(std::uint32_t reg) {
    state_->RequireOwner();
    switch(reg){case 0x60:return state_->gpio.diflags;case 0x73:return state_->gpio.direction;case 0x74:return state_->gpio.input;}
    throw std::logic_error("OS audio GPIO register read is unsupported");
}
void NativeOSAudioBootDevice::WriteIPC(std::uint32_t reg,std::uint32_t value) {
    auto& s=*state_;s.RequireOwner();
    switch(reg){case 0x60:s.gpio.diflags=value;break;case 0x73:s.gpio.direction=value;break;case 0x74:s.gpio.input=value;break;
    default:throw std::logic_error("OS audio GPIO register write is unsupported");}
    // Native virtual GPIO latches only. Original OS tick waits choose the clock
    // sequencing; these bits do not assert audio hardware/game initialization.
    ++s.gpio_writes;
}
void* NativeOSAudioBootDevice::WorkMemory() const {
    state_->RequireOwner();auto* address=OSPhysicalToCached(0x01000000);
    if(!address||OSCachedToPhysical(address)!=0x01000000||OSGetPhysicalMemSize()<0x01000400)
        throw std::logic_error("original OS audio work memory has no actual fixed MEM1 backing");
    return address;
}
NativeOSAudioBootStatus NativeOSAudioBootDevice::Status() const {
    const auto& s=*state_;s.RequireOwner();return {s.phase,s.uploads,s.aram,s.steps,s.gpio_writes,s.gpio,s.aram_size};
}
void NativeOSAudioBootDevice::Close() {
    NativeInterruptGuard exclusion;auto& s=*state_;s.RequireOwner();
    if(!(GetNativeDSPControlStatus().csr&Halt)||s.servicing||current!=this)
        throw std::logic_error("OS audio boot hardware must halt/drain before retirement");
    s.chip.PauseHaltedExecution();DetachNativeDSPMailboxService(s.mailboxes,&s);
    DetachNativeDSPProcessorControl(s.control,&s);current=nullptr;s.attached=false;s.phase=NativeOSAudioBootPhase::Retired;s.environment.Close();
}
} // namespace mscharged::platform
extern "C" uint16_t ChargedOSAudioDSPRead(uint32_t reg) {return Current().ReadDSP(reg);}
extern "C" void ChargedOSAudioDSPWrite(uint32_t reg,uint16_t value) {Current().WriteDSP(reg,value);}
extern "C" uint32_t ChargedOSAudioDSPReadPair(uint32_t reg) {return Current().ReadDSPPair(reg);}
extern "C" void ChargedOSAudioDSPWritePair(uint32_t reg,uint32_t value) {Current().WriteDSPPair(reg,value);}
extern "C" uint32_t ChargedOSAudioIPCRead(uint32_t reg) {return Current().ReadIPC(reg);}
extern "C" void ChargedOSAudioIPCWrite(uint32_t reg,uint32_t value) {Current().WriteIPC(reg,value);}
extern "C" void* ChargedOSAudioWorkMemory() {return Current().WorkMemory();}
