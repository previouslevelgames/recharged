#include "platform/dsp_control.h"
#include "platform/dsp_control_abi.h"
#include "platform/interrupt_controller.h"
#include "platform/interrupts.h"
#include <mutex>
#include <stdexcept>
#include <thread>

namespace {
using namespace mscharged::platform;
constexpr std::uint16_t DspInterrupt=0x0080,DspMask=0x0100,Halt=0x0004,PiInterrupt=0x0002;
constexpr std::uint16_t HardwareMasks=0x0150,AramInterrupt=0x0020,AramMask=0x0040;
constexpr std::uint16_t AramBusy=0x0200,BootstrapBusy=0x0400;
struct Control {
    std::mutex mutex;
    bool connected{};
    std::thread::id owner{};
    std::uint64_t generation{};
    std::uint16_t csr{};
    NativeDSPMailboxEndpoint mailboxes{};
    NativeInterruptSource interrupt{},aram_interrupt{};
    NativeDSPProcessorControl processor{};
};
Control& State() {static Control state;return state;}
void RequireCPU(const Control& state) {
    if (!state.connected || state.owner!=std::this_thread::get_id())
        throw std::logic_error("native DSP CSR access needs the attached CPU owner");
}
void RequireDevice(const Control& state,NativeDSPControlEndpoint endpoint) {
    if (!state.connected || state.generation!=endpoint.generation)
        throw std::logic_error("native DSP CSR device endpoint is stale or detached");
}
void Publish(Control& state,std::uint16_t next) {
    // Hardware latches its cause independently of enable. Only its enabled
    // level reaches the actual native OS owner controller; no callback here.
    if (!SetNativeInterruptPending(state.interrupt,(next&DspInterrupt) && (next&DspMask)))
        throw std::logic_error("native DSP CSR lost its actual interrupt controller");
    if (!SetNativeInterruptPending(state.aram_interrupt,(next&AramInterrupt) && (next&AramMask)))
        throw std::logic_error("native ARAM CSR lost its actual interrupt controller");
    state.csr=next;
}
void ApplyOSMask(u32 mask,void* context) {
    auto& state=*static_cast<Control*>(context);std::lock_guard lock(state.mutex);RequireCPU(state);
    auto next=static_cast<std::uint16_t>(state.csr&~HardwareMasks);
    if (!(mask&0x04000000u))next|=0x0010;
    if (!(mask&0x02000000u))next|=0x0040;
    if (!(mask&0x01000000u))next|=DspMask;
    Publish(state,next);
}
} // namespace

