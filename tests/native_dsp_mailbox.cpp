#include "platform/dsp_mailbox.h"
#include "platform/dsp_mailbox_abi.h"
#include "platform/interrupt_controller.h"
#include "platform/interrupts.h"
#include <dolphin/os.h>
extern "C" {
#include <revolution/dsp.h>
}
#include <array>
#include <atomic>
#include <chrono>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace {
using namespace mscharged::platform;
std::atomic<unsigned> checks{};
std::thread::id owner;
NativeDSPMailboxEndpoint device{};
OSContext* interrupted_context{};
u32 interrupt_word{};
unsigned deliveries{};
void Check(bool value, const char* message) {
    ++checks;
    if (!value) throw std::runtime_error(message);
}
template<class F> void Throws(F&& function, const char* message) {
    bool rejected=false;
    try { function(); } catch (const std::exception&) { rejected=true; }
    Check(rejected,message);
}
u32 MailWord(DSPMail word) { return static_cast<u32>(reinterpret_cast<std::uintptr_t>(word)); }
DSPMail Word(u32 word) { return reinterpret_cast<DSPMail>(static_cast<std::uintptr_t>(word)); }
void Publish(NativeDSPMailboxEndpoint endpoint,u32 value) {
    DSPBackendMailFromWriteHigh(endpoint,static_cast<u16>(value>>16));
    DSPBackendMailFromWriteLow(endpoint,static_cast<u16>(value));
}
void WireFixtureInterrupt(__OSInterrupt index,OSContext* context) {
    Check(index==7,"wrong original DSP IRQ index");
    Check(std::this_thread::get_id()==owner,"mailbox IRQ ran on device worker");
    Check(!NativeInterruptsEnabled(),"source callback ran with CPU interrupts enabled");
    Check(context==interrupted_context,"wrong interrupted context");
    Check(OSGetCurrentContext()!=context,"source callback lacks temporary native context");
    Check(DSPCheckMailFromDSP()==TRUE,"device IRQ arrived without its independently published mail");
    interrupt_word=MailWord(DSPReadMailFromDSP());
    Check(DSPCheckMailFromDSP()==FALSE,"source low-register read did not acknowledge device mail");
    Check(DSPBackendSetInterrupt(device,false),"device IRQ acknowledgement rejected");
    ++deliveries;
}
void Run() {
    owner=std::this_thread::get_id();
    Check(DSPCheckInit()==FALSE,"original DSP init flag changed before a device exists");
    Check(!GetNativeDSPMailboxStatus().connected,"mailboxes silently connected");
    Throws([] { DSPCheckMailToDSP(); },"disconnected CPU mailbox reported successful readiness");
    Throws([] { DSPSendMailToDSP(Word(0x1234)); },"disconnected CPU sent successful mail");
    Throws([] { AttachNativeDSPMailboxes(); },"wire attachment invented an IRQ controller");
    InitializeNativeInterruptController();
    device=AttachNativeDSPMailboxes();
    Check(AttachNativeDSPMailboxes().generation==device.generation,"idempotent attachment reset device wire");
    auto status=GetNativeDSPMailboxStatus();
    Check(status.connected && !status.cpu_mail_full && !status.dsp_mail_full,"wire attachment synthesized boot mail");
    Check(DSPCheckInit()==FALSE,"wire attachment called original DSPInit or fabricated its state");
    Check(DSPCheckMailToDSP()==FALSE && DSPCheckMailFromDSP()==FALSE,"empty transport reported full mail");
    Check(!ServiceNativeInterruptController(),"wire attachment injected an IRQ");
    Check(MailWord(DSPReadMailFromDSP())==0,"empty initial mail invented a boot response");

    // Independently authored register-level oracle. Bit31 is the mailbox-full
    // flag; the wire carries31 payload bits. No DSP_INIT/task-completion messages
    // or firmware implementation appear in this hardware fixture.
    struct Example { u32 input;u16 empty_high;u16 full_high;u16 low;u32 full_word;u32 empty_word; };
    constexpr Example examples[]{
        {0x00000000,0x0000,0x8000,0x0000,0x80000000,0x00000000},
        {0x00001234,0x0000,0x8000,0x1234,0x80001234,0x00001234},
        {0x12345678,0x1234,0x9234,0x5678,0x92345678,0x12345678},
        {0x7fffffff,0x7fff,0xffff,0xffff,0xffffffff,0x7fffffff},
        {0x80000000,0x0000,0x8000,0x0000,0x80000000,0x00000000},
        {0x89abcdef,0x09ab,0x89ab,0xcdef,0x89abcdef,0x09abcdef},
        {0xf1230042,0x7123,0xf123,0x0042,0xf1230042,0x71230042},
        {0xffffffff,0x7fff,0xffff,0xffff,0xffffffff,0x7fffffff}
    };
    for (const auto& e:examples) {
        ChargedDSPMailToWriteHigh(static_cast<u16>(e.input>>16));
        Check(DSPBackendMailToHigh(device)==e.empty_high,"CPU high write did not clear status/extract payload");
        Check(DSPCheckMailToDSP()==FALSE,"CPU high-only write published mail");
        ChargedDSPMailToWriteLow(e.low);
        Check(DSPCheckMailToDSP()==TRUE,"CPU low write failed to publish full mail");
        Check(DSPBackendMailToHigh(device)==e.full_high,"DSP high read lost CPU full/payload bits");
        Check(DSPCheckMailToDSP()==TRUE,"DSP high read acknowledged CPU mail prematurely");
        Check(DSPBackendMailToLow(device)==e.low,"DSP low read returned incorrect CPU payload");
        Check(DSPCheckMailToDSP()==FALSE,"DSP low read did not acknowledge CPU mail");
        Check(DSPBackendMailToHigh(device)==e.empty_high,"DSP acknowledgement erased retained CPU payload");
        Check(DSPBackendMailToLow(device)==e.low,"stale DSP low read lost retained payload");
        DSPSendMailToDSP(Word(e.input));
        Check(DSPBackendMailToHigh(device)==e.full_high && DSPBackendMailToLow(device)==e.low,
              "actual original SDK send differs from authored wire word");
        Check(DSPCheckMailToDSP()==FALSE,"actual original send acknowledgement failed");

        DSPBackendMailFromWriteHigh(device,static_cast<u16>(e.input>>16));
        Check(ChargedDSPMailFromHigh()==e.empty_high && DSPCheckMailFromDSP()==FALSE,
              "device high-only write published a response");
        DSPBackendMailFromWriteLow(device,e.low);
        Check(ChargedDSPMailFromHigh()==e.full_high && DSPCheckMailFromDSP()==TRUE,
              "CPU high read lost device flag/payload or acknowledged it");
        Check(MailWord(DSPReadMailFromDSP())==e.full_word,"actual original receive lost full bit/high-before-low order");
        Check(DSPCheckMailFromDSP()==FALSE,"actual original receive did not acknowledge device low register");
        Check(MailWord(DSPReadMailFromDSP())==e.empty_word,"empty read did not retain original payload with cleared status");
        Check(!ServiceNativeInterruptController(),"mail publication automatically fabricated a DSP interrupt");
    }
    // Retail register writes can overwrite a full mailbox; preserve this quirk.
    DSPSendMailToDSP(Word(0x13579bdf));
    DSPSendMailToDSP(Word(0x2468ace0));
    Check(DSPBackendMailToHigh(device)==0xa468 && DSPBackendMailToLow(device)==0xace0,
          "host added a queue or rejected original full-mail overwrite");
    Publish(device,0x12345678);DSPBackendMailFromWriteHigh(device,0x2345);
    Check(DSPCheckMailFromDSP()==FALSE && MailWord(DSPReadMailFromDSP())==0x23455678,
          "high overwrite failed to clear busy while preserving the other register");

    // An unrepresentable native pointer must fail before either register changes.
    DSPSendMailToDSP(Word(0x10203040));
    if (sizeof(std::uintptr_t)>sizeof(u32)) {
        Throws([] { DSPSendMailToDSP(reinterpret_cast<DSPMail>(std::uintptr_t{1}<<40)); },
               "native pointer was truncated into an invented DSP bus address");
        Check(DSPBackendMailToHigh(device)==0x9020 && DSPBackendMailToLow(device)==0x3040,
              "failed address conversion partially overwrote mailbox");
    }
    std::exception_ptr worker_failure;
    std::thread foreign([&] {
        try {
            Throws([] { DSPCheckMailToDSP(); },"foreign thread ran original CPU mailbox read");
            Throws([] { DetachNativeDSPMailboxes(); },"foreign thread detached the owner device");
            Check(DSPBackendMailToHigh(device)==0x1020,"legitimate device worker lost retained mail");
        } catch (...) { worker_failure=std::current_exception(); }
    });
    foreign.join();if (worker_failure) std::rethrow_exception(worker_failure);

    // Execute the literal source wait macro: only a real endpoint low read may
    // complete it. This worker is a hardware test fixture, not DSP firmware.
    constexpr u32 synchronous_words[]{0x11223344,0x55667788,0x01020304};
    std::atomic<unsigned> acknowledgements{};
    worker_failure={};
    std::thread consumer([&] {
        try {
            for (const auto value:synchronous_words) {
                const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(3);
                while ((DSPBackendMailToHigh(device)&0x8000)==0) {
                    if (std::chrono::steady_clock::now()>deadline) throw std::runtime_error("source send never reached device endpoint");
                    std::this_thread::yield();
                }
                const auto high=DSPBackendMailToHigh(device);
                Check(high==static_cast<u16>((value>>16)|0x8000),"source synchronous word high payload changed");
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
                Check(DSPBackendMailToLow(device)==static_cast<u16>(value),"source synchronous word low payload changed");
                ++acknowledgements;
            }
        } catch (...) { worker_failure=std::current_exception(); }
    });
    for (const auto value:synchronous_words) {
        DSP_SEND_MAIL_SYNC(value);
        Check(DSPCheckMailToDSP()==FALSE,"original synchronous wait bypassed device acknowledgement");
    }
    consumer.join();if (worker_failure) std::rethrow_exception(worker_failure);
    Check(acknowledgements==3,"source synchronous macro returned without all actual device acknowledgements");

    // IRQ cause is a separate level. Publication alone already proved inert.
    __OSSetInterruptHandler(7,WireFixtureInterrupt);
    OSContext context{};interrupted_context=&context;
    OSContext* previous_context=OSGetCurrentContext();OSSetCurrentContext(&context);
    worker_failure={};
    std::thread publisher([&] {
        try { Publish(device,0x89abcdef);Check(DSPBackendSetInterrupt(device,true),"device IRQ request rejected"); }
        catch (...) { worker_failure=std::current_exception(); }
    });
    publisher.join();if (worker_failure) std::rethrow_exception(worker_failure);
    Check(deliveries==0,"device worker executed the source IRQ handler");
    Check(!ServiceNativeInterruptController(),"source-masked DSP interrupt was delivered");
    __OSUnmaskInterrupts(0x01000000);
    const BOOL old=OSDisableInterrupts();
    Check(!ServiceNativeInterruptController(),"CPU-masked device IRQ was delivered");
    OSRestoreInterrupts(old);
    Check(ServiceNativeInterruptController() && deliveries==1 && interrupt_word==0x89abcdef,
          "owner did not deliver exact published/acknowledged wire word");
    Check(!ServiceNativeInterruptController(),"device acknowledgement left a pending IRQ");
    Check(OSGetCurrentContext()==&context && NativeInterruptsEnabled(),"callback did not restore context/exclusion");
    OSSetCurrentContext(previous_context);

    // Device detach drains real fixture workers above, clears its IRQ, and
    // rejects old generations without inventing a DSP or startup state.
    DSPBackendSetInterrupt(device,true);
    const auto old_device=device;DetachNativeDSPMailboxes();
    Check(!GetNativeDSPMailboxStatus().connected && GetNativeInterruptControllerStatus().pending_mask==0,
          "detach left a connected transport or asserted hardware IRQ");
    Throws([&] { DSPBackendMailToHigh(old_device); },"detached device retained mailbox ownership");
    Throws([&] { DSPBackendSetInterrupt(old_device,true); },"detached endpoint resurrected IRQ");
    device=AttachNativeDSPMailboxes();
    Check(device.generation!=old_device.generation,"reattach reused stale device identity");
    Throws([&] { DSPBackendMailFromWriteLow(old_device,1); },"old device published into new transport");
    Check(DSPCheckMailToDSP()==FALSE && DSPCheckMailFromDSP()==FALSE && DSPCheckInit()==FALSE,
          "reattach fabricated firmware/init mail or carried old full cells");
    DetachNativeDSPMailboxes();ShutdownNativeInterruptController();
    Check(DSPCheckInit()==FALSE,"hardware fixture changed genuine original DSP init flag");
}
}
int main() {
    try {
        Run();
        std::cout << "native DSP mailbox hardware fixture: " << checks << " checks; source DSPCheckInit remains false\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "DSP wire test failure: " << error.what() << '\n';
        return 1;
    }
}
