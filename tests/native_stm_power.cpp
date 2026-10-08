#include "platform/instruction_cache.h"
#include "platform/interrupt_controller.h"
#include "platform/interrupts.h"
#include "platform/ios_device.h"
#include "platform/stm_device.h"
#include "platform/thread_registry_abi.h"
#include "platform/thread_queues.h"
#include "platform/video_device.h"
#include <aurora/video.h>
#include <revolution/os.h>
#include <revolution/ipc.h>
#include <dolphin/vi.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <thread>

using namespace mscharged::platform;
namespace {
unsigned checks{}, completions{}, verifications{};
std::thread::id owner;
OSContext* context;
OSThread* retained_thread{};
OSThread retained_image{};
std::atomic<unsigned> thread_entries{};

void Check(bool value, const char* message) {
    ++checks;
    if (!value) throw std::runtime_error(message);
}
bool Display() {
    bool enabled;
    Check(aurora_get_video_display_enabled(&enabled), "Actual VI ENB owner absent");
    return enabled;
}
void Quiescent(void* opaque, const NativeSTMPowerRequest& request) {
    Check(opaque == &checks && std::this_thread::get_id() == owner,
        "Power verification lost the actual native owner");
    Check(!NativeInterruptsEnabled() && NativeInterruptWaitAllowed()
        && !NativeInterruptDispatchActive() && OSGetCurrentContext() == context,
        "Source terminal wait ran inside a callback/guard or before masking");
    Check(!GetNativeIOSStatus().pending && !GetNativeIOSStatus().active && !completions,
        "An unretired source IOS receiver reached power removal");
    Check(!Display() && __OSGetInterruptHandler(__OS_INTERRUPT_PI_VI),
        "Source disabled display lost its real retained VI owner");
    Check(NativeInstructionCacheSequence() == request.instruction_cache_sequence + 1,
        "Original source cache operation did not precede its hardware wait");
    Check(request.generation == GetNativeSTMInput().generation
        && GetNativeSTMPowerRequest()->input == request.input,
        "Received request changed actual device/input ownership");
    for (auto byte : request.input)
        Check(!byte, "Cold original shutdown changed its source32-byte input");
    if (retained_thread) {
        const auto terminal=ValidateNativeThreadsForPowerRemoval();
        Check(terminal.completed_workers==1&&terminal.retained_moribund_threads==1,
            "Actual completed source descriptor was not retained through terminal power");
        Check(std::memcmp(retained_thread,&retained_image,sizeof(retained_image))==0,
            "Terminal power changed the original MORIBUND descriptor/list/result");
        Check(thread_entries==0||thread_entries==1,"Terminal power repeated a source worker entry");
    }
    // This leaf owns only the real CPU VI/IRQ and STM/IOS endpoints. No audio,
    // DVD, renderer or game owner exists here; their full-drain predicates must
    // be provided by the production owner before any ResetTask admission.
    ++verifications;
    std::printf("Native STM source power removal PASS: %u checks, one genuine0x2003 request, original cache/mask order, CPU owners quiescent; no full OSShutdown/ResetTask or physical black surface claim.\n", checks);
    std::fflush(nullptr);
}
s32 Pending(s32, void*) { ++completions; return IPC_RESULT_OK; }
void* NeverEntered(void*) {
    throw std::logic_error("Parked source worker entry must not run in this leaf");
}
void* Returned(void* value) { ++thread_entries;return value; }
void Run(const std::string& mode) {
    owner = std::this_thread::get_id();
    context = OSGetCurrentContext();
    (void)OSGetCurrentThread(); // Real default SDK caller exists before borrowed worker records.
    InitializeNativeInterruptController();
    ConfigureNativeVideoHardware(VI_TVMODE_NTSC_INT, false);
    VIInit(); // Actual CPU VI owner, with no GPU/output endpoint installed.
    InitializeNativeSTMDevice();
    Check(__OSInitSTM(), "Whole original STM initialization failed");
    Check(Display(), "Actual original cached VIInit ENB absent");
    Check(__OSUnRegisterStateEvent() == 0, "Actual source event receiver did not unregister");
    Check(!GetNativeSTMPowerRequest() && !NativeInstructionCacheSequence(),
        "Cold device invented an accepted request/cache operation");

    if (mode != "unconfigured") ConfigureNativeSTMPowerRemoval({&checks, Quiescent});
    alignas(32) static std::array<unsigned char, 4096> stack;
    static OSThread parked;
    if (mode == "pending") {
        Check(IOS_OpenAsync("/dev/stm/immediate", IPC_OPEN_NONE, Pending, &checks) == 0
            && GetNativeIOSStatus().pending == 1 && !completions,
            "Actual asynchronous predecessor lost its retained source receiver");
    } else if (mode == "thread" || mode == "thread-cancelled" || mode == "thread-completed") {
        Check(OSCreateThread(&parked, mode=="thread-completed"?Returned:NeverEntered, &checks,
            stack.data() + stack.size(),stack.size(), 10, 0), "Genuine SDK thread could not be constructed");
        if (mode=="thread-completed") {
            Check(OSResumeThread(&parked)==1,"Actual terminal source worker did not resume");
            const auto end=std::chrono::steady_clock::now()+std::chrono::seconds(2);
            while(!OSIsThreadTerminated(&parked)) {
                if(std::chrono::steady_clock::now()>=end)
                    throw std::runtime_error("Actual terminal source worker did not complete");
                std::this_thread::yield();
            }
            Check(thread_entries==1&&parked.val==&checks,"Original worker completion/result was not genuine");
        } else if (mode=="thread-cancelled") {
            OSCancelThread(&parked);
            Check(!thread_entries&&OSIsThreadTerminated(&parked),"Actual parked cancellation ran a source entry");
        }
        if (mode!="thread") {
            Check(parked.state==OS_THREAD_STATE_MORIBUND&&!parked.attr,
                "Native completion removed the original unjoined MORIBUND record");
            retained_thread=&parked;retained_image=parked;
        }
        const auto mask = OSDisableInterrupts();
        auto* queue = ChargedNativeActiveThreadQueue();
        Check(queue->head != queue->tail, "Parked source descriptor absent from sole SDK registry");
        OSRestoreInterrupts(mask);
    }
    const auto sequence = NativeInstructionCacheSequence();
    bool rejected{};
    try {
        if (mode == "irq") {
            Check(DispatchNativeInterrupt([] { __OSShutdownToSBY(); }),
                "Real IRQ dispatch did not enter original terminal method");
        } else if (mode == "restart") {
            __OSHotReset();
        } else {
            __OSShutdownToSBY();
        }
    } catch (const std::logic_error&) { rejected = true; }
    Check(mode != "power"&&mode!="thread-cancelled"&&mode!="thread-completed",
        "Configured genuine source power request unexpectedly returned");
    Check(rejected && !verifications && !completions,
        "Unsupported/unfinished terminal flow returned successful power removal");
    if (mode == "pending") {
        Check(GetNativeIOSStatus().pending == 1 && !GetNativeSTMPowerRequest()
            && NativeInstructionCacheSequence() == sequence && !Display(),
            "Queued source receiver was consumed, relabeled drained or allowed terminal receipt");
    } else if (mode == "irq") {
        Check(NativeInterruptsEnabled() && !GetNativeSTMPowerRequest()
            && NativeInstructionCacheSequence() == sequence && Display()
            && OSGetCurrentContext() == context,
            "Rejected IRQ power request changed caller/source/video state");
    } else {
        Check(!NativeInterruptsEnabled() && !Display()
            && NativeInstructionCacheSequence() == sequence + 1,
            "Unsupported terminal flow altered original cache/mask/display order");
        Check(bool(GetNativeSTMPowerRequest()) == (mode == "thread"),
            "Unconfigured/restart path invented an accepted power request");
    }
    std::printf("Native STM terminal negative PASS: %s, %u checks, no process removal/verification/callback; terminal ownership retained.\n",
        mode.c_str(), checks);
    std::fflush(nullptr);
    std::_Exit(0); // No later callback, source worker or module retirement.
}
}
int main(int argc, char** argv) {
    try {
        if (argc != 2) throw std::invalid_argument("Expected one terminal leaf mode");
        const std::string mode = argv[1];
        Check(mode == "power" || mode == "unconfigured" || mode == "pending"
            || mode == "thread" || mode == "thread-cancelled" || mode == "thread-completed"
            || mode == "irq" || mode == "restart", "Unknown terminal leaf mode");
        Run(mode);
    } catch (const std::exception& error) {
        std::fprintf(stderr, "STM source terminal boundary: %s (%u checks)\n", error.what(), checks);
        std::fflush(nullptr);
        std::_Exit(1);
    }
}
