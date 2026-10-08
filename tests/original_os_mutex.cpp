#include "platform/interrupts.h"
#include "platform/thread_queues.h"
#include <dolphin/os.h>
#include <revolution/os/OSMessage.h>
#include <revolution/os/OSMutex.h>
#include <revolution/os/OSThread.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <stdexcept>
#include <thread>
#include <initializer_list>

namespace {
using namespace mscharged::platform;
using namespace std::chrono_literals;
std::atomic<unsigned> checks{};
void Check(bool value, const char* message) {
    ++checks;
    if (!value) throw std::runtime_error(message);
}
class Mask {
public:
    Mask() : previous_(OSDisableInterrupts()) {}
    ~Mask() { OSRestoreInterrupts(previous_); }
private:
    BOOL previous_;
};
template<class F> void Until(F condition) {
    const auto limit = std::chrono::steady_clock::now() + 2s;
    while (!condition()) {
        if (std::chrono::steady_clock::now() >= limit)
            throw std::runtime_error("Original mutex owner did not reach its real wait boundary");
        std::this_thread::yield();
    }
}
template<class F> void Reject(F action, const char* message) {
    bool rejected{};
    try { action(); } catch (const std::logic_error&) { rejected = true; }
    Check(rejected, message);
}
struct Owned {
    OSThread sdk{};
    alignas(8) std::array<u8, 0x4000> stack{};
};
void Start(Owned& owner, void* (*entry)(void*), void* argument, s32 priority) {
    Check(OSCreateThread(&owner.sdk, entry, argument, owner.stack.data() + owner.stack.size(),
              owner.stack.size(), priority, 0), "Actual SDK worker creation failed");
    Check(OSResumeThread(&owner.sdk) == 1, "Actual initial SDK Resume counter changed");
}
void Join(Owned& owner, void* expected) {
    void* result{};
    Check(OSJoinThread(&owner.sdk, &result) && result == expected,
          "Original attached worker did not return and join");
    Check(OSIsThreadTerminated(&owner.sdk), "Joined worker still owns source execution");
}
bool Waiting(Owned& owner, OSThreadQueue& queue, OSMutex* mutex = nullptr) {
    Mask mask;
    return owner.sdk.state == OS_THREAD_STATE_WAITING && owner.sdk.queue == &queue
        && owner.sdk.mutex == mutex;
}
void Order(OSThreadQueue& queue, std::initializer_list<OSThread*> expected) {
    Mask mask;
    auto* node = queue.head;
    OSThread* previous{};
    for (auto* item : expected) {
        Check(node == item && item->link.prev == previous,
              "Original priority/equal-priority wait order changed");
        previous = node;
        node = node->link.next;
    }
    Check(!node && queue.tail == previous, "Original wait tail/extent changed");
}
struct LockJob {
    Owned owner;
    OSMutex* mutex{};
};
void* LockWorker(void* argument) {
    auto& job = *static_cast<LockJob*>(argument);
    OSLockMutex(job.mutex);
    {
        Mask mask;
        Check(OSGetCurrentThread() == &job.owner.sdk && job.mutex->thread == &job.owner.sdk
              && job.mutex->count == 1 && !job.owner.sdk.mutex,
              "Whole original OSMutex did not acquire its real source owner");
    }
    OSUnlockMutex(job.mutex);
    {
        Mask mask;
        Check(!job.owner.sdk.queueMutex.head && !job.owner.sdk.queueMutex.tail
              && job.owner.sdk.priority == job.owner.sdk.base,
              "Whole original OSMutex failed to remove/deboost its last owned lock");
    }
    Check(NativeInterruptsEnabled(), "Whole original OSMutex changed its worker mask");
    return &job;
}

void Contention(OSThread* caller) {
    OSMutex mutex{};
    OSInitMutex(&mutex);
    OSLockMutex(&mutex);
    OSLockMutex(&mutex);
    Check(OSTryLockMutex(&mutex), "Original recursive TryLock failed");
    Check(mutex.thread == caller && mutex.count == 3 && caller->queueMutex.head == &mutex
          && caller->queueMutex.tail == &mutex, "Original recursive ownership/list changed");
    OSUnlockMutex(&mutex);
    OSUnlockMutex(&mutex);
    Check(mutex.count == 1 && mutex.thread == caller, "Original recursive release ended ownership early");
    Check(OSSetThreadPriority(caller, 20), "Actual owner base-priority request failed");

    std::array<LockJob, 3> jobs;
    const std::array<s32, 3> priorities{8, 4, 12};
    for (unsigned i = 0; i < jobs.size(); ++i) {
        jobs[i].mutex = &mutex;
        Start(jobs[i].owner, LockWorker, &jobs[i], priorities[i]);
        Until([&] { return Waiting(jobs[i].owner, mutex.queue, &mutex); });
    }
    Order(mutex.queue, {&jobs[1].owner.sdk, &jobs[0].owner.sdk, &jobs[2].owner.sdk});
    Check(OSGetThreadPriority(caller) == 4 && __OSGetEffectivePriority(caller) == 4
          && caller->base == 20, "Real waiter priority did not reach its original mutex owner");
    Check(OSSetThreadPriority(caller, 3) && OSGetThreadPriority(caller) == 3,
          "Original base priority did not win over weaker mutex donation");
    Check(OSSetThreadPriority(caller, 20) && OSGetThreadPriority(caller) == 4,
          "Original base-priority update dropped a live donation");
    Check(OSSetThreadPriority(&jobs[1].owner.sdk, 10) && OSGetThreadPriority(caller) == 8,
          "Real wait reprioritization did not recompute its owner's inherited priority");
    Check(OSSetThreadPriority(&jobs[2].owner.sdk, 8), "Real equal-priority request failed");
    Order(mutex.queue, {&jobs[0].owner.sdk, &jobs[2].owner.sdk, &jobs[1].owner.sdk});
    Check(OSSuspendThread(&jobs[0].owner.sdk) == 0 && jobs[0].owner.sdk.priority == 32,
          "Actual mutex-wait suspension lost its original sentinel");
    Check(OSSuspendThread(&jobs[2].owner.sdk) == 0 && OSGetThreadPriority(caller) == 10,
          "Actual suspended waiters kept donating their stale priority");
    Order(mutex.queue, {&jobs[1].owner.sdk, &jobs[0].owner.sdk, &jobs[2].owner.sdk});
    Check(!OSSetThreadPriority(&jobs[2].owner.sdk, -1)
          && !OSSetThreadPriority(&jobs[2].owner.sdk, 32) && jobs[2].owner.sdk.base == 8,
          "Invalid original priority requests changed a real suspended waiter");
    Check(OSResumeThread(&jobs[0].owner.sdk) == 1 && OSGetThreadPriority(caller) == 8,
          "Actual resumed mutex waiter did not restore/reapply its original priority");
    Check(OSResumeThread(&jobs[2].owner.sdk) == 1, "Actual second mutex waiter failed to resume");
    Order(mutex.queue, {&jobs[0].owner.sdk, &jobs[2].owner.sdk, &jobs[1].owner.sdk});
    Check(OSResumeThread(&jobs[0].owner.sdk) == 0, "Redundant original Resume counter changed");
    Order(mutex.queue, {&jobs[0].owner.sdk, &jobs[2].owner.sdk, &jobs[1].owner.sdk});
    Check(OSSetThreadPriority(caller, 14) && OSGetThreadPriority(caller) == 8,
          "Actual owner base update lost its strongest live waiter");
    OSUnlockMutex(&mutex);
    Check(OSGetThreadPriority(caller) == 14 && !caller->queueMutex.head,
          "Original final Unlock did not restore the owner's actual base priority");
    for (auto& job : jobs) Join(job.owner, &job);
    Check(!mutex.thread && !mutex.count && !mutex.queue.head && !mutex.queue.tail,
          "Actual contention left a borrowed mutex owner or waiter");
    DrainNativeThreadLifetimes();
    Reject([&] { __OSGetEffectivePriority(&jobs[0].owner.sdk); },
           "Expired native thread owner was accepted by the new priority endpoint");
    Check(OSSetThreadPriority(caller, 16), "Original caller priority restoration failed");
}

struct Chain {
    OSMutex first{}, second{};
    OSMessageQueue release;
    OSMessage cell{};
    Owned low, middle, high;
};
void* Low(void* argument) {
    auto& chain = *static_cast<Chain*>(argument);
    OSLockMutex(&chain.first);
    OSMessage message{};
    Check(OSReceiveMessage(&chain.release, &message, OS_MESSAGE_BLOCK) && message == &chain,
          "Actual holding owner lost its original message wait");
    OSUnlockMutex(&chain.first);
    Check(NativeInterruptsEnabled(), "Original low-owner release lost its interrupt mask");
    return &chain.low;
}
void* Middle(void* argument) {
    auto& chain = *static_cast<Chain*>(argument);
    OSLockMutex(&chain.second);
    OSLockMutex(&chain.first);
    OSUnlockMutex(&chain.first);
    {
        Mask mask;
        Check(chain.middle.sdk.queueMutex.head == &chain.second
              && chain.middle.sdk.queueMutex.tail == &chain.second
              && chain.middle.sdk.priority == __OSGetEffectivePriority(&chain.middle.sdk),
              "Unlocking one actual mutex ignored a remaining owned lock/donation");
    }
    OSUnlockMutex(&chain.second);
    return &chain.middle;
}
void* High(void* argument) {
    auto& chain = *static_cast<Chain*>(argument);
    OSLockMutex(&chain.second);
    OSUnlockMutex(&chain.second);
    return &chain.high;
}
void ChainInheritance() {
    Chain chain;
    OSInitMutex(&chain.first);
    OSInitMutex(&chain.second);
    OSInitMessageQueue(&chain.release, &chain.cell, 1);
    Start(chain.low, Low, &chain, 24);
    Until([&] { return Waiting(chain.low, chain.release.queueReceive); });
    Check(!OSTryLockMutex(&chain.first) && chain.first.count == 1,
          "Original foreign TryLock gained ownership or changed its count");
    Start(chain.middle, Middle, &chain, 12);
    Until([&] { return Waiting(chain.middle, chain.first.queue, &chain.first); });
    Check(OSGetThreadPriority(&chain.low.sdk) == 12, "First real chain waiter did not boost its owner");
    Start(chain.high, High, &chain, 4);
    Until([&] { return Waiting(chain.high, chain.second.queue, &chain.second); });
    Check(OSGetThreadPriority(&chain.middle.sdk) == 4 && OSGetThreadPriority(&chain.low.sdk) == 4,
          "Actual two-mutex chain did not propagate original inherited priority");
    Reject([&] { OSCancelThread(&chain.middle.sdk); },
           "Native cancellation falsely retired a real lock holder/waiter");
    Reject([&] { OSCancelThread(&chain.low.sdk); },
           "Native cancellation falsely retired a real holding message waiter");
    Reject([] { DrainNativeThreadLifetimes(); },
           "Host registry retirement accepted live original mutex/entry owners");
    Check(chain.first.thread == &chain.low.sdk && chain.second.thread == &chain.middle.sdk
          && OSGetThreadPriority(&chain.middle.sdk) == 4 && OSGetThreadPriority(&chain.low.sdk) == 4,
          "Rejected lifecycle operations changed actual original locks/donations");
    Order(chain.release.queueReceive, {&chain.low.sdk});
    Order(chain.first.queue, {&chain.middle.sdk});
    Order(chain.second.queue, {&chain.high.sdk});
    Check(OSSetThreadPriority(&chain.high.sdk, 9)
          && OSGetThreadPriority(&chain.middle.sdk) == 9 && OSGetThreadPriority(&chain.low.sdk) == 9,
          "Actual chain priority reduction retained a stale donation");
    Check(OSSetThreadPriority(&chain.low.sdk, 18) && OSGetThreadPriority(&chain.low.sdk) == 9,
          "Held-message owner base request lost an actual mutex donation");
    Check(OSSetThreadPriority(&chain.middle.sdk, 7) && OSGetThreadPriority(&chain.low.sdk) == 7,
          "Original lower owner base did not propagate through its actual wait");
    Check(OSSetThreadPriority(&chain.high.sdk, 2) && OSGetThreadPriority(&chain.low.sdk) == 2,
          "Actual chain boost did not propagate after a base update");
    Check(OSSuspendThread(&chain.middle.sdk) == 0 && chain.middle.sdk.priority == 32
          && OSGetThreadPriority(&chain.low.sdk) == 18,
          "Suspended real lock holder/waiter did not remove its upstream donation");
    Check(OSSetThreadPriority(&chain.high.sdk, 1) && chain.middle.sdk.priority == 32
          && __OSGetEffectivePriority(&chain.middle.sdk) == 1
          && OSGetThreadPriority(&chain.low.sdk) == 18,
          "Original suspended-owner barrier was bypassed by inheritance");
    Check(OSSetThreadPriority(&chain.middle.sdk, 6) && chain.middle.sdk.priority == 32,
          "Original suspended owner base update changed its actual wait sentinel");
    Check(OSResumeThread(&chain.middle.sdk) == 1 && chain.middle.sdk.priority == 1
          && OSGetThreadPriority(&chain.low.sdk) == 1,
          "Real resumed holder/waiter did not recompute and propagate its owned donation");
    Check(OSResumeThread(&chain.middle.sdk) == 0 && OSGetThreadPriority(&chain.low.sdk) == 1,
          "Redundant original Resume erased a genuine inherited priority");
    Check(OSSuspendThread(&chain.low.sdk) == 0 && chain.low.sdk.priority == 32,
          "Actual holding message-waiter could not suspend at its real boundary");
    Check(OSSetThreadPriority(&chain.high.sdk, 3) && chain.low.sdk.priority == 32
          && __OSGetEffectivePriority(&chain.low.sdk) == 3,
          "Suspended actual message owner lost its retained donation contract");
    Check(OSResumeThread(&chain.low.sdk) == 1 && OSGetThreadPriority(&chain.low.sdk) == 3,
          "Real holding-message Resume discarded original effective priority");
    Check(OSSendMessage(&chain.release, &chain, OS_MESSAGE_NO_FLAGS),
          "Original real message could not release the lock owner");
    Join(chain.low, &chain.low);
    Join(chain.middle, &chain.middle);
    Join(chain.high, &chain.high);
    Check(chain.low.sdk.priority == 18 && chain.middle.sdk.priority == 6 && chain.high.sdk.priority == 3,
          "Actual source unlocks did not restore the three real requested base priorities");
    Check(!chain.first.thread && !chain.second.thread && !chain.first.queue.head && !chain.second.queue.head,
          "Actual original chain release retained a source mutex waiter/owner");
    DrainNativeThreadLifetimes();
}
}

