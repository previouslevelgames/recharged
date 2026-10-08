#include "platform/thread_queues.h"
#include "platform/thread_registry_abi.h"
#include "platform/interrupts.h"
#include "platform/thread.h"

#include <dolphin/os.h>
#include <dolphin/os/OSThread.h>

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <exception>
#include <limits>
#include <memory>
#include <thread>
#include <system_error>
#include <mutex>
#include <stdexcept>
#include <unordered_map>
#include <vector>

namespace {
using namespace mscharged::platform;

struct NativeThread;
struct Threads {
    std::mutex latch;
    std::unordered_map<OSThread*, NativeThread*> live;
    std::unordered_map<OSThread*, std::shared_ptr<NativeThread>> managed;
    OSThreadQueue active{};
    std::uint64_t next_alarm_tag{0x80000000u};
    // Original OSThread.c Reschedule is a signed disable count. Physical
    // workers use this same registry's entry/wait boundaries, not a second
    // source scheduler or replacement runnable list.
    s32 reschedule{};
    NativeThread* scheduler_owner{};
    NativeThread* power_owner{};
};
Threads& State() { static Threads state; return state; }
void AppendActive(Threads& state, NativeThread& native);
void RemoveActive(Threads& state, NativeThread& native);

class Mask {
public:
    Mask() : previous_(OSDisableInterrupts()) {}
    ~Mask() { OSRestoreInterrupts(previous_); }
    Mask(const Mask&) = delete;
    Mask& operator=(const Mask&) = delete;
private:
    BOOL previous_;
};

struct NativeThread {
    OSThread local_sdk{};
    OSThread& sdk;
    std::condition_variable changed;
    bool waiting{}, servicing{};
    bool parked{}, terminal_stopped{};
    // A genuine initial Resume can park its lower-priority caller until the
    // resumed incarnation actually blocks or returns. This is execution
    // bookkeeping, with no source queue or readiness acknowledgement.
    bool initial_handoff_parked{};
    NativeThread* initial_resumer{};
    OSContext* execution_context{};
    NativeThreadWaitService service{};
    // Created SDK descriptors are borrowed from the actual caller. Host stack
    // backing and native execution resources are separate physical ABI state.
    bool managed{}, started{}, completed{}, cancelled{}, active{}, self_suspended{};
    s32 suspend_count{};
    u32 alarm_tag{};
    // Only failed native wait unwinding consumes this bookkeeping credit.
    // The first real resume consumes it; later suspension requests stay counted.
    bool self_suspend_pending{};
    std::thread worker;
    void* (*entry)(void*){};
    void* argument{};
    std::exception_ptr failure;
    std::uintptr_t logical_stack_low{}, logical_stack_high{};
    u32 logical_stack_bytes{};

    explicit NativeThread(OSThread& borrowed) : sdk(borrowed), managed(true) {}


    NativeThread() : sdk(local_sdk) {
        // The original default thread uses priority 16 and detached state.
        // Execution/register switching belongs to the native OS; the canonical
        // SDK object retains actual native stack bounds and opaque context data.
        const auto stack = mscharged::CurrentThreadStackLimits();
        sdk.state = OS_THREAD_STATE_RUNNING;
        sdk.attr = OS_THREAD_ATTR_DETACH;
        sdk.priority = sdk.base = 16;
        sdk.val = reinterpret_cast<void*>(std::uintptr_t{0xffffffffu});
        sdk.stackBase = reinterpret_cast<u8*>(stack.high);
        sdk.stackEnd = reinterpret_cast<u8*>(stack.low);
        execution_context = OSGetCurrentContext();
        if (execution_context) sdk.context = *execution_context;
        NativeInterruptGuard exclusion;
        auto& state = State();
        std::lock_guard lock(state.latch);
        if (state.reschedule > 0)
            throw std::logic_error("New native SDK caller has no disabled-scheduler entry boundary");
        state.live.emplace(&sdk, this);
        // Original __OSThreadInit includes its default thread in this same list.
        AppendActive(state, *this);
    }

