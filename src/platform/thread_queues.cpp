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
    NativeThreadWaitService service{};
    // Created SDK descriptors are borrowed from the actual caller. Host stack
    // backing and native execution resources are separate physical ABI state.
    bool managed{}, started{}, completed{}, cancelled{}, active{}, self_suspended{};
    s32 suspend_count{};
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
        if (auto* context = OSGetCurrentContext()) sdk.context = *context;
        NativeInterruptGuard exclusion;
        auto& state = State();
        std::lock_guard lock(state.latch);
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
        if (waiting || sdk.queue) std::terminate();
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

void Wait(NativeThread& native, std::unique_lock<std::mutex>& lock) {
    while (native.waiting || native.suspend_count != 0) {
        if (!native.service) {
            native.changed.wait(lock, [&] { return !native.waiting && native.suspend_count == 0; });
            break;
        }
        if (native.changed.wait_for(lock, std::chrono::milliseconds(1),
                [&] { return !native.waiting && native.suspend_count == 0; })) break;
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
void Complete(Threads& state, NativeThread& native, void* result) {
    if (native.completed) return;
    if (native.waiting || native.sdk.queue || native.sdk.mutex ||
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
    native.changed.notify_all();
}
void Run(const std::shared_ptr<NativeThread>& native) noexcept {
    OSContext* prior_context = OSGetCurrentContext();
    void* result{};
    try {
        auto& state = State();
        {
            std::unique_lock lock(state.latch);
            native->changed.wait(lock, [&] { return native->cancelled || native->suspend_count == 0; });
            if (native->cancelled) return; // Cancel joined this parked host task; no source entry ran.
        }
        const auto stack = mscharged::CurrentThreadStackLimits();
        {
            Mask mask;
            std::lock_guard lock(state.latch);
            if (native->cancelled) return;
            // A concurrent source Suspend may precede the first native entry.
            if (native->suspend_count != 0) {
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
            native->changed.wait(lock, [&] { return native->cancelled || native->suspend_count == 0; });
            if (native->cancelled) return;
            lock.unlock();
            Mask mask;
            lock.lock();
            if (native->cancelled) return;
            if (native->suspend_count != 0) continue;
            native->started = true;
            native->sdk.state = OS_THREAD_STATE_RUNNING;
            native->sdk.stackBase = reinterpret_cast<u8*>(stack.high);
            native->sdk.stackEnd = reinterpret_cast<u8*>(stack.low);
        }
        bound_thread = native.get();
        OSSetCurrentContext(&native->sdk.context);
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
        if (native.waiting || native.servicing || native.sdk.queue ||
                native.sdk.state != OS_THREAD_STATE_RUNNING || native.sdk.suspend != 0 ||
                native.sdk.mutex || native.sdk.queueMutex.head || native.sdk.queueMutex.tail)
            throw std::logic_error("Native SDK thread wait state is unqualified");
        ValidateQueue(state, queue);
        native.sdk.state = OS_THREAD_STATE_WAITING;
        native.waiting = true;
        Insert(queue, &native.sdk);
    }

    // A descheduled Wii thread does not keep hardware globally masked. Enqueue
    // first, then release the native mask; the wait predicate prevents a lost
    // wake in the gap before the actual condition-variable wait.
    OSEnableInterrupts();
    try {
        std::unique_lock lock(state.latch);
        Wait(native, lock);
        lock.unlock();
        OSDisableInterrupts();
        lock.lock();
        native.sdk.state = OS_THREAD_STATE_RUNNING;
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
    auto& native = Find(state, thread);
    if (thread->mutex || thread->queueMutex.head || thread->queueMutex.tail || thread->suspend != native.suspend_count)
        throw std::logic_error("Native SDK mutex/suspension scheduling is unqualified");
    if (thread->base != priority) {
        auto* queue = native.waiting ? thread->queue : nullptr;
        if (queue) { ValidateQueue(state, queue); Remove(queue, thread); }
        thread->base = priority;
        if (native.suspend_count == 0) thread->priority = priority;
        if (queue) Insert(queue, thread);
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
            if (!old->completed || old->active)
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
        return found->second->completed ? TRUE : FALSE;
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
    Mask mask;
    auto& state = State();
    std::lock_guard lock(state.latch);
    auto& native = Find(state, thread);
    if (native.sdk.suspend != native.suspend_count || native.sdk.mutex ||
            native.sdk.queueMutex.head || native.sdk.queueMutex.tail)
        throw std::logic_error("Native SDK resume/mutex state is unqualified");
    const auto prior = native.suspend_count;
    if (native.suspend_count > 0) --native.suspend_count;
    native.sdk.suspend = native.suspend_count;
    if (native.suspend_count == 0) {
        if (native.waiting) {
            auto* queue = native.sdk.queue;
            ValidateQueue(state, queue);
            Remove(queue, thread);
            thread->priority = thread->base;
            Insert(queue, thread);
        } else if (native.sdk.state == OS_THREAD_STATE_READY) {
            thread->priority = thread->base;
        }
        native.changed.notify_one();
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
        if (native->sdk.suspend != native->suspend_count || native->sdk.mutex ||
                native->sdk.queueMutex.head || native->sdk.queueMutex.tail || native->completed ||
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
            } else if (native == &caller && native->sdk.state == OS_THREAD_STATE_RUNNING) {
                native->sdk.state = OS_THREAD_STATE_READY;
                native->self_suspended = true;
                self_wait = true;
            }
        }
    }
    if (self_wait) {
        OSEnableInterrupts();
        {
            std::unique_lock lock(state.latch);
            Wait(*native, lock);
        }
        OSDisableInterrupts();
        std::lock_guard lock(state.latch);
        native->self_suspended = false;
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
    auto& state = State();
    std::shared_ptr<NativeThread> native;
    {
        Mask mask;
        std::lock_guard lock(state.latch);
        native = Managed(state, thread);
        if (native->completed) return;
        if (native->started || native->waiting || native->sdk.mutex ||
                native->sdk.queueMutex.head || native->sdk.queueMutex.tail)
            throw std::logic_error("Running native SDK cancellation would destroy unqualified source C++ frames");
        native->cancelled = true;
        Complete(state, *native, reinterpret_cast<void*>(std::uintptr_t{0xffffffffu}));
        native->changed.notify_one();
    }
    JoinHost(native);
}
namespace mscharged::platform {
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
            if (!native->completed || native->active)
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
