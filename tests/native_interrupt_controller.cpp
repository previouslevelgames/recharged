#include "platform/interrupt_controller.h"
#include "platform/interrupts.h"
#include <dolphin/os.h>
#include <dolphin/os/OSContext.h>
#include <dolphin/os/OSTime.h>
#include <array>
#include <atomic>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {
using namespace mscharged::platform;
unsigned checks;
std::thread::id owner;
OSContext* expected_interrupted;
std::array<NativeInterruptSource,32> lines;
std::vector<int> delivered;
bool acknowledge = true;

void Check(bool value, const char* message) {
    ++checks;
    if (!value) throw std::runtime_error(message);
}
template<class F> void Throws(F function, const char* message) {
    bool failed = false;
    try { function(); } catch (const std::exception&) { failed = true; }
    Check(failed,message);
}
void Handler(__OSInterrupt interrupt, OSContext* context) {
    Check(std::this_thread::get_id() == owner,"worker executed source IRQ handler");
    Check(!NativeInterruptsEnabled(),"source IRQ entered with CPU interrupts enabled");
    Check(context == expected_interrupted,"wrong interrupted context supplied to source handler");
    Check(OSGetCurrentContext() != context,"source IRQ did not receive temporary context");
    Check(!ServiceNativeInterruptController(),"masked callback reentered IRQ controller");
    delivered.push_back(interrupt);
    if (acknowledge) Check(SetNativeInterruptPending(lines[interrupt],false),"device ack was rejected");
}
void Replacement(__OSInterrupt interrupt, OSContext* context) { Handler(interrupt,context); }
void Failure(__OSInterrupt interrupt, OSContext* context) {
    Handler(interrupt,context);
    throw std::runtime_error("intentional source callback exception");
}
void Nested(__OSInterrupt interrupt, OSContext* context) {
    Handler(interrupt,context);
    const auto prior_context = expected_interrupted;
    expected_interrupted = OSGetCurrentContext();
    const BOOL previous = OSEnableInterrupts();
    Check(previous == FALSE,"nested IRQ enable did not return previous CPU mask");
    Check(SetNativeInterruptPending(lines[24],true),"nested real device fixture latch failed");
    Check(ServiceNativeInterruptController(),"explicitly enabled different IRQ did not run");
    OSRestoreInterrupts(previous);
    expected_interrupted = prior_context;
    Check(!NativeInterruptsEnabled(),"nested IRQ did not restore outer CPU mask");
    Check(GetNativeInterruptControllerStatus().dispatch_depth == 1,"nested IRQ depth did not unwind");
    Throws([] { ShutdownNativeInterruptController(); },"active source IRQ shutdown succeeded");
}

