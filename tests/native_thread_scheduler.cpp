#include "platform/interrupts.h"
#include "platform/thread_queues.h"
#include <dolphin/os.h>
#include <dolphin/os/OSMessage.h>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <memory>
#include <stdexcept>
#include <thread>
#include <type_traits>

using namespace mscharged::platform;
using namespace std::chrono_literals;
namespace {
unsigned checks;
void Check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
    ++checks;
}
template<class Fn> void Reject(Fn fn, const char* message) {
    bool failed = false;
    try { fn(); } catch (const std::logic_error&) { failed = true; }
    Check(failed, message);
}
template<class Fn> void Until(Fn fn) {
    const auto end = std::chrono::steady_clock::now() + 2s;
    while (!fn()) {
        if (std::chrono::steady_clock::now() > end) throw std::runtime_error("actual scheduler gate timeout");
        std::this_thread::yield();
    }
}
struct Owned {
    OSThread sdk{};
    alignas(32) unsigned char stack[0x4000]{};
    void* Top() { return stack + sizeof(stack); }
};
struct Job {
    OSMessageQueue messages{};
    OSMessage cell{};
    std::atomic<bool> entered{}, returned{};
    void* payload{};
};
void* Receive(void* argument) {
    auto& job = *static_cast<Job*>(argument);
    job.entered = true;
    OSMessage value{};
    if (!OSReceiveMessage(&job.messages, &value, OS_MESSAGE_BLOCK) || value != job.payload)
        throw std::runtime_error("whole original message ring lost payload");
    job.returned = true;
    return value;
}
void Create(Owned& owner, Job& job, void* payload) {
    job.payload = payload;
    OSInitMessageQueue(&job.messages, &job.cell, 1);
    Check(OSCreateThread(&owner.sdk, Receive, &job, owner.Top(), sizeof(owner.stack), 14, 0),
          "actual native SDK create failed");
    Check(owner.sdk.suspend == 1 && owner.sdk.state == OS_THREAD_STATE_READY && !job.entered,
          "source entry ran before original resume");
}
bool Waiting(Owned& owner, Job& job) {
    const auto mask = OSDisableInterrupts();
    const bool waiting = job.messages.queueReceive.head == &owner.sdk &&
        job.messages.queueReceive.tail == &owner.sdk && owner.sdk.state == OS_THREAD_STATE_WAITING;
    OSRestoreInterrupts(mask);
    return waiting;
}
void Join(Owned& owner, Job& job) {
    void* value{};
    Check(OSJoinThread(&owner.sdk, &value) && value == job.payload && job.returned,
          "genuine attached join lost returned pointer");
    Check(owner.sdk.state == 0, "original joined state did not retire");
    DrainNativeThreadLifetimes();
}
Job* irq_job{};
void WakeIRQ() {
    Check(NativeInterruptDispatchActive() && !NativeInterruptsEnabled(), "wake escaped real IRQ context");
    Check(OSDisableScheduler() == 1, "nested IRQ disable prior count changed");
    Check(OSSendMessage(&irq_job->messages, irq_job->payload, 0), "original IRQ message send failed");
    Check(OSEnableScheduler() == 2 && !NativeInterruptsEnabled(), "nested IRQ enable altered caller mask/count");
}
struct Running {
    std::atomic<bool> entered{}, release{};
    void* payload{};
};
void* Noncooperative(void* argument) {
    auto& job = *static_cast<Running*>(argument);
    job.entered = true;
    while (!job.release) std::this_thread::yield();
    return job.payload;
}
}

