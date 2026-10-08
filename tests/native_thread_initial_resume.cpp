#include "platform/interrupts.h"
#include "platform/thread_queues.h"
#include <dolphin/os.h>
#include <dolphin/os/OSMessage.h>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <stdexcept>
#include <thread>

using namespace mscharged::platform;
using namespace std::chrono_literals;
namespace {
std::atomic<unsigned> checks{};
void Check(bool value, const char* message) {
    ++checks;
    if (!value) throw std::runtime_error(message);
}
void CheckRetained(bool value, const char* message) {
    ++checks;
    if (!value) {
        std::fprintf(stderr, "Native initial Resume failed: %s\n", message);
        std::fflush(nullptr);
        std::_Exit(1); // A still-live observer/source worker cannot be unwound.
    }
}
template<class F> void Reject(F operation, const char* message) {
    bool rejected{};
    try { operation(); } catch (const std::logic_error&) { rejected = true; }
    Check(rejected, message);
}
template<class F> void Until(F predicate) {
    const auto end = std::chrono::steady_clock::now() + 2s;
    while (!predicate()) {
        if (std::chrono::steady_clock::now() >= end)
            throw std::runtime_error("Actual initial-resume fixture boundary timed out");
        std::this_thread::yield();
    }
}
struct Owned {
    OSThread sdk{};
    alignas(32) std::array<unsigned char, 0x4000> stack{};
};
struct Job {
    Owned owner;
    OSMessageQueue queue{};
    OSMessage cell{};
    std::atomic<bool> entered{}, allow_initialization{true}, returned{};
    bool self_suspend{};
};
void* Receive(void* argument) {
    auto& job = *static_cast<Job*>(argument);
    job.entered = true;
    while (!job.allow_initialization) std::this_thread::yield();
    OSInitMessageQueue(&job.queue, &job.cell, 1);
    if (job.self_suspend)
        Check(OSSuspendThread(OSGetCurrentThread()) == 0, "Actual source self-suspend count changed");
    OSMessage value{};
    Check(OSReceiveMessage(&job.queue, &value, OS_MESSAGE_BLOCK) && value == &job,
          "Whole original message receive lost the actual payload");
    job.returned = true;
    return value;
}
void Create(Owned& owner, void* (*entry)(void*), void* argument, int priority) {
    Check(OSCreateThread(&owner.sdk, entry, argument, owner.stack.data() + owner.stack.size(),
          owner.stack.size(), priority, 0), "Actual source worker creation failed");
    Check(owner.sdk.state == OS_THREAD_STATE_READY && owner.sdk.suspend == 1,
          "Actual source creation did not retain its parked incarnation");
}
bool Waiting(Job& job) {
    const auto mask = OSDisableInterrupts();
    const bool value = job.owner.sdk.state == OS_THREAD_STATE_WAITING &&
        job.owner.sdk.queue == &job.queue.queueReceive && job.queue.queueReceive.head == &job.owner.sdk;
    OSRestoreInterrupts(mask);
    return value;
}
void Join(Owned& owner, void* expected) {
    void* result{};
    Check(OSJoinThread(&owner.sdk, &result) && result == expected && owner.sdk.state == 0,
          "Actual attached return/join changed source result or retirement");
}
void InitialBlock(bool masked) {
    Job job; job.allow_initialization = false;
    Create(job.owner, Receive, &job, 3);
    auto* caller = OSGetCurrentThread();
    auto* context = OSGetCurrentContext();
    std::atomic<bool> resume_returned{};
    std::exception_ptr observer_failure;
    std::thread observer([&] {
        try {
            Until([&] { return job.entered.load(); });
            Check(!resume_returned, "Initial Resume credited first entry before actual source queue initialization");
            const auto mask = OSDisableInterrupts();
            Check(caller->state == OS_THREAD_STATE_READY && job.owner.sdk.state == OS_THREAD_STATE_RUNNING,
                  "Initial handoff did not retain the real ready caller/running peer");
            OSRestoreInterrupts(mask);
            auto* marker = reinterpret_cast<void*>(std::uintptr_t{0x123456789ABCDEF0ull});
            void* result = marker;
            Reject([&] { OSJoinThread(&job.owner.sdk, &result); }, "Concurrent Join retired the initial incarnation");
            Check(result == marker, "Rejected concurrent Join wrote a result");
            Reject([&] { OSDetachThread(&job.owner.sdk); }, "Concurrent Detach changed the initial incarnation");
            Reject([&] { OSSetThreadPriority(&job.owner.sdk, 4); }, "Running initial peer base reprioritization skipped source rescheduling");
            Reject([&] { OSSetThreadPriority(caller, 15); }, "Parked resumer base reprioritization skipped source rescheduling");
            Check(job.owner.sdk.base == 3 && caller->base == 16,
                  "Rejected base reprioritization changed an original priority");
            Reject([&] { OSCancelThread(&job.owner.sdk); }, "Concurrent cancellation consumed initial execution");
            Reject([&] { Create(job.owner, Receive, &job, 3); }, "Concurrent source reuse replaced the initial incarnation");
        } catch (...) { observer_failure = std::current_exception(); }
        job.allow_initialization = true;
    });
    const auto outer = masked ? OSDisableInterrupts() : TRUE;
    CheckRetained(OSResumeThread(&job.owner.sdk) == 1, "Initial source Resume returned the wrong prior count");
    resume_returned = true;
    CheckRetained(Waiting(job) && job.queue.msgArray == &job.cell && job.queue.msgCount == 1,
          "Initial Resume returned before original queue sleep");
    CheckRetained(NativeInterruptsEnabled() == !masked && OSGetCurrentContext() == context &&
          caller->state == OS_THREAD_STATE_RUNNING, "Initial Resume did not restore its real caller mask/context");
    if (masked) OSRestoreInterrupts(outer);
    observer.join();
    if (observer_failure) std::rethrow_exception(observer_failure);
    Check(OSSendMessage(&job.queue, &job, 0), "Immediate original message send failed after initial Resume");
    Join(job.owner, &job);
    DrainNativeThreadLifetimes();
}
struct Nested {
    Job parent, child;
};
void* NestedEntry(void* argument) {
    auto& nest = *static_cast<Nested*>(argument);
    Create(nest.child.owner, Receive, &nest.child, 2);
    Check(OSResumeThread(&nest.child.owner.sdk) == 1 && Waiting(nest.child),
          "Nested initial Resume did not await its genuine higher-priority source block");
    return Receive(&nest.parent);
}
void NestedBlock() {
    Nested nest;
    Create(nest.parent.owner, NestedEntry, &nest, 4);
    Check(OSResumeThread(&nest.parent.owner.sdk) == 1 && Waiting(nest.parent) && Waiting(nest.child),
          "Nested higher-priority resumes lost the actual caller chain");
    Check(OSSendMessage(&nest.child.queue, &nest.child, 0) && OSSendMessage(&nest.parent.queue, &nest.parent, 0),
          "Actual nested source message wake failed");
    Join(nest.parent.owner, &nest.parent);
    Join(nest.child.owner, &nest.child);
    DrainNativeThreadLifetimes();
}
std::atomic<bool> frame_destroyed{};
void* Returned(void* argument) {
    struct Frame { ~Frame() { frame_destroyed = true; } } frame;
    return argument;
}
void* Failed(void*) { throw std::runtime_error("Actual initial source-entry failure"); }
void ReturnAndFailure() {
    Owned owner; frame_destroyed = false;
    Create(owner, Returned, &owner, 3);
    Check(OSResumeThread(&owner.sdk) == 1 && frame_destroyed && OSIsThreadTerminated(&owner.sdk),
          "Initial Resume returned before actual source entry/frames ended");
    Join(owner, &owner); DrainNativeThreadLifetimes();
    Create(owner, Failed, nullptr, 3);
    Check(OSResumeThread(&owner.sdk) == 1, "Failed entry changed original Resume suspend-count result");
    bool failed{};
    try { OSIsThreadTerminated(&owner.sdk); } catch (const std::runtime_error&) { failed = true; }
    Check(failed, "Initial entry failure became successful termination readiness");
    failed = false; void* output = &owner;
    try { OSJoinThread(&owner.sdk, &output); } catch (const std::runtime_error&) { failed = true; }
    Check(failed && output == &owner && owner.sdk.state == 0,
          "Actual failed entry did not retain its failure through genuine source join");
    DrainNativeThreadLifetimes();
}
void UnsupportedAndDeferred() {
    Job job; Create(job.owner, Receive, &job, 3);
    const auto before = job.owner.sdk;
    auto unchanged = [&] { Check(!job.entered && !std::memcmp(&before, &job.owner.sdk, sizeof(before)),
                                 "Rejected initial Resume changed source descriptor/count"); };
    {
        NativeInterruptGuard first, second;
        Reject([&] { OSResumeThread(&job.owner.sdk); }, "Initial Resume waited under retained host exclusions");
        unchanged();
    }
    auto irq = [&] {
        Reject([&] { OSResumeThread(&job.owner.sdk); }, "IRQ initial Resume waited inside actual interrupt delivery");
        unchanged();
        const auto mask = OSEnableInterrupts();
        Reject([&] { OSResumeThread(&job.owner.sdk); }, "Enabled IRQ fabricated a cooperative initial boundary");
        unchanged(); OSRestoreInterrupts(mask);
    };
    Check(DispatchNativeInterrupt([](void* context) { (*static_cast<decltype(irq)*>(context))(); }, &irq),
          "Actual IRQ negative did not dispatch");
    auto* context = OSGetCurrentContext(); OSContext foreign{};
    OSSetCurrentContext(&foreign);
    Reject([&] { OSResumeThread(&job.owner.sdk); }, "Foreign context fabricated source rescheduling");
    unchanged(); OSSetCurrentContext(context);
    Check(OSDisableScheduler() == 0, "Actual original scheduler disable count changed");
    Check(OSResumeThread(&job.owner.sdk) == 1 && !job.entered && job.owner.sdk.suspend == 0,
          "Initial Resume started execution while original scheduling was disabled");
    Check(OSEnableScheduler() == 1, "Actual original scheduler enable count changed");
    // Deferred rescheduling is a separate held contract: this observer is not
    // an initial-Resume success predicate or a source wait inserted by the port.
    Until([&] { return Waiting(job); });
    Check(OSSendMessage(&job.queue, &job, 0), "Genuine deferred source message send failed");
    Join(job.owner, &job); DrainNativeThreadLifetimes();
}
struct Spin { std::atomic<bool> entered{}, release{}; };
void* ParallelSpin(void* argument) {
    auto& spin = *static_cast<Spin*>(argument);
    spin.entered = true;
    while (!spin.release) std::this_thread::yield();
    return argument;
}
void RunningPeerNegative() {
    Owned running; Spin spin;
    // Explicit native-only parallel setup: equal priority does not require
    // original SelectThread(FALSE) to preempt its current caller.
    Create(running, ParallelSpin, &spin, OSGetThreadPriority(OSGetCurrentThread()));
    Check(OSResumeThread(&running.sdk) == 1, "Equal-priority native fixture did not resume");
    Until([&] { return spin.entered.load(); });
    Job cold; Create(cold.owner, Receive, &cold, 3);
    const auto before = cold.owner.sdk;
    Reject([&] { OSResumeThread(&cold.owner.sdk); }, "Initial Resume fabricated preemption of a foreign running peer");
    Check(!cold.entered && !std::memcmp(&before, &cold.owner.sdk, sizeof(before)),
          "Rejected foreign preemption mutated the initial target");
    spin.release = true; Join(running, &spin);
    OSCancelThread(&cold.owner.sdk);
    Join(cold.owner, reinterpret_cast<void*>(std::uintptr_t{0xffffffffu}));
    DrainNativeThreadLifetimes();
}
void SelfSuspend() {
    Job job; job.self_suspend = true;
    Create(job.owner, Receive, &job, 3);
    Check(OSResumeThread(&job.owner.sdk) == 1 && job.owner.sdk.state == OS_THREAD_STATE_READY &&
          job.owner.sdk.suspend == 1 && OSIsThreadSuspended(&job.owner.sdk),
          "Initial Resume did not await actual source self-suspension");
    Check(OSResumeThread(&job.owner.sdk) == 1 && OSSendMessage(&job.queue, &job, 0),
          "Genuine self-suspended source continuation did not resume");
    Join(job.owner, &job); DrainNativeThreadLifetimes();
}
}
int main() {
    try {
        Check(OSGetThreadPriority(OSGetCurrentThread()) == 16, "Actual default native caller priority changed");
        InitialBlock(false); InitialBlock(true);
        NestedBlock(); ReturnAndFailure(); UnsupportedAndDeferred(); SelfSuspend(); RunningPeerNegative();
        std::printf("Native initial Resume: %u checks; genuine block/return/nested/mask/incarnation gates\n", checks.load());
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "Native initial Resume failed: %s\n", e.what());
        std::fflush(nullptr);
        std::_Exit(1); // Retain any failed source lifetime; no forced completion.
    }
}