extern "C" std::uint16_t ChargedDSPControlRead() {
    NativeInterruptGuard exclusion;auto& state=State();std::lock_guard lock(state.mutex);RequireCPU(state);
    return state.csr;
}
extern "C" void ChargedDSPControlWrite(std::uint16_t value) {
    NativeInterruptGuard exclusion;auto& state=State();
    NativeDSPProcessorControl processor{};std::uint16_t previous{};
    {
        std::lock_guard lock(state.mutex);RequireCPU(state);
        processor=state.processor;previous=state.csr;
        if (!processor.context && (value&0x0801))
            throw std::logic_error("native DSP reset/ROM bootstrap execution is unsupported");
        const auto allowed=processor.context?std::uint16_t(0x09ff):std::uint16_t(0x01fe);
        if (value&~allowed)
            throw std::logic_error("native DSP DMA/ROM/status control bits are unsupported");
    }
    if (processor.context)processor.validate(processor.context,previous,value);
    {
        std::lock_guard lock(state.mutex);RequireCPU(state);
        // Reset is synchronous only when the attached real processor validates
        // and completes its reset operation below. Hardware RESET self-clears;
        // INIT remains a mode bit. Ordinary causes retain existing W1C semantics.
        auto next=static_cast<std::uint16_t>((value&(HardwareMasks|Halt|PiInterrupt|
            (processor.context?0x0800:0)))|(state.csr&(DspInterrupt|AramInterrupt|AramBusy|BootstrapBusy)));
        if ((value&DspInterrupt)||(value&1))next=static_cast<std::uint16_t>(next&~DspInterrupt);
        if ((value&AramInterrupt)||(value&1))next=static_cast<std::uint16_t>(next&~AramInterrupt);
        Publish(state,next);
    }
    if (processor.context)processor.apply(processor.context,previous,value);
}
namespace mscharged::platform {
NativeDSPControlEndpoint AttachNativeDSPControl(NativeDSPMailboxEndpoint mailboxes) {
    NativeInterruptGuard exclusion;auto& state=State();
    const auto wire=GetNativeDSPMailboxStatus();
    if (!wire.connected || wire.generation!=mailboxes.generation)
        throw std::logic_error("native DSP control requires the actual live mailbox endpoint");
    {
        std::lock_guard lock(state.mutex);
        if (state.connected) {
            RequireCPU(state);
            if (state.mailboxes.generation!=mailboxes.generation)
                throw std::logic_error("native DSP control is attached to another mailbox lifetime");
            return {state.generation};
        }
        state.interrupt=GetNativeInterruptSource(__OS_INTERRUPT_DSP_DSP);
        state.aram_interrupt=GetNativeInterruptSource(__OS_INTERRUPT_DSP_ARAM);
        state.csr=Halt;state.mailboxes=mailboxes;state.owner=std::this_thread::get_id();
        ++state.generation;state.connected=true;
    }
    try {AttachNativeDSPMaskObserver(ApplyOSMask,&state);}
    catch (...) {std::lock_guard lock(state.mutex);state.connected=false;state.owner={};throw;}
    return {state.generation};
}
void DetachNativeDSPControl() {
    NativeInterruptGuard exclusion;auto& state=State();std::lock_guard lock(state.mutex);
    if (!state.connected)return;
    RequireCPU(state);
    if(state.processor.context)throw std::logic_error("native DSP control still owns a live processor");
    DetachNativeDSPMaskObserver(ApplyOSMask,&state);
    if (!SetNativeInterruptPending(state.interrupt,false))
        throw std::logic_error("native DSP controller disappeared before hardware drain");
    if (!SetNativeInterruptPending(state.aram_interrupt,false))
        throw std::logic_error("native ARAM controller disappeared before hardware drain");
    state.connected=false;state.owner={};state.csr=0;
}
void AttachNativeDSPProcessorControl(NativeDSPControlEndpoint endpoint,NativeDSPProcessorControl processor) {
    NativeInterruptGuard exclusion;auto& state=State();std::lock_guard lock(state.mutex);
    RequireCPU(state);RequireDevice(state,endpoint);
    if(!processor.context||!processor.validate||!processor.apply||state.processor.context||!(state.csr&Halt))
        throw std::logic_error("native DSP processor needs a halted unique validated control owner");
    state.processor=processor;
}
void DetachNativeDSPProcessorControl(NativeDSPControlEndpoint endpoint,void* context) {
    NativeInterruptGuard exclusion;auto& state=State();std::lock_guard lock(state.mutex);
    RequireCPU(state);RequireDevice(state,endpoint);
    if(state.processor.context!=context||!(state.csr&Halt))
        throw std::logic_error("native DSP processor must halt/drain before detachment");
    state.processor={};
}
NativeDSPControlStatus GetNativeDSPControlStatus() {
    NativeInterruptGuard exclusion;auto& state=State();std::lock_guard lock(state.mutex);
    return {state.connected,state.csr,state.generation};
}
void DSPBackendBeginBootTransfer(NativeDSPControlEndpoint endpoint,bool instruction) {
    auto& state=State();std::lock_guard lock(state.mutex);RequireDevice(state,endpoint);
    const auto busy=instruction?BootstrapBusy:AramBusy;
    if(!(state.csr&Halt)||(state.csr&(AramBusy|BootstrapBusy)))
        throw std::logic_error("native boot DMA needs actual halted idle hardware");
    Publish(state,static_cast<std::uint16_t>(state.csr|busy));
}
void DSPBackendEndBootTransfer(NativeDSPControlEndpoint endpoint,bool instruction,bool completed) {
    auto& state=State();std::lock_guard lock(state.mutex);RequireDevice(state,endpoint);
    const auto busy=instruction?BootstrapBusy:AramBusy;
    if(!(state.csr&busy))throw std::logic_error("native boot DMA has no matching active transfer");
    auto next=static_cast<std::uint16_t>(state.csr&~busy);
    if(completed&&!instruction)next=static_cast<std::uint16_t>(next|AramInterrupt);
    Publish(state,next);
}
void DSPBackendWriteInterruptRequest(NativeDSPControlEndpoint endpoint,std::uint16_t value) {
    auto& state=State();std::lock_guard lock(state.mutex);RequireDevice(state,endpoint);
    if (value>1)throw std::logic_error("native DSP IFX IRQ operand bits are unsupported");
    if (value)Publish(state,static_cast<std::uint16_t>(state.csr|DspInterrupt));
}
void DSPBackendRequireInstructionExecution(NativeDSPControlEndpoint endpoint,std::uint16_t dsp_status) {
    auto& state=State();std::lock_guard lock(state.mutex);RequireDevice(state,endpoint);
    if (state.csr&Halt)throw std::logic_error("native DSP instruction execution is hardware halted");
    if ((state.csr&PiInterrupt) && (dsp_status&0x0800))
        throw std::logic_error("native DSP external interrupt vector/stack execution is unsupported");
}
void DSPBackendHaltExecution(NativeDSPControlEndpoint endpoint) {
    auto& state=State();std::lock_guard lock(state.mutex);RequireDevice(state,endpoint);
    Publish(state,static_cast<std::uint16_t>(state.csr|Halt));
}
} // namespace mscharged::platform