    ~NativeThread() {
        if (managed) {
            // Actual source callbacks have ended before this metadata can retire.
            // Never inspect a borrowed descriptor after its owner may free it.
            if (worker.joinable()) std::terminate();
            return;
        }
        // A returning native thread cannot retain a source critical section.
        // No source waiter is completed by thread retirement.
        OSEnableInterrupts();
        NativeInterruptGuard exclusion;
        auto& state = State();
        std::lock_guard lock(state.latch);
        if (state.reschedule > 0 && state.scheduler_owner == this) std::terminate();
        if (waiting || initial_handoff_parked || initial_resumer ||
                sdk.queue || sdk.mutex || sdk.queueMutex.head || sdk.queueMutex.tail)
            std::terminate();
        sdk.state = OS_THREAD_STATE_MORIBUND;
        RemoveActive(state, *this);
        state.live.erase(&sdk);
    }
};

thread_local NativeThread* bound_thread{};
NativeThread& Current() {
    if (bound_thread) return *bound_thread;
    thread_local NativeThread thread;
    return thread;
}

NativeThread& Find(Threads& state, OSThread* thread) {
    const auto found = state.live.find(thread);
    if (found == state.live.end())
        throw std::logic_error("SDK thread is not a live native caller");
    return *found->second;
}

bool SchedulerAllows(const Threads& state, const NativeThread& native) {
    return state.reschedule <= 0 || state.scheduler_owner == &native;
}

bool Runnable(const Threads& state, const NativeThread& native) {
    return !native.terminal_stopped && !native.waiting && native.suspend_count == 0 && SchedulerAllows(state, native);
}

void ValidateQueue(Threads& state, OSThreadQueue* queue) {
    OSThread* previous{};
    std::size_t count{};
    for (auto* thread = queue->head; thread; thread = thread->link.next) {
        if (++count > state.live.size())
            throw std::logic_error("Native SDK wait queue has a cycle");
        auto& native = Find(state, thread);
        if (!native.waiting || thread->queue != queue ||
                thread->state != OS_THREAD_STATE_WAITING ||
                thread->link.prev != previous || thread->suspend != native.suspend_count)
            throw std::logic_error("Native SDK wait queue ownership is unqualified");
        if (previous && previous->priority > thread->priority)
            throw std::logic_error("Native SDK wait queue priority order is invalid");
        previous = thread;
    }
    if (previous != queue->tail)
        throw std::logic_error("Native SDK wait queue tail is invalid");
}

void Remove(OSThreadQueue* queue, OSThread* thread) {
    if (thread->link.prev) thread->link.prev->link.next = thread->link.next;
    else queue->head = thread->link.next;
    if (thread->link.next) thread->link.next->link.prev = thread->link.prev;
    else queue->tail = thread->link.prev;
    thread->queue = nullptr;
    thread->link = {};
}

void Insert(OSThreadQueue* queue, OSThread* thread) {
    // Source SDK order: ascending priority, behind existing equal priorities.
    auto* next = queue->head;
    while (next && next->priority <= thread->priority) next = next->link.next;
    auto* previous = next ? next->link.prev : queue->tail;
    thread->queue = queue;
    thread->link = {next, previous};
    if (previous) previous->link.next = thread;
    else queue->head = thread;
    if (next) next->link.prev = thread;
    else queue->tail = thread;
}

void ValidatePriorityOwner(Threads& state, OSThread* thread) {
    auto& native = Find(state, thread);
    if (thread->suspend != native.suspend_count ||
            thread->base < OS_PRIORITY_MIN || thread->base > OS_PRIORITY_MAX)
        throw std::logic_error("Native SDK priority owner/counter is invalid");
}

s32 EffectivePriority(Threads& state, OSThread* thread) {
    ValidatePriorityOwner(state, thread);
    s32 priority = thread->base;
    OSMutex* previous{};
    std::vector<OSMutex*> visited;
    // Original __OSGetEffectivePriority: the minimum base and first waiter
    // priority among the actual mutexes held by this same source thread.
    for (auto* mutex = thread->queueMutex.head; mutex; mutex = mutex->link.next) {
        for (auto* seen : visited)
            if (seen == mutex) throw std::logic_error("Native SDK held-mutex list has a cycle");
        visited.push_back(mutex);
        if (mutex->thread != thread || mutex->count <= 0 || mutex->link.prev != previous)
            throw std::logic_error("Native SDK held-mutex owner/list is invalid");
        ValidateQueue(state, &mutex->queue);
        if (auto* waiter = mutex->queue.head)
            if (waiter->priority < priority) priority = waiter->priority;
        previous = mutex;
    }
    if (previous != thread->queueMutex.tail)
        throw std::logic_error("Native SDK held-mutex tail is invalid");
    return priority;
}

OSThread* SetEffectivePriority(Threads& state, OSThread* thread, s32 priority) {
    auto& native = Find(state, thread);
    switch (thread->state) {
    case OS_THREAD_STATE_WAITING: {
        if (!native.waiting || !thread->queue)
            throw std::logic_error("Native SDK priority waiter lacks its real queue");
        auto* queue = thread->queue;
        ValidateQueue(state, queue);
        OSThread* owner{};
        if (thread->mutex) {
            owner = thread->mutex->thread;
            if (queue != &thread->mutex->queue || !owner || owner == thread)
                throw std::logic_error("Native SDK mutex wait owner is invalid");
            (void)Find(state, owner);
        }
        Remove(queue, thread);
        thread->priority = priority;
        Insert(queue, thread); // Behind existing equal priorities, as in OSThread.c.
        if (owner) return owner;
        break;
    }
    case OS_THREAD_STATE_READY:
    case OS_THREAD_STATE_RUNNING:
        // Native execution owns physical runnable threads; the canonical SDK
        // priority and source wait order remain in this sole registry.
        thread->priority = priority;
        break;
    default:
        break;
    }
    return nullptr;
}

void UpdatePriority(Threads& state, OSThread* thread) {
    std::size_t changes{};
    while (thread) {
        ValidatePriorityOwner(state, thread);
        if (thread->suspend > 0) return;
        const auto priority = EffectivePriority(state, thread);
        if (thread->priority == priority) return;
        if (++changes > state.live.size())
            throw std::logic_error("Native SDK priority inheritance failed to converge");
        thread = SetEffectivePriority(state, thread, priority);
    }
}

void PromoteThread(Threads& state, OSThread* thread, s32 priority) {
    std::size_t changes{};
    while (thread) {
        ValidatePriorityOwner(state, thread);
        if (thread->suspend > 0 || thread->priority <= priority) return;
        if (++changes > state.live.size())
            throw std::logic_error("Native SDK priority promotion failed to converge");
        thread = SetEffectivePriority(state, thread, priority);
    }
}

void Wait(NativeThread& native, std::unique_lock<std::mutex>& lock) {
    auto& state = State();
    struct ParkedWait {
        NativeThread& native;
        explicit ParkedWait(NativeThread& thread) : native(thread) { native.parked = true; }
        ~ParkedWait() { native.parked = false; }
    } parked(native);
    while (!Runnable(state, native)) {
        if (native.terminal_stopped) {
            // Hardware context cancellation in the final power path cannot
            // unwind original C++ frames. Keep this actual wait/stack alive
            // until process removal, including across spurious native wakes.
            native.changed.wait(lock, [] { return false; });
        }
        if (!SchedulerAllows(state, native)) {
            // An IRQ on the current owner may wake this source queue, but the
            // other SDK thread cannot resume source frames or service hardware
            // until the original scheduler count permits a selection.
            native.changed.wait(lock, [&] { return SchedulerAllows(state, native); });
            continue;
        }
        if (!native.service) {
            native.changed.wait(lock, [&] { return Runnable(state, native) || !SchedulerAllows(state, native); });
            continue;
        }
        if (native.changed.wait_for(lock, std::chrono::milliseconds(1),
                [&] { return Runnable(state, native) || !SchedulerAllows(state, native); })) continue;
        native.servicing = true;
        const auto service = native.service;
        lock.unlock();
        try { service(); }
        catch (...) {
            lock.lock();
            native.servicing = false;
            throw;
        }
        lock.lock();
        native.servicing = false;
    }
}

void WaitToRun(NativeThread& native) {
    auto& state = State();
    for (;;) {
        std::unique_lock lock(state.latch);
        Wait(native, lock);
        lock.unlock();
        OSDisableInterrupts();
        lock.lock();
        // Disable/Suspend may win the race between condition wake and actual
        // IRQ exclusion. Recheck before any original source frame resumes.
        if (Runnable(state, native)) {
            native.sdk.state = OS_THREAD_STATE_RUNNING;
            return;
        }
        lock.unlock();
        OSEnableInterrupts();
    }
}

constexpr u16 kExited = 0; // Original OSThread.c OS_THREAD_STATE_EXITED.

void AppendActive(Threads& state, NativeThread& native) {
    auto* thread = &native.sdk;
    thread->linkActive = {nullptr, state.active.tail};
    if (state.active.tail) state.active.tail->linkActive.next = thread;
    else state.active.head = thread;
    state.active.tail = thread;
    native.active = true;
}
void RemoveActive(Threads& state, NativeThread& native) {
    if (!native.active) return;
    auto* thread = &native.sdk;
    if (thread->linkActive.prev) thread->linkActive.prev->linkActive.next = thread->linkActive.next;
    else state.active.head = thread->linkActive.next;
    if (thread->linkActive.next) thread->linkActive.next->linkActive.prev = thread->linkActive.prev;
    else state.active.tail = thread->linkActive.prev;
    native.active = false;
}
void Wake(Threads& state, OSThreadQueue* queue) {
    ValidateQueue(state, queue);
    while (auto* thread = queue->head) {
        auto& native = Find(state, thread);
        Remove(queue, thread);
        thread->state = OS_THREAD_STATE_READY;
        native.waiting = false;
        native.changed.notify_one();
    }
}
void NotifyInitialResumer(NativeThread& native) {
    if (native.initial_resumer) native.initial_resumer->changed.notify_one();
}

bool InitialBoundary(const NativeThread& native) {
    // Completion is native metadata; never dereference an already retired SDK
    // descriptor. Entry alone is insufficient: its source queue may not exist.
    return native.completed || (native.started &&
        (native.waiting || (native.self_suspended && native.suspend_count > 0)));
}

void WaitForInitialBoundary(Threads& state, NativeThread& caller,
        const std::shared_ptr<NativeThread>& peer) {
    for (;;) {
        std::unique_lock lock(state.latch);
        caller.changed.wait(lock, [&] { return InitialBoundary(*peer); });
        lock.unlock();
        OSDisableInterrupts();
        lock.lock();
        // A genuine wake may have won the native mask before the resumer. It
        // must wait for the higher-priority source continuation to block again.
        if (InitialBoundary(*peer)) {
            if (peer->initial_resumer != &caller || !caller.initial_handoff_parked ||
                    caller.sdk.state != OS_THREAD_STATE_READY || caller.suspend_count ||
                    caller.sdk.suspend || caller.waiting || caller.sdk.queue)
                throw std::logic_error("Native initial-resume continuation ownership changed");
            peer->initial_resumer = nullptr;
            caller.initial_handoff_parked = false;
            caller.sdk.state = OS_THREAD_STATE_RUNNING;
            return;
        }
        lock.unlock();
        OSEnableInterrupts();
    }
}

void Complete(Threads& state, NativeThread& native, void* result) {
    if (native.completed) return;
    if (state.reschedule > 0 && state.scheduler_owner == &native)
        throw std::logic_error("Native SDK owner returned with original scheduling disabled");
    if (native.waiting || native.initial_handoff_parked || native.sdk.queue || native.sdk.mutex ||
            native.sdk.queueMutex.head || native.sdk.queueMutex.tail)
        throw std::logic_error("Native SDK exit still borrows an unqualified wait/mutex owner");
    OSClearContext(&native.sdk.context);
    if (native.sdk.attr & OS_THREAD_ATTR_DETACH) {
        RemoveActive(state, native);
        native.sdk.state = kExited;
    } else {
        native.sdk.val = result;
        native.sdk.state = OS_THREAD_STATE_MORIBUND;
    }
    Wake(state, &native.sdk.queueJoin);
    native.completed = true;
    NotifyInitialResumer(native);
    native.changed.notify_all();
}
void Run(const std::shared_ptr<NativeThread>& native) noexcept {
    OSContext* prior_context = OSGetCurrentContext();
    void* result{};
    try {
        auto& state = State();
        {
            std::unique_lock lock(state.latch);
            native->changed.wait(lock, [&] { return native->cancelled || Runnable(state, *native); });
            if (native->cancelled) return; // Cancel joined this parked host task; no source entry ran.
        }
        const auto stack = mscharged::CurrentThreadStackLimits();
        {
            Mask mask;
            std::lock_guard lock(state.latch);
            if (native->cancelled) return;
            // A concurrent source Suspend may precede the first native entry.
            if (!Runnable(state, *native)) {
                // Re-enter the genuine park, rather than execute a suspended source worker.
                // This is handled below without retaining hardware exclusion.
            } else {
                native->started = true;
                native->sdk.state = OS_THREAD_STATE_RUNNING;
                native->sdk.stackBase = reinterpret_cast<u8*>(stack.high);
                native->sdk.stackEnd = reinterpret_cast<u8*>(stack.low);
            }
        }
        while (!native->started) {
            std::unique_lock lock(state.latch);
            native->changed.wait(lock, [&] { return native->cancelled || Runnable(state, *native); });
            if (native->cancelled) return;
            lock.unlock();
            Mask mask;
            lock.lock();
            if (native->cancelled) return;
            if (!Runnable(state, *native)) continue;
            native->started = true;
            native->sdk.state = OS_THREAD_STATE_RUNNING;
            native->sdk.stackBase = reinterpret_cast<u8*>(stack.high);
            native->sdk.stackEnd = reinterpret_cast<u8*>(stack.low);
        }
        bound_thread = native.get();
        native->execution_context = &native->sdk.context;
        OSSetCurrentContext(native->execution_context);
        result = native->entry(native->argument);
    } catch (...) {
        native->failure = std::current_exception();
    }
    // Source execution and all of its ordinary frame destructors have ended.
    // No source callbacks or descriptor/context access occurs after completion.
    OSEnableInterrupts();
    OSSetCurrentContext(prior_context);
    bound_thread = nullptr;
    try {
        Mask mask;
        auto& state = State();
        std::lock_guard lock(state.latch);
        Complete(state, *native, result);
    } catch (...) {
        // An outstanding borrowed wait/mutex is not successful termination.
        // The platform cannot safely let its caller free the source descriptor.
        std::terminate();
    }
}
std::shared_ptr<NativeThread> Managed(Threads& state, OSThread* thread) {
    const auto found = state.managed.find(thread);
    if (found == state.managed.end())
        throw std::logic_error("SDK lifecycle requires a genuinely created native thread");
    if (found->second->terminal_stopped)
        throw std::logic_error("Terminally stopped native source context cannot be reused");
    return found->second;
}
void JoinHost(const std::shared_ptr<NativeThread>& native) {
    if (native->worker.joinable()) {
        if (native->worker.get_id() == std::this_thread::get_id())
            throw std::logic_error("Cannot retire a native SDK worker from its own entry");
        native->worker.join();
    }
}
} // namespace

