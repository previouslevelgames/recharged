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
constexpr std::uint16_t HardwareMasks=0x0150;
struct Control {
    std::mutex mutex;
    bool connected{};
    std::thread::id owner{};
    std::uint64_t generation{};
    std::uint16_t csr{};
    NativeDSPMailboxEndpoint mailboxes{};
    NativeInterruptSource interrupt{};
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
    NativeInterruptGuard exclusion;auto& state=State();std::lock_guard lock(state.mutex);RequireCPU(state);
    if (value&0x0801)throw std::logic_error("native DSP reset/ROM bootstrap execution is unsupported");
    if (value&~std::uint16_t(0x01fe))
        throw std::logic_error("native DSP DMA/ROM/status control bits are unsupported");
    // DSPINT is W1C. Other device causes are inactive in this bounded DSP-only
    // endpoint; AI/ARAM DMA cause transport is a separate unqualified boundary.
    auto next=static_cast<std::uint16_t>((value&(HardwareMasks|Halt|PiInterrupt))|(state.csr&DspInterrupt));
    if (value&DspInterrupt)next=static_cast<std::uint16_t>(next&~DspInterrupt);
    Publish(state,next);
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
    RequireCPU(state);DetachNativeDSPMaskObserver(ApplyOSMask,&state);
    if (!SetNativeInterruptPending(state.interrupt,false))
        throw std::logic_error("native DSP controller disappeared before hardware drain");
    state.connected=false;state.owner={};state.csr=0;
}
NativeDSPControlStatus GetNativeDSPControlStatus() {
    NativeInterruptGuard exclusion;auto& state=State();std::lock_guard lock(state.mutex);
    return {state.connected,state.csr,state.generation};
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
} // namespace mscharged::platform