void Run() {
    owner=std::this_thread::get_id();
    Check(!GetNativeInterruptControllerStatus().initialized,"controller existed before host setup");
    Check(!ServiceNativeInterruptController(),"uninitialized host controller dispatched");
    Throws([] { __OSSetInterruptHandler(7,Handler); },"SDK registration invented initialized controller");
    InitializeNativeInterruptController();
    auto status=GetNativeInterruptControllerStatus();
    Check(status.initialized && status.owner_thread,"host controller did not initialize on owner");
    Check(status.user_mask==0xfffffff0u && status.current_mask==0 && status.pending_mask==0,
          "Wii0..27 initial interrupt mask changed");
    for (int i=0;i<32;++i) {
        lines[i]=GetNativeInterruptSource(i);
        Check(__OSGetInterruptHandler(i)==nullptr,"source IRQ table was not empty");
        Check(__OSSetInterruptHandler(i,Handler)==nullptr,"first source IRQ handler returned nonnull predecessor");
        Check(__OSGetInterruptHandler(i)==Handler,"registered source IRQ handler was not retained");
    }
    InitializeNativeInterruptController();
    Check(GetNativeInterruptControllerStatus().generation==status.generation,"idempotent SDK setup reset generation");
    Check(__OSGetInterruptHandler(7)==Handler,"idempotent SDK setup reset source handler");
    Check(__OSSetInterruptHandler(7,Replacement)==Handler,"replacement lost exact previous source handler");
    Check(__OSSetInterruptHandler(7,Handler)==Replacement,"restoration lost replacement handler");
    Throws([] { __OSSetInterruptHandler(-1,Handler); },"negative source IRQ index accepted");
    Throws([] { GetNativeInterruptSource(32); },"past-table native line accepted");

    OSContext interrupted{};
    std::memset(&interrupted,0xa5,sizeof(interrupted));
    const u32 address=0x12345678;
    std::memcpy(reinterpret_cast<u8*>(&interrupted)+OS_CONTEXT_SRR0,&address,sizeof(address));
    OSContext saved=interrupted;
    OSContext* original=OSGetCurrentContext();
    OSSetCurrentContext(&interrupted);
    expected_interrupted=&interrupted;
    const u32 dsp = 0x01000000; // actual Wii DSP_DSP index7, authored mask oracle
    Check(__OSUnmaskInterrupts(dsp)==0xfffffff0u,"unmask return did not preserve previous user mask");
    Check(SetNativeInterruptPending(lines[7],true),"actual device fixture did not latch DSP line");
    const BOOL old=OSDisableInterrupts();
    Check(old==TRUE && OSDisableInterrupts()==FALSE,"CPU interrupt masks lost nested previous status");
    Check(!ServiceNativeInterruptController(),"CPU-masked interrupt was dispatched");
    Check(GetNativeInterruptControllerStatus().pending_mask==dsp,"CPU masking consumed pending hardware level");
    Check(__OSMaskInterrupts(dsp)==(0xfffffff0u & ~dsp),"masked user-mask previous state wrong");
    Check(!NativeInterruptsEnabled(),"SDK mask API enabled a previously masked CPU");
    OSRestoreInterrupts(old);
    Check(!ServiceNativeInterruptController(),"user-masked interrupt was dispatched");
    Check(__OSUnmaskInterrupts(dsp)==0xfffffff0u,"second unmask previous state wrong");
    Check(OSGetInterruptMask()==0 && OSSetInterruptMask(dsp)==0,"current mask previous state wrong");
    Check(!ServiceNativeInterruptController(),"current mask was not combined with user mask");
    Check(OSSetInterruptMask(0)==dsp,"current mask restore lost previous state");
    const auto before=OSGetTime();
    Check(ServiceNativeInterruptController(),"real fixture level did not invoke registered handler");
    const auto after=OSGetTime();
    Check(__OSLastInterrupt==7 && __OSLastInterruptSrr0==address,"source last-IRQ identity/address wrong");
    Check(__OSLastInterruptTime>=before && __OSLastInterruptTime<=after,"IRQ timestamp did not use genuine SDK clock");
    Check(OSGetCurrentContext()==&interrupted && NativeInterruptsEnabled(),"interrupt exit did not restore source context/mask");
    Check(std::memcmp(&interrupted,&saved,sizeof(saved))==0,"native dispatch rewrote interrupted PPC/context bytes");

    // Device latches are levels: delivery without a hardware ack retains the
    // cause, including the original repeated-delivery behavior.
    acknowledge=false;
    SetNativeInterruptPending(lines[7],true);
    const auto previous_count=delivered.size();
    Check(ServiceNativeInterruptController() && ServiceNativeInterruptController(),"asserted device level was consumed by dispatch");
    Check(delivered.size()==previous_count+2,"level-pending repeated delivery changed");
    Check(GetNativeInterruptControllerStatus().pending_mask==dsp,"unacknowledged device line was cleared");
    SetNativeInterruptPending(lines[7],false); acknowledge=true;

    // Independent fully authored total order derived from retail priority
    // meanings; it does not run the provider's grouping/selection algorithm.
    constexpr int expected_order[]{23,25,0,1,2,3,4,22,24,27,18,19,26,6,7,8,9,10,
                                   11,12,13,14,15,16,20,21,5,17,28,29,30,31};
    delivered.clear();
    __OSUnmaskInterrupts(0xffffffffu);
    for (int i=0;i<32;++i) SetNativeInterruptPending(lines[i],true);
    for (int expected:expected_order) {
        const auto old_interrupt=__OSLastInterrupt;
        const auto old_time=__OSLastInterruptTime;
        const auto old_address=__OSLastInterruptSrr0;
        Check(ServiceNativeInterruptController(),"priority oracle pending line was not dispatched");
        Check(delivered.back()==expected,"native IRQ priority differs from authored retail order");
        if (expected<=4) {
            Check(__OSLastInterrupt==old_interrupt && __OSLastInterruptTime==old_time &&
                  __OSLastInterruptSrr0==old_address,"memory IRQ changed retail-excluded last-interrupt metadata");
        }
    }
    Check(GetNativeInterruptControllerStatus().pending_mask==0,"priority oracle did not acknowledge all source levels");
    Check(!ServiceNativeInterruptController(),"empty IRQ controller dispatched an event");

    // Missing selected handlers do not fall through to a lower-priority source
    // interrupt, matching the actual Wii __OSDispatchInterrupt selection.
    __OSSetInterruptHandler(23,nullptr);
    SetNativeInterruptPending(lines[23],true);SetNativeInterruptPending(lines[7],true);
    Check(!ServiceNativeInterruptController(),"missing highest IRQ handler selected a lower source handler");
    Check(GetNativeInterruptControllerStatus().pending_mask==(0x100u|dsp),"missing handler consumed an asserted level");
    __OSSetInterruptHandler(23,Handler);
    Check(ServiceNativeInterruptController() && delivered.back()==23,"restored highest-priority source handler was bypassed");
    Check(ServiceNativeInterruptController() && delivered.back()==7,"remaining source interrupt was lost");

    // Workers only latch real device state while the source CPU is masked.
    const BOOL prior=OSDisableInterrupts();
    std::atomic<bool> worker_raised{};
    std::thread worker([&] { worker_raised=SetNativeInterruptPending(lines[7],true); });
    worker.join();
    Check(worker_raised,"device worker blocked on source exclusion while latching");
    Check(!ServiceNativeInterruptController(),"device worker caused masked source delivery");
    OSRestoreInterrupts(prior);
    std::atomic<bool> rejected_service{},rejected_registration{};
    std::thread foreign([&] {
        try { ServiceNativeInterruptController(); } catch(const std::logic_error&) { rejected_service=true; }
        try { __OSSetInterruptHandler(7,Replacement); } catch(const std::logic_error&) { rejected_registration=true; }
    });
    foreign.join();
    Check(rejected_service && rejected_registration,"foreign thread executed SDK source operations");
    Check(ServiceNativeInterruptController(),"owner did not deliver worker-latched source interrupt");

    delivered.clear();
    __OSSetInterruptHandler(7,Nested);SetNativeInterruptPending(lines[7],true);
    Check(ServiceNativeInterruptController(),"nested fixture source handler did not execute");
    Check(delivered.size()==2 && delivered[0]==7 && delivered[1]==24,"explicit source IRQ nesting order changed");
    Check(GetNativeInterruptControllerStatus().dispatch_depth==0,"nested source IRQ depth leaked");
    Check(OSGetCurrentContext()==&interrupted && NativeInterruptsEnabled(),"nested source IRQ context/mask leaked");

    __OSSetInterruptHandler(7,Failure);SetNativeInterruptPending(lines[7],true);
    Throws([] { ServiceNativeInterruptController(); },"source IRQ exception was swallowed");
    Check(OSGetCurrentContext()==&interrupted && NativeInterruptsEnabled(),"exception did not restore source context/mask");
    Check(GetNativeInterruptControllerStatus().dispatch_depth==0,"exception leaked active IRQ depth");
    Check(GetNativeInterruptControllerStatus().pending_mask==0,"exception invented pending IRQ after genuine ack");
    __OSSetInterruptHandler(7,Handler);

    OSSetCurrentContext(nullptr);SetNativeInterruptPending(lines[7],true);
    Throws([] { ServiceNativeInterruptController(); },"missing native SDK context invented a successful interrupt");
    OSSetCurrentContext(&interrupted);
    Check(GetNativeInterruptControllerStatus().dispatch_depth==0,"invalid native context leaked IRQ depth");
    Check(ServiceNativeInterruptController(),"retained device cause lost after invalid native context");
    OSSetCurrentContext(original);
    expected_interrupted=original;

    const auto old_line=lines[7];
    const auto old_generation=GetNativeInterruptControllerStatus().generation;
    ShutdownNativeInterruptController();
    Check(!SetNativeInterruptPending(old_line,true),"closed controller accepted an old device callback");
    Check(!ServiceNativeInterruptController(),"closed host controller dispatched");
    InitializeNativeInterruptController();
    status=GetNativeInterruptControllerStatus();
    Check(status.generation>old_generation && status.pending_mask==0,"native controller restart retained old source level");
    Check(!SetNativeInterruptPending(old_line,true),"stale native device callback reached restarted controller");
    Check(__OSGetInterruptHandler(7)==nullptr,"restart retained disposed source handler");
    Check(__OSSetInterruptHandler(7,Handler)==nullptr,"new source handler received stale predecessor");
    lines[7]=GetNativeInterruptSource(7);
    __OSUnmaskInterrupts(dsp);SetNativeInterruptPending(lines[7],true);
    Check(ServiceNativeInterruptController(),"new native device lifetime did not dispatch");
    ShutdownNativeInterruptController();ShutdownNativeInterruptController();
}
}
int main() {
    try {
        Run();
        std::cout<<"Native IRQ controller hardware-injection fixture: "<<checks<<" checks; no DSP/AX/movie readiness\n";
        return 0;
    } catch(const std::exception& error) {
        std::cerr<<"Native IRQ controller fixture failed after "<<checks<<" checks: "<<error.what()<<"\n";
        return 1;
    }
}