extern "C" OSThread* OSGetCurrentThread() { return &Current().sdk; }

extern "C" s32 __OSGetEffectivePriority(OSThread* thread) {
    Mask mask;
    auto& state = State();
    std::lock_guard lock(state.latch);
    return EffectivePriority(state, thread);
}

extern "C" void __OSPromoteThread(OSThread* thread, s32 priority) {
    if (priority < OS_PRIORITY_MIN || priority > OS_PRIORITY_MAX + 1)
        throw std::invalid_argument("Native SDK inherited priority is invalid");
    Mask mask;
    auto& state = State();
    std::lock_guard lock(state.latch);
    PromoteThread(state, thread, priority);
}

extern "C" s32 OSDisableScheduler() {
    Mask mask;
    auto& current = Current();
    auto& state = State();
    std::lock_guard lock(state.latch);
    if (state.reschedule == std::numeric_limits<s32>::max())
        throw std::overflow_error("Native SDK scheduler disable counter overflow");
    if (state.reschedule > 0 && state.scheduler_owner != &current)
        throw std::logic_error("Native SDK scheduling is disabled by another caller");
    if (state.reschedule == 0) {
        // No native signal, forced suspension or C++ frame destruction is a
        // Wii context-switch equivalent. Only genuinely parked peers qualify.
        for (const auto& entry : state.live) {
            const auto& peer = *entry.second;
            if (&peer != &current && (peer.servicing ||
                    peer.sdk.state == OS_THREAD_STATE_RUNNING))
                throw std::logic_error("Native SDK running peer has no cooperative scheduler boundary");
        }
        state.scheduler_owner = &current;
    }
    return state.reschedule++;
}

