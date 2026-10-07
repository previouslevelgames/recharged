#include "platform/interrupts.h"
#include "platform/thread.h"
#include "platform/thread_queues.h"
#include <dolphin/os.h>
#include <dolphin/os/OSThread.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <exception>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {
using namespace mscharged::platform;
using namespace std::chrono_literals;
std::atomic<unsigned> checks{}, returns{}, irq_wakes{}, service_calls{};
std::thread::id owner;
OSThreadQueue* owner_queue{};
OSContext* owner_context{};
std::atomic<bool> pending{};

void Check(bool value, const char* message) {
    ++checks;
    if (!value) throw std::runtime_error(message);
}
template<class F> void Reject(F operation, const char* message) {
    bool rejected{};
    try { operation(); }
    catch (const std::logic_error&) { rejected = true; }
    Check(rejected, message);
}
std::vector<OSThread*> Queue(OSThreadQueue& queue) {
    const auto enabled = OSDisableInterrupts();
    std::vector<OSThread*> result;
    for (auto* p = queue.head; p; p = p->link.next) result.push_back(p);
    OSRestoreInterrupts(enabled);
    return result;
}
template<class F> void Until(F predicate) {
    const auto end = std::chrono::steady_clock::now() + 2s;
    while (!predicate()) {
        if (std::chrono::steady_clock::now() >= end)
            throw std::runtime_error("Actual native thread operation timed out");
        std::this_thread::yield();
    }
}

void InterruptWake() {
    Check(std::this_thread::get_id() == owner, "Wait IRQ escaped actual owner");
    Check(NativeInterruptDispatchActive() && !NativeInterruptsEnabled(), "Wait IRQ context/mask lost");
    Check(OSGetCurrentContext() != owner_context, "Wait IRQ did not install temporary context");
    Reject([&] { OSSleepThread(owner_queue); }, "IRQ callback accepted an actual blocking wait");
    Reject([] { SetNativeThreadWaitService(nullptr); }, "IRQ callback replaced the active wait hook");
    const auto enabled = OSEnableInterrupts();
    Check(NativeInterruptDispatchActive(), "Enabled IRQ lost dispatch identity");
    Reject([&] { OSSleepThread(owner_queue); }, "Enabled IRQ callback accepted a blocking wait");
    OSRestoreInterrupts(enabled);
    OSWakeupThread(owner_queue);
    ++irq_wakes;
}
void WaitService() {
    ++service_calls;
    Check(std::this_thread::get_id() == owner, "Wait service was inherited by a foreign thread");
    Check(NativeInterruptsEnabled() && !NativeInterruptDispatchActive(), "Wait service retained source exclusion");
    Reject([] { SetNativeThreadWaitService(nullptr); }, "Wait hook replaced itself while borrowed");
    if (pending.exchange(false))
        Check(DispatchNativeInterrupt(InterruptWake), "Actual latched wait event did not dispatch");
}
void FailingService() { throw std::runtime_error("Deliberate failed native wait endpoint"); }

void OneWait(bool masked, bool stale_wake) {
    OSThreadQueue queue;
    OSInitThreadQueue(&queue);
    if (stale_wake) OSWakeupThread(&queue);
    auto* thread = OSGetCurrentThread();
    auto* context = OSGetCurrentContext();
    std::atomic<bool> actual_wake{};
    std::thread waker([&] {
        Until([&] { return Queue(queue).size() == 1; });
        Check(OSGetCurrentThread() != thread, "Foreign caller reused owner SDK thread metadata");
        Check(Queue(queue)[0] == thread, "Source owner wait is not on real queue");
        std::this_thread::sleep_for(8ms);
        actual_wake = true;
        OSWakeupThread(&queue);
    });
    const auto outer = masked ? OSDisableInterrupts() : TRUE;
    OSSleepThread(&queue);
    Check(actual_wake, "Empty old wake was credited to a later source wait");
    Check(NativeInterruptsEnabled() == !masked, "Source caller mask not restored after sleep");
    Check(OSGetCurrentContext() == context, "Native wait changed source caller context");
    Check(!thread->queue && !thread->link.next && !thread->link.prev &&
        thread->state == OS_THREAD_STATE_RUNNING, "Returned native thread still borrows wait queue");
    if (masked) OSRestoreInterrupts(outer);
    waker.join();
    ++returns;
    Check(!queue.head && !queue.tail, "Actual wake left source queue populated");
}