int main() {
    try {
        static_assert(std::is_same_v<decltype(&OSDisableScheduler), s32(*)(void)>);
        static_assert(std::is_same_v<decltype(&OSEnableScheduler), s32(*)(void)>);
        static_assert(sizeof(s32) == 4);
        auto* caller = OSGetCurrentThread();
        auto payload = std::make_unique<unsigned>(0xC0FFEEu);
        Check(std::uintptr_t(payload.get()) > 0xFFFFFFFFull, "actual message pointer is not above 4GiB");
        {
            Owned owner; Job job; Create(owner, job, payload.get());
            Check(OSDisableScheduler() == 0 && NativeInterruptsEnabled(), "first disable prior count/mask changed");
            Check(OSResumeThread(&owner.sdk) == 1 && owner.sdk.suspend == 0,
                  "original resume counter changed under scheduler disable");
            Check(OSSendMessage(&job.messages, payload.get(), 0), "actual original queued message failed");
            std::this_thread::sleep_for(5ms);
            Check(!job.entered && owner.sdk.state == OS_THREAD_STATE_READY,
                  "native source entry ran while scheduler was disabled");
            Check(OSDisableScheduler() == 1, "nested original disable prior count changed");
            Check(OSEnableScheduler() == 2, "nested enable prior count changed");
            std::this_thread::sleep_for(3ms);
            Check(!job.entered, "partial enable released original source entry");
            Check(OSEnableScheduler() == 1 && NativeInterruptsEnabled(), "final enable prior count/mask changed");
            Join(owner, job);
        }
        {
            Owned owner; Job job; Create(owner, job, payload.get());
            Check(OSResumeThread(&owner.sdk) == 1, "wait worker resume failed");
            Until([&] { return Waiting(owner, job); });
            Check(OSDisableScheduler() == 0, "genuinely blocked peer rejected scheduler disable");
            irq_job = &job;
            Check(DispatchNativeInterrupt(WakeIRQ), "actual owner IRQ failed to deliver queued message");
            const auto mask = OSDisableInterrupts();
            Check(owner.sdk.state == OS_THREAD_STATE_READY && !owner.sdk.queue &&
                  !job.messages.queueReceive.head && !job.messages.queueReceive.tail,
                  "original wake did not remove source queue and mark READY");
            OSRestoreInterrupts(mask);
            std::this_thread::sleep_for(5ms);
            Check(!job.returned && NativeInterruptsEnabled(), "woken source frames resumed while disabled");
            Check(OSEnableScheduler() == 1, "final enable did not release actual source waiter");
            Join(owner, job); irq_job = nullptr;
        }
        {
            Check(OSEnableScheduler() == 0, "unbalanced enable did not retain original zero prior value");
            Owned owner; Job job; Create(owner, job, payload.get());
            Check(OSSendMessage(&job.messages, payload.get(), 0), "negative-count message setup failed");
            Check(OSResumeThread(&owner.sdk) == 1, "negative-count actual resume failed");
            Join(owner, job);
            Check(OSDisableScheduler() == -1, "native adapter clamped original negative counter quirk");
            Check(OSDisableScheduler() == 0 && OSEnableScheduler() == 1,
                  "original signed counter did not return to balanced operation");
        }
        {
            Owned owner; Running job; job.payload = payload.get();
            Check(OSCreateThread(&owner.sdk, Noncooperative, &job, owner.Top(), sizeof(owner.stack), 15, 0),
                  "actual running peer create failed");
            Check(OSResumeThread(&owner.sdk) == 1, "actual running peer resume failed");
            Until([&] { return job.entered.load(); });
            Reject([] { OSDisableScheduler(); }, "native adapter fabricated preemption of live source frames");
            Check(OSEnableScheduler() == 0 && OSDisableScheduler() == -1,
                  "rejected disable mutated original counter");
            job.release = true;
            void* result{};
            Check(OSJoinThread(&owner.sdk, &result) && result == job.payload, "actual running peer failed to return/join");
            DrainNativeThreadLifetimes();
        }
        {
            OSThreadQueue queue{}; OSInitThreadQueue(&queue);
            Check(OSDisableScheduler() == 0, "owner refusal setup failed");
            Reject([&] { OSSleepThread(&queue); }, "disabled caller silently descheduled");
            Reject([&] { OSSuspendThread(caller); }, "disabled caller silently self-suspended");
            Check(!queue.head && !queue.tail && caller->state == OS_THREAD_STATE_RUNNING &&
                  caller->suspend == 0 && NativeInterruptsEnabled(), "rejected wait changed source state/mask");
            std::atomic<bool> foreign_rejected{};
            std::thread foreign([&] {
                try { OSEnableScheduler(); } catch (const std::logic_error&) { foreign_rejected = true; }
            }); foreign.join();
            Check(foreign_rejected, "foreign caller changed disabled owner counter");
            Check(OSDisableScheduler() == 1 && OSEnableScheduler() == 2 && OSEnableScheduler() == 1,
                  "foreign rejection changed original nested count");
        }
        {
            Owned owner; Job job; Create(owner, job, payload.get());
            Check(OSDisableScheduler() == 0 && OSResumeThread(&owner.sdk) == 1,
                  "cancel setup failed");
            OSCancelThread(&owner.sdk);
            Check(!job.entered && OSIsThreadTerminated(&owner.sdk), "cancelled parked entry executed source frames");
            void* result{};
            Check(OSJoinThread(&owner.sdk, &result) && std::uintptr_t(result) == 0xFFFFFFFFu,
                  "genuine parked cancellation/join default changed");
            Check(OSEnableScheduler() == 1, "cancel retirement lost scheduler ownership");
            DrainNativeThreadLifetimes();
        }
        const auto masked = OSDisableInterrupts();
        Check(OSDisableScheduler() == 0 && OSEnableScheduler() == 1 && !NativeInterruptsEnabled(),
              "original scheduler operations changed already-masked caller state");
        OSRestoreInterrupts(masked);
        Check(OSGetCurrentThread() == caller && NativeInterruptsEnabled(), "scheduler gate changed current descriptor/mask");
        std::printf("Native scheduler PASS:%u checks; original signed counters, genuine source entry/wake gates; live preemption/reset held\n", checks);
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "Native scheduler failed after %u checks: %s\n", checks, e.what());
        std::fflush(nullptr);
        std::_Exit(1); // Never unwind borrowed live worker descriptors after a fixture failure.
    }
}