extern "C" s32 OSEnableScheduler() {
    Mask mask;
    auto& current = Current();
    auto& state = State();
    std::lock_guard lock(state.latch);
    if (state.reschedule == std::numeric_limits<s32>::min())
        throw std::overflow_error("Native SDK scheduler enable counter overflow");
    if (state.reschedule > 0 && state.scheduler_owner != &current)
        throw std::logic_error("Native SDK scheduler enable requires its disabling caller");
    if (state.power_owner)
        throw std::logic_error("Final native power removal cannot re-enable source scheduling");
    const auto prior = state.reschedule--;
    if (prior == 1) {
        state.scheduler_owner = nullptr;
        for (const auto& entry : state.live) entry.second->changed.notify_all();
    }
    // Original OSEnableScheduler does not clamp unbalanced negative counts.
    return prior;
}

extern "C" void OSInitThreadQueue(OSThreadQueue* queue) {
    if (!queue) throw std::invalid_argument("SDK thread queue is null");
    Mask mask;
    auto& state = State();
    std::lock_guard lock(state.latch);
    // Constructor input need not be initialized. Only existing live borrowed
    // waiters forbid reinitialization; unknown old bytes are not dereferenced.
    for (const auto& entry : state.live)
        if (entry.second->waiting && entry.first->queue == queue)
            throw std::logic_error("Cannot reinitialize a borrowed native wait queue");
    queue->tail = nullptr;
    queue->head = nullptr;
}

extern "C" void OSWakeupThread(OSThreadQueue* queue) {
    if (!queue) throw std::invalid_argument("SDK thread queue is null");
    Mask mask;
    auto& state = State();
    std::lock_guard lock(state.latch);
    Wake(state, queue);
}

extern "C" void OSSleepThread(OSThreadQueue* queue) {
    if (!queue) throw std::invalid_argument("SDK thread queue is null");
    if (!NativeInterruptWaitAllowed())
        throw std::logic_error("Cannot sleep a native SDK interrupt/host exclusion scope");
    Mask mask;
    auto& native = Current();
    auto& state = State();
    {
        std::lock_guard lock(state.latch);
        if (state.reschedule > 0 && state.scheduler_owner == &native)
            throw std::logic_error("Native SDK disabled-scheduler caller cannot deschedule");
        if (native.waiting || native.servicing || native.sdk.queue ||
                native.sdk.state != OS_THREAD_STATE_RUNNING || native.sdk.suspend != 0)
            throw std::logic_error("Native SDK thread wait state is unqualified");
        (void)EffectivePriority(state, &native.sdk);
        // Whole original OSLockMutex publishes mutex before this actual sleep;
        // source holders may also wait on messages while retaining their locks.
        if (native.sdk.mutex) {
            auto* owner = native.sdk.mutex->thread;
            if (queue != &native.sdk.mutex->queue || !owner || owner == &native.sdk ||
                    native.sdk.mutex->count <= 0)
                throw std::logic_error("Native SDK source mutex wait is invalid");
            (void)Find(state, owner);
        }
        ValidateQueue(state, queue);
        native.sdk.state = OS_THREAD_STATE_WAITING;
        native.waiting = true;
        Insert(queue, &native.sdk);
        NotifyInitialResumer(native);
    }

    // A descheduled Wii thread does not keep hardware globally masked. Enqueue
    // first, then release the native mask; the wait predicate prevents a lost
    // wake in the gap before the actual condition-variable wait.
    OSEnableInterrupts();
    try {
        WaitToRun(native);
    } catch (...) {
        // Unwind a failed host service without leaving borrowed source storage
        // linked or manufacturing successful source completion.
        OSDisableInterrupts();
        std::lock_guard lock(state.latch);
        if (native.waiting) Remove(queue, &native.sdk);
        native.waiting = native.servicing = false;
        native.sdk.state = OS_THREAD_STATE_RUNNING;
        throw;
    }
}