void OrderedWaiters() {
    auto* queue = new OSThreadQueue;
    OSInitThreadQueue(queue);
    constexpr std::array<int,5> priorities{10,2,10,6,2};
    std::array<std::atomic<OSThread*>,5> descriptor{};
    std::vector<std::thread> workers;
    std::mutex error_latch;
    std::exception_ptr failure;
    for (unsigned index = 0; index < priorities.size(); ++index) {
        workers.emplace_back([&, index] {
            try {
                auto* thread = OSGetCurrentThread();
                Check(OSSetThreadPriority(thread, priorities[index]), "Valid source priority request rejected");
                Check(OSGetThreadPriority(thread) == priorities[index], "SDK thread priority not retained");
                auto* context = OSGetCurrentContext();
                descriptor[index] = thread;
                OSSleepThread(queue);
                Check(NativeInterruptsEnabled() && OSGetCurrentContext() == context,
                    "Foreign wait lost native mask/context");
                Check(thread->state == OS_THREAD_STATE_RUNNING && !thread->queue &&
                    !thread->link.next && !thread->link.prev, "Foreign return borrows freed source queue");
                ++returns;
            } catch (...) {
                std::lock_guard lock(error_latch);
                failure = std::current_exception();
            }
        });
        Until([&] { return Queue(*queue).size() == index + 1; });
    }
    const auto mask = OSDisableInterrupts();
    const auto order = Queue(*queue);
    const std::array<unsigned,5> expected{1,4,3,0,2};
    for (unsigned n=0; n<expected.size(); ++n) {
        Check(order[n] == descriptor[expected[n]], "Original priority/equal-FIFO wait ordering changed");
        Check(order[n]->link.prev == (n ? order[n-1] : nullptr), "Original queue previous-link broken");
        Check(order[n]->link.next == (n+1<expected.size() ? order[n+1] : nullptr), "Original queue next-link broken");
    }
    Reject([&] { OSInitThreadQueue(queue); }, "Active source queue was silently reinitialized");
    Check(Queue(*queue) == order, "Rejected reinitialization changed borrowed links");
    Check(OSSetThreadPriority(descriptor[3], 1), "Sleeping thread reprioritization rejected");
    const auto revised = Queue(*queue);
    Check(revised[0] == descriptor[3] && revised[1] == descriptor[1] && revised[2] == descriptor[4],
        "Native sleeping priority change lost source ordering");
    auto* expired = descriptor[0].load();
    OSWakeupThread(queue);
    Check(!queue->head && !queue->tail, "Nonempty wake did not drain all real waiters");
    for (const auto& p : descriptor)
        Check(p.load()->state == OS_THREAD_STATE_READY && !p.load()->queue,
            "Actual waiter not made ready before caller exclusion release");
    delete queue;
    // Queue storage is no longer borrowed after wake; every real waiter still
    // waits for this actual source critical section to release before return.
    OSRestoreInterrupts(mask);
    for (auto& worker : workers) worker.join();
    if (failure) std::rethrow_exception(failure);
    Reject([&] { OSGetThreadPriority(expired); }, "Expired SDK native thread descriptor was reused");
}
} // namespace