int main(int argc, char** argv) {
    try {
        if (argc == 2 && std::strcmp(argv[1], "--retire-held-mutex") == 0) {
            // A default/TLS SDK caller that returns while holding a source
            // mutex must fail terminally, not erase the owner and leave a
            // dangling original mutex. The child observes that real refusal.
            static std::atomic<bool> held{};
            std::set_terminate([] { std::_Exit(held.load() ? 0 : 1); });
            OSMutex mutex{};
            OSInitMutex(&mutex);
            std::thread borrower([&] {
                OSLockMutex(&mutex);
                auto* owner = OSGetCurrentThread();
                Check(owner->queueMutex.head == &mutex && mutex.thread == owner,
                      "Original unmanaged caller did not hold its real mutex");
                held = true;
            });
            borrower.join();
            std::fputs("Unmanaged caller retirement silently lost its source mutex owner\n", stderr);
            return 1;
        }
        Check(argc == 1, "Unexpected original mutex fixture mode");
        auto* caller = OSGetCurrentThread();
        auto* context = OSGetCurrentContext();
        Contention(caller);
        ChainInheritance();
        {
            Mask mask;
            Check(__OSGetEffectivePriority(caller) == 16 && !NativeInterruptsEnabled()
                  && OSGetCurrentContext() == context,
                  "Original priority query changed its caller mask/context");
        }
        Check(NativeInterruptsEnabled() && OSGetCurrentThread() == caller
              && !caller->mutex && !caller->queueMutex.head && !caller->queueMutex.tail,
              "Original mutex lifecycle left the caller or a borrowed owner unfinished");
        std::printf("Whole original OSMutex: %u checks, six genuinely contended/joined workers; recursive ownership, sorted real waits, inherited/base changes, two-lock chain, held/waiting suspend/resume and original unlock deboost PASS\n", checks.load());
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "Original mutex lifetime failed: %s\n", error.what());
        std::fflush(nullptr);
        std::_Exit(1); // Keep any failed live source descriptor/lock; no forced cancellation.
    }
}