extern "C" BOOL OSSetThreadPriority(OSThread* thread, OSPriority priority) {
    if (priority < OS_PRIORITY_MIN || priority > OS_PRIORITY_MAX) return FALSE;
    Mask mask;
    auto& state = State();
    std::lock_guard lock(state.latch);
    ValidatePriorityOwner(state, thread);
    if (thread->base != priority) {
        const auto& native = Find(state, thread);
        if (native.initial_resumer || native.initial_handoff_parked)
            throw std::logic_error("Native initial-resume base reprioritization has no rescheduling boundary");
        thread->base = priority;
        UpdatePriority(state, thread);
    }
    return TRUE;
}

extern "C" s32 OSGetThreadPriority(OSThread* thread) {
    Mask mask;
    auto& state = State();
    std::lock_guard lock(state.latch);
    (void)Find(state, thread);
    return thread->priority;
}

namespace mscharged::platform {
NativeThreadWaitService SetNativeThreadWaitService(NativeThreadWaitService service) {
    if (NativeInterruptDispatchActive())
        throw std::logic_error("Cannot replace native wait service inside an interrupt");
    Mask mask;
    auto& native = Current();
    auto& state = State();
    std::lock_guard lock(state.latch);
    if (native.waiting || native.servicing)
        throw std::logic_error("Cannot replace an active native wait service");
    const auto previous = native.service;
    native.service = service;
    return previous;
}

std::uint32_t NativeThreadAlarmTag() {
    if (!NativeInterruptWaitAllowed())
        throw std::logic_error("Cannot register a native timed sleep in an interrupt/host exclusion");
    Mask mask;
    auto& native = Current();
    auto& state = State();
    std::lock_guard lock(state.latch);
    if (state.reschedule > 0 && state.scheduler_owner == &native)
        throw std::logic_error("Native timed sleep cannot deschedule a disabled-scheduler caller");
    if (!native.service || native.waiting || native.servicing || native.self_suspended ||
            native.sdk.state != OS_THREAD_STATE_RUNNING || native.sdk.suspend != 0 ||
            native.suspend_count != 0 || native.sdk.queue || native.sdk.mutex ||
            native.sdk.queueMutex.head || native.sdk.queueMutex.tail)
        throw std::logic_error("Native timed sleep requires its runnable owner and hardware wait service");
    if (!native.alarm_tag) {
        if (state.next_alarm_tag > std::numeric_limits<u32>::max())
            throw std::overflow_error("Native SDK thread alarm identities exhausted");
        // Original tags are u32 cancellation identities. Keep that width and
        // uniqueness across descriptor/TLS reuse without casting a host pointer.
        native.alarm_tag = static_cast<u32>(state.next_alarm_tag++);
    }
    return native.alarm_tag;
}
} // namespace mscharged::platform


extern "C" BOOL OSCreateThread(OSThread* thread, void* (*entry)(void*), void* argument,
        void* stack_begin, u32 stack_bytes, OSPriority priority, u16 attributes) {
    if (priority < OS_PRIORITY_MIN || priority > OS_PRIORITY_MAX) return FALSE;
    if (!thread || !entry || !stack_begin || stack_bytes < 8 ||
            reinterpret_cast<std::uintptr_t>(thread) % alignof(OSThread) ||
            reinterpret_cast<std::uintptr_t>(stack_begin) < stack_bytes)
        throw std::invalid_argument("Native SDK thread descriptor/entry/logical stack ABI is invalid");
    if (!NativeInterruptWaitAllowed())
        throw std::logic_error("Cannot create a native SDK worker in an interrupt/host exclusion");
    auto& state = State();
    std::shared_ptr<NativeThread> old;
    {
        Mask mask;
        std::lock_guard lock(state.latch);
        if (const auto found = state.live.find(thread); found != state.live.end()) {
            old = Managed(state, thread);
            if (!old->completed || old->active || old->initial_resumer || old->initial_handoff_parked)
                throw std::logic_error("Native SDK thread descriptor is still borrowed by an active lifetime");
        }
    }
    if (old) JoinHost(old);
    auto native = std::make_shared<NativeThread>(*thread);
    native->entry = entry;
    native->argument = argument;
    native->logical_stack_high = reinterpret_cast<std::uintptr_t>(stack_begin);
    native->logical_stack_low = native->logical_stack_high - stack_bytes;
    native->logical_stack_bytes = stack_bytes;
    native->suspend_count = 1;
    Mask mask;
    std::lock_guard lock(state.latch);
    if (old) { state.live.erase(thread); state.managed.erase(thread); }
    thread->context = {}; // Native opaque execution context, not PowerPC code/register execution.
    thread->state = OS_THREAD_STATE_READY;
    thread->attr = attributes & OS_THREAD_ATTR_DETACH;
    thread->base = thread->priority = priority;
    thread->suspend = 1;
    thread->val = reinterpret_cast<void*>(std::uintptr_t{0xffffffffu});
    thread->mutex = nullptr;
    thread->queue = nullptr;
    thread->link = {};
    thread->queueJoin = {};
    thread->queueMutex = {};
    thread->stackBase = static_cast<u8*>(stack_begin);
    thread->stackEnd = reinterpret_cast<u8*>(native->logical_stack_low);
    const u32 zero = 0, magic = OS_THREAD_STACK_MAGIC;
    const auto aligned_top = native->logical_stack_high & ~std::uintptr_t{7};
    if (aligned_top < native->logical_stack_low + 8)
        throw std::invalid_argument("Native SDK logical stack has no aligned source prologue");
    std::memcpy(reinterpret_cast<void*>(aligned_top-8), &zero, sizeof(zero));
    std::memcpy(reinterpret_cast<void*>(aligned_top-4), &zero, sizeof(zero));
    std::memcpy(reinterpret_cast<void*>(native->logical_stack_low), &magic, sizeof(magic));
    thread->error = 0;
    thread->specific[0] = thread->specific[1] = nullptr;
    state.live.emplace(thread, native.get());
    state.managed.emplace(thread, native);
    AppendActive(state, *native);
    try { native->worker = std::thread(Run, native); }
    catch (const std::system_error&) {
        RemoveActive(state, *native);
        state.live.erase(thread); state.managed.erase(thread);
        thread->state = kExited;
        return FALSE;
    }
    return TRUE;
}