int main() {
    try {
        owner = std::this_thread::get_id();
        owner_context = OSGetCurrentContext();
        Check(owner_context && NativeInterruptsEnabled() && !NativeInterruptDispatchActive(), "Native owner initial context invalid");
        auto* current = OSGetCurrentThread();
        Check(current == OSGetCurrentThread(), "SDK current native caller identity is unstable");
        Check(reinterpret_cast<std::uintptr_t>(current) > 0xffffffffULL, "Actual SDK caller identity was not above 4 GiB");
        Check(current->state == OS_THREAD_STATE_RUNNING && current->attr == OS_THREAD_ATTR_DETACH &&
            current->priority == 16 && current->base == 16 && !current->suspend, "Source default native caller metadata changed");
        const auto bounds = mscharged::CurrentThreadStackLimits();
        Check(reinterpret_cast<std::uintptr_t>(current->stackBase) == bounds.high &&
            reinterpret_cast<std::uintptr_t>(current->stackEnd) == bounds.low, "SDK native stack boundaries are not actual host stack");
        Check(OSGetCurrentContext() == owner_context, "Current-thread query mutated original context");
        Check(!OSSetThreadPriority(current,-1) && !OSSetThreadPriority(current,32) &&
            OSGetThreadPriority(current)==16, "Invalid original priority request changed state");
        Check(OSSetThreadPriority(current,20) && current->base==20 && OSGetThreadPriority(current)==20,
            "Actual source default-thread priority request lost");

        struct Guarded { unsigned before; OSThreadQueue queue; unsigned after; } guarded;
        std::memset(&guarded,0xA5,sizeof(guarded));
        std::array<unsigned char,sizeof(guarded)> expected;
        std::memcpy(expected.data(),&guarded,sizeof(guarded));
        std::memset(expected.data()+offsetof(Guarded,queue)+offsetof(OSThreadQueue,head),0,sizeof(guarded.queue.head));
        std::memset(expected.data()+offsetof(Guarded,queue)+offsetof(OSThreadQueue,tail),0,sizeof(guarded.queue.tail));
        OSInitThreadQueue(&guarded.queue);
        Check(std::memcmp(expected.data(),&guarded,sizeof(guarded))==0, "Queue init changed bytes beyond original two-pointer writes");
        OSWakeupThread(&guarded.queue);
        Check(std::memcmp(expected.data(),&guarded,sizeof(guarded))==0, "Empty wake changed initialized source queue");
        Reject([] { OSInitThreadQueue(nullptr); }, "Null queue constructor was accepted");
        Reject([] { OSWakeupThread(nullptr); }, "Null queue wake was accepted");
        Reject([] { OSSleepThread(nullptr); }, "Null queue sleep was accepted");

        {
            NativeInterruptGuard first;
            NativeInterruptGuard second;
            Check(!NativeInterruptWaitAllowed(), "Extra host exclusion was not tracked");
            Reject([&] { OSSleepThread(&guarded.queue); }, "Host exclusion accepted a deadlocking source wait");
            Check(!guarded.queue.head && !guarded.queue.tail, "Rejected host guard borrowed queue storage");
        }
        Check(NativeInterruptWaitAllowed(), "Retired host guards retained exclusion metadata");
        {
            NativeInterruptRead read;
            Check(bool(read) && !NativeInterruptWaitAllowed(), "Host reader exclusion was not tracked");
            Reject([&] { OSSleepThread(&guarded.queue); }, "Host reader accepted a deadlocking source wait");
        }
        Check(NativeInterruptWaitAllowed(), "Retired reader retained exclusion metadata");
        OneWait(false,false);
        OneWait(true,false);
        OneWait(false,true);
        OneWait(true,true);
        OrderedWaiters();

        OSThread fake{};
        OSThreadQueue unknown{&fake,&fake};
        Reject([&] { OSWakeupThread(&unknown); }, "Unregistered nonempty queue manufactured wake success");
        Check(unknown.head==&fake && unknown.tail==&fake, "Unqualified queue was mutated before rejection");

        OSThreadQueue irq_queue;
        OSInitThreadQueue(&irq_queue);
        owner_queue=&irq_queue;
        Check(!SetNativeThreadWaitService(WaitService), "Fresh real owner wait service slot was occupied");
        std::thread producer([] { std::this_thread::sleep_for(12ms); pending=true; });
        const auto disabled=OSDisableInterrupts();
        OSSleepThread(&irq_queue);
        Check(!NativeInterruptsEnabled(), "Owner IRQ wait lost original masked caller");
        OSRestoreInterrupts(disabled);
        producer.join();
        ++returns;
        Check(irq_wakes==1 && service_calls>0 && !pending, "Actual worker-latched owner wake lost or repeated");
        Check(OSGetCurrentContext()==owner_context && !NativeInterruptDispatchActive(), "Owner wait IRQ leaked context/dispatch state");
        Check(SetNativeThreadWaitService(nullptr)==WaitService, "Exact borrowed owner wait service was not retired");

        Check(!SetNativeThreadWaitService(FailingService), "Wait service retirement retained old callback");
        bool failed{};
        try { OSSleepThread(&irq_queue); }
        catch(const std::runtime_error&) { failed=true; }
        Check(failed && !irq_queue.head && !irq_queue.tail && !current->queue &&
            current->state==OS_THREAD_STATE_RUNNING && NativeInterruptsEnabled(),
            "Failed hardware wait endpoint fabricated completion or retained borrowed queue/mask");
        Check(SetNativeThreadWaitService(nullptr)==FailingService, "Failed borrowed wait endpoint was not retired");
        current->suspend=1;
        Reject([&] { OSSleepThread(&irq_queue); }, "Unimplemented native thread suspension manufactured successful wait");
        current->suspend=0;
        Check(!irq_queue.head && !irq_queue.tail, "Rejected suspension changed source queue");
        for (unsigned n=0;n<4;++n) OneWait(n&1,true);
        Check(OSGetCurrentContext()==owner_context && NativeInterruptsEnabled(), "Final source owner context/mask differs");
        std::printf("Native OS thread queues: %u checks, %u actual wait returns, %u owner IRQ wake; canonical OSThread=%zu/align%zu queue=%zu. No source AX/DSP initialization or readiness.\n",
            checks.load(),returns.load(),irq_wakes.load(),sizeof(OSThread),alignof(OSThread),sizeof(OSThreadQueue));
        return 0;
    } catch(const std::exception& error) {
        std::fprintf(stderr,"Native OS thread queue failure: %s (%u checks/%u returns)\n",error.what(),checks.load(),returns.load());
        return 1;
    }
}