extern "C" BOOL OSIsThreadTerminated(OSThread* thread) {
    Mask mask;
    auto& state = State();
    std::lock_guard lock(state.latch);
    const auto found = state.managed.find(thread);
    if (found != state.managed.end()) {
        if (found->second->completed && found->second->failure)
            std::rethrow_exception(found->second->failure);
        return found->second->completed || found->second->terminal_stopped ? TRUE : FALSE;
    }
    (void)Find(state, thread);
    return thread->state == OS_THREAD_STATE_MORIBUND || thread->state == kExited;
}
extern "C" BOOL OSIsThreadSuspended(OSThread* thread) {
    Mask mask;
    auto& state = State();
    std::lock_guard lock(state.latch);
    auto& native = Find(state, thread);
    if (native.sdk.suspend != native.suspend_count)
        throw std::logic_error("Native SDK suspend counter was changed outside its lifecycle");
    return native.suspend_count > 0;
}
extern "C" s32 OSResumeThread(OSThread* thread) {
    auto& caller = Current();
    const bool may_wait = NativeInterruptWaitAllowed();
    Mask mask;
    auto& state = State();
    std::unique_lock lock(state.latch);
    auto& native = Find(state, thread);
    if (native.terminal_stopped)
        throw std::logic_error("Terminally stopped native source context cannot resume");
    ValidatePriorityOwner(state, thread);
    const auto prior = native.suspend_count;
    std::shared_ptr<NativeThread> initial_peer;
    if (prior == 1 && native.managed && !native.started && !native.completed &&
            !native.waiting && native.sdk.state == OS_THREAD_STATE_READY &&
            state.reschedule <= 0 && EffectivePriority(state, thread) < caller.sdk.priority) {
        // Original SelectThread(FALSE) preempts only for a strictly higher
        // ready priority. Arbitrary running-peer preemption remains held.
        if (!may_wait || caller.servicing || caller.initial_handoff_parked ||
                caller.waiting || caller.sdk.state != OS_THREAD_STATE_RUNNING ||
                caller.suspend_count || caller.sdk.queue ||
                OSGetCurrentContext() != caller.execution_context)
            throw std::logic_error("Native initial Resume has no cooperative caller/context boundary");
        for (const auto& entry : state.live) {
            const auto& peer = *entry.second;
            if (&peer == &caller || &peer == &native || peer.completed || peer.initial_handoff_parked)
                continue;
            if (peer.servicing || peer.sdk.state == OS_THREAD_STATE_RUNNING ||
                    (!peer.waiting && peer.suspend_count == 0))
                throw std::logic_error("Native initial Resume cannot preempt a running/runnable foreign peer");
        }
        if (native.initial_resumer)
            throw std::logic_error("Native initial Resume already borrows this incarnation");
        initial_peer = Managed(state, thread);
        native.initial_resumer = &caller;
        caller.initial_handoff_parked = true;
        caller.sdk.state = OS_THREAD_STATE_READY;
    }
    if (native.suspend_count > 0) {
        --native.suspend_count;
        if (native.self_suspended && native.self_suspend_pending)
            native.self_suspend_pending = false;
    }
    native.sdk.suspend = native.suspend_count;
    if (prior == 1) {
        if (native.waiting) {
            auto* queue = native.sdk.queue;
            ValidateQueue(state, queue);
            Remove(queue, thread);
            thread->priority = EffectivePriority(state, thread);
            Insert(queue, thread);
            if (thread->mutex) UpdatePriority(state, thread->mutex->thread);
        } else if (native.sdk.state == OS_THREAD_STATE_READY) {
            thread->priority = EffectivePriority(state, thread);
        }
        native.changed.notify_one();
    }
    if (initial_peer) {
        lock.unlock();
        // Preserve a plain caller mask, but do not carry its global native
        // exclusion through the actual higher-priority source execution.
        OSEnableInterrupts();
        WaitForInitialBoundary(state, caller, initial_peer);
    }
    return prior;
}
extern "C" s32 OSSuspendThread(OSThread* thread) {
    if (!NativeInterruptWaitAllowed())
        throw std::logic_error("Cannot suspend native SDK execution in an interrupt/host exclusion");
    Mask mask;
    auto& state = State();
    auto& caller = Current();
    NativeThread* native{};
    s32 prior{};
    bool self_wait{};
    {
        std::lock_guard lock(state.latch);
        native = &Find(state, thread);
        if (native->terminal_stopped)
            throw std::logic_error("Terminally stopped native source context cannot suspend");
        if (native->initial_handoff_parked || (native->initial_resumer && !native->started))
            throw std::logic_error("Native initial-resume continuation cannot be externally suspended");
        if (native == &caller && state.reschedule > 0 && state.scheduler_owner == &caller)
            throw std::logic_error("Native SDK disabled-scheduler caller cannot self-suspend");
        ValidatePriorityOwner(state, thread);
        if (native->completed ||
                native->suspend_count == std::numeric_limits<s32>::max())
            throw std::logic_error("Native SDK suspend state is unqualified");
        if (!native->waiting && native->sdk.state == OS_THREAD_STATE_RUNNING && native != &caller)
            throw std::logic_error("External running native SDK suspension has no qualified preemption boundary");
        prior = native->suspend_count++;
        native->sdk.suspend = native->suspend_count;
        if (prior == 0) {
            if (native->waiting) {
                auto* queue = native->sdk.queue;
                ValidateQueue(state, queue);
                Remove(queue, thread);
                thread->priority = OS_PRIORITY_MAX + 1;
                Insert(queue, thread);
                if (thread->mutex) UpdatePriority(state, thread->mutex->thread);
            } else if (native == &caller && native->sdk.state == OS_THREAD_STATE_RUNNING) {
                native->sdk.state = OS_THREAD_STATE_READY;
                native->self_suspended = true;
                native->self_suspend_pending = true;
                NotifyInitialResumer(*native);
                self_wait = true;
            }
        }
    }
    if (self_wait) {
        OSEnableInterrupts();
        try {
            WaitToRun(*native);
        } catch (...) {
            OSDisableInterrupts();
            std::lock_guard lock(state.latch);
            // No successful source resume is invented for a failed host service.
            // Remove only this wait's unconsumed increment. A real resume may
            // already have consumed it before a later concurrent resuspension.
            if (native->self_suspend_pending) {
                --native->suspend_count;
                native->sdk.suspend = native->suspend_count;
            }
            native->self_suspend_pending = native->self_suspended = false;
            native->sdk.state = native->suspend_count ? OS_THREAD_STATE_READY : OS_THREAD_STATE_RUNNING;
            throw;
        }
        OSDisableInterrupts();
        std::lock_guard lock(state.latch);
        native->self_suspend_pending = native->self_suspended = false;
        native->sdk.state = OS_THREAD_STATE_RUNNING;
    }
    return prior;
}
extern "C" BOOL OSJoinThread(OSThread* thread, void** value) {
    if (!NativeInterruptWaitAllowed())
        throw std::logic_error("Cannot join native SDK execution in an interrupt/host exclusion");
    Mask mask;
    auto& state = State();
    std::shared_ptr<NativeThread> native;
    bool wait{};
    {
        std::lock_guard lock(state.latch);
        native = Managed(state, thread);
        if (native->initial_resumer || native->initial_handoff_parked)
            throw std::logic_error("Native join still borrows an initial-resume continuation");
        wait = !(thread->attr & OS_THREAD_ATTR_DETACH) &&
            thread->state != OS_THREAD_STATE_MORIBUND && thread->queueJoin.head == nullptr;
    }
    if (wait) OSSleepThread(&thread->queueJoin);
    bool success{};
    std::exception_ptr failure;
    {
        std::lock_guard lock(state.latch);
        if (wait && !native->active) return FALSE;
        if (thread->state == OS_THREAD_STATE_MORIBUND) {
            failure = native->failure;
            if (value && !failure) *value = thread->val;
            RemoveActive(state, *native);
            thread->state = kExited;
            success = true;
        }
    }
    if (success) JoinHost(native);
    if (failure) std::rethrow_exception(failure);
    return success ? TRUE : FALSE;
}
extern "C" void OSDetachThread(OSThread* thread) {
    Mask mask;
    auto& state = State();
    std::lock_guard lock(state.latch);
    auto native = Managed(state, thread);
    if (native->initial_resumer || native->initial_handoff_parked)
        throw std::logic_error("Native detach still borrows an initial-resume continuation");
    thread->attr |= OS_THREAD_ATTR_DETACH;
    if (thread->state == OS_THREAD_STATE_MORIBUND) {
        RemoveActive(state, *native);
        thread->state = kExited;
    }
    Wake(state, &thread->queueJoin);
}
extern "C" void OSCancelThread(OSThread* thread) {
    if (!NativeInterruptWaitAllowed())
        throw std::logic_error("Cannot cancel native SDK execution in an interrupt/host exclusion");
    auto& caller = Current();
    auto& state = State();
    std::shared_ptr<NativeThread> native;
    {
        Mask mask;
        std::lock_guard lock(state.latch);
        native = Managed(state, thread);
        if (native->initial_resumer || native->initial_handoff_parked)
            throw std::logic_error("Native cancellation still borrows an initial-resume continuation");
        if (native->completed) return;
        if (native->started && state.power_owner == &caller) {
            // Source KillThreads may cancel a physically parked message waiter
            // after final shutdown callbacks. This is not normal completion.
            if (!native->parked || !native->waiting || native->servicing || native->service ||
                    native->failure || native->self_suspended || native->self_suspend_pending ||
                    native->sdk.state != OS_THREAD_STATE_WAITING || !native->sdk.queue ||
                    native->sdk.mutex || native->sdk.queueMutex.head || native->sdk.queueMutex.tail ||
                    native->sdk.queueJoin.head || native->sdk.queueJoin.tail ||
                    state.reschedule <= 0 || state.scheduler_owner != &caller)
                throw std::logic_error("Terminal source cancellation lacks a quiescent native wait");
            auto* queue = native->sdk.queue;
            ValidateQueue(state, queue);
            Remove(queue, &native->sdk);
            native->waiting = false;
            native->terminal_stopped = true;
            OSClearContext(&native->sdk.context);
            if (native->sdk.attr & OS_THREAD_ATTR_DETACH) {
                RemoveActive(state, *native);
                native->sdk.state = kExited;
            } else {
                native->sdk.state = OS_THREAD_STATE_MORIBUND;
            }
            // Preserve val as original OSCancelThread does. No source joiner
            // exists in this qualified branch; no completion or wake is forged.
            return;
        }
        if (native->started || native->waiting || native->sdk.mutex ||
                native->sdk.queueMutex.head || native->sdk.queueMutex.tail)
            throw std::logic_error("Running native SDK cancellation would destroy unqualified source C++ frames");
        native->cancelled = true;
        Complete(state, *native, reinterpret_cast<void*>(std::uintptr_t{0xffffffffu}));
        native->changed.notify_one();
    }
    JoinHost(native);
}
extern "C" void ChargedNativeBeginThreadPowerRemoval() {
    if (NativeInterruptsEnabled() || !NativeInterruptWaitAllowed())
        throw std::logic_error("Native final power boundary requires the original plain interrupt mask");
    auto& caller = Current();
    auto& state = State();
    std::lock_guard lock(state.latch);
    if (state.power_owner || state.reschedule <= 0 || state.scheduler_owner != &caller ||
            caller.waiting || caller.servicing || caller.initial_resumer ||
            caller.initial_handoff_parked || caller.sdk.state != OS_THREAD_STATE_RUNNING)
        throw std::logic_error("Native final power boundary has no exclusive source scheduler owner");
    state.power_owner = &caller;
}
namespace mscharged::platform {
NativeThreadPowerRemovalStatus ValidateNativeThreadsForPowerRemoval() {
    if (NativeInterruptsEnabled() || !NativeInterruptWaitAllowed())
        throw std::logic_error("Native terminal thread fence requires the source plain interrupt mask");
    auto& caller = Current();
    auto& state = State();
    std::lock_guard lock(state.latch);
    if (!caller.active || caller.waiting || caller.servicing || caller.initial_handoff_parked ||
            caller.initial_resumer || caller.self_suspended ||
            caller.self_suspend_pending || caller.sdk.state != OS_THREAD_STATE_RUNNING ||
            caller.sdk.suspend != 0 || caller.sdk.queue || caller.sdk.mutex ||
            caller.sdk.queueMutex.head || caller.sdk.queueMutex.tail ||
            (state.reschedule > 0 && state.scheduler_owner != &caller))
        throw std::logic_error("Native terminal caller still borrows an unfinished SDK lifetime");

    NativeThreadPowerRemovalStatus status;
    // Complete is published only after the original entry and all ordinary
    // native C++ frame destructors have ended. A std::thread may still have a
    // joinable handle at that point; power removal retains that handle/image.
    // Inactive joined/detached descriptors may already be freed by their source
    // owner, so only native metadata is read in this pass, never their sdk alias.
    for (const auto& entry : state.managed) {
        const auto& native = *entry.second;
        if (&native == &caller) continue;
        if (native.terminal_stopped) {
            if (state.power_owner != &caller || state.reschedule <= 0 ||
                    state.scheduler_owner != &caller || !native.parked || native.completed ||
                    native.failure || native.waiting || native.servicing || native.service ||
                    native.initial_resumer || native.initial_handoff_parked ||
                    native.self_suspended || native.self_suspend_pending)
                throw std::logic_error("Terminally cancelled native source context is not retained and parked");
            ++status.stopped_workers;
            continue;
        }
        if (!native.completed || native.failure || native.waiting || native.servicing ||
                native.initial_resumer || native.initial_handoff_parked ||
                native.self_suspended || native.self_suspend_pending)
            throw std::logic_error("Native power removal has an unfinished or failed source worker");
        ++status.completed_workers;
    }
    for (const auto& entry : state.live) {
        const auto& native = *entry.second;
        if (&native != &caller && !native.managed)
            throw std::logic_error("Native power removal still owns a foreign SDK caller");
    }

    // Original OSCancelThread leaves joinable threads MORIBUND in this actual
    // active list; OSReset::KillThreads does not join them. Verify the retained
    // source records without changing their state, links, results or ownership.
    OSThread* previous{};
    std::size_t count{};
    bool found_caller{};
    for (auto* thread = state.active.head; thread; thread = thread->linkActive.next) {
        if (++count > state.live.size())
            throw std::logic_error("Native terminal active thread list has a cycle");
        auto& native = Find(state, thread);
        if (!native.active || thread->linkActive.prev != previous)
            throw std::logic_error("Native terminal active thread list ownership is invalid");
        if (&native == &caller) {
            found_caller = true;
        } else {
            if (!native.managed || (!native.completed && !native.terminal_stopped) || native.failure ||
                    thread->state != OS_THREAD_STATE_MORIBUND || thread->queue ||
                    thread->mutex || thread->queueMutex.head || thread->queueMutex.tail ||
                    thread->queueJoin.head || thread->queueJoin.tail)
                throw std::logic_error("Native terminal source descriptor is not genuinely quiescent");
            ++status.retained_moribund_threads;
        }
        previous = thread;
    }
    std::size_t active_count{};
    for (const auto& entry : state.live) if (entry.second->active) ++active_count;
    if (!found_caller || previous != state.active.tail || count != active_count)
        throw std::logic_error("Native terminal source caller/list ownership is invalid");
    return status;
}

void DrainNativeThreadLifetimes() {
    if (!NativeInterruptWaitAllowed())
        throw std::logic_error("Cannot retire SDK workers inside an interrupt/host exclusion");
    auto& state = State();
    std::vector<std::shared_ptr<NativeThread>> retired;
    {
        Mask mask;
        std::lock_guard lock(state.latch);
        for (const auto& entry : state.managed) {
            const auto& native = entry.second;
            if (!native->completed || native->active || native->initial_resumer || native->initial_handoff_parked)
                throw std::logic_error("Native SDK source callback or attached thread still owns its lifetime");
            retired.push_back(native);
        }
    }
    for (const auto& native : retired) JoinHost(native);
    Mask mask;
    std::lock_guard lock(state.latch);
    for (const auto& native : retired) {
        state.live.erase(&native->sdk);
        state.managed.erase(&native->sdk);
    }
}
} // namespace mscharged::platform

extern "C" OSThreadQueue* ChargedNativeActiveThreadQueue() {
    // A borrowed source traversal must retain actual SDK exclusion throughout.
    if (NativeInterruptsEnabled())
        throw std::logic_error("Native active thread list requires the source interrupt mask");
    (void)Current();
    auto& state = State();
    std::lock_guard lock(state.latch);
    return &state.active;
}
