#include "runtime/events.h"
#include "runtime/graphics_memory.h"
#include "Game/EventConnection.h"
#include "Game/EventRegistry.h"
#include "Game/EventDispatcher.inl"
#include "NL/nlFunction.inl"
#include "NL/MemAlloc.h"
#include <array>
#include <cstddef>
#include <cstring>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <thread>
#include <vector>

namespace
{
void Require(bool condition, const char* message)
{ if (!condition) throw std::runtime_error(message); }
template<class Exception = std::logic_error, class Action> void Reject(Action action)
{
    try { action(); } catch (const Exception&) { return; }
    throw std::runtime_error("Invalid queued event operation was accepted");
}
struct Payload { int value; };
using OwnedPayload = std::unique_ptr<Payload, void(*)(Payload*)>;
OwnedPayload MakePayload(int value)
{
    auto* data = new (nlMalloc(sizeof(Payload))) Payload{value};
    return {data, [](Payload* data) { data->~Payload(); nlFree(data); }};
}
void Queue(QueuedEvent<Payload>& event, int value, const Function<Payload*>& disposer)
{
    auto data = MakePayload(value);
    event.Queue(data.get(), disposer);
    data.release();
}

void DispatcherModes()
{
    EventDispatcher dispatcher;
    Reject([] { ShutdownNativeEventRegistry(); });
    std::vector<int> order;
    Function<bool> child([&](bool deliver) { order.push_back(deliver ? 3 : -3); });
    Function<bool> first([&](bool deliver) {
        order.push_back(deliver ? 1 : -1);
        if (deliver) dispatcher.Add(child);
    });
    Function<bool> second([&](bool deliver) { order.push_back(deliver ? 2 : -2); });
    dispatcher.Add(first); dispatcher.Add(second);
    Require(first && second, "Dispatcher Add consumed the caller's callback");
    dispatcher.Dispatch(true);
    Require(order == std::vector<int>({1,2}) && dispatcher.state.fields.callbackCount == 1,
            "One-batch delivery ran a newly queued callback");
    dispatcher.Dispatch(true);
    Require(order == std::vector<int>({1,2,3}) && !dispatcher.state.fields.callbackCount,
            "Next-batch delivery lost the appended callback");
    order.clear();
    dispatcher.Add(first); dispatcher.Add(second);
    dispatcher.Dispatch(false);
    Require(order == std::vector<int>({1,2,3}) && dispatcher.callbacks.IsEmpty(), "Full drain failed");
    dispatcher.Add(child); dispatcher.Clear();
    Require(order.back() == -3 && dispatcher.callbacks.IsEmpty(), "Clear did not invoke cancellation");
    Function<bool> empty;
    Reject<std::invalid_argument>([&] { dispatcher.Add(empty); });

    Function<bool> checked([&](bool deliver) {
        Reject([&] { dispatcher.Dispatch(true); });
        Reject([&] { dispatcher.Clear(); });
        Reject([&] { dispatcher.FreeBlocks(); });
        if (!deliver) Reject([&] { dispatcher.Add(child); });
    });
    dispatcher.Add(checked); dispatcher.Dispatch(true);
    dispatcher.Add(checked); dispatcher.Clear();
    bool wrongThread = false;
    std::thread worker([&] {
        try { dispatcher.Add(child); } catch (const std::logic_error&) { wrongThread = true; }
    });
    worker.join();
    Require(wrongThread, "Dispatcher accepted another thread");

    order.clear();
    Function<bool> stop([&](bool deliver) {
        order.push_back(deliver ? 1 : -1);
        if (deliver) { dispatcher.Add(child); dispatcher.state.fields.stopDispatch = true; }
    });
    dispatcher.Add(stop); dispatcher.Add(second); dispatcher.Dispatch(false);
    Require(order == std::vector<int>({1,-2,-3}) && dispatcher.callbacks.IsEmpty()
            && !dispatcher.state.fields.callbackCount && !dispatcher.state.fields.stopDispatch,
            "Stop failed to cancel old/new batches");
}

void QueuedPayloads()
{
    EventDispatcher dispatcher;
    QueuedEvent<Payload> event(&dispatcher, "Payloads", -1);
    std::vector<int> values;
    int disposed = 0;
    Function<Payload*> disposer([&](Payload* data) { ++disposed; nlFree(data); });
    Function<Payload*> listener([&](Payload* data) {
        values.push_back(data->value);
        if (data->value == 1) Queue(event, 3, disposer);
    });
    event.Add(listener, 0, -1);
    Queue(event, 1, disposer); Queue(event, 2, disposer);
    dispatcher.Dispatch(true);
    Require(values == std::vector<int>({1,2}) && disposed == 2 && disposer,
            "Payload delivery, disposal or disposer copy failed");
    dispatcher.Dispatch(false);
    Require(values == std::vector<int>({1,2,3}) && disposed == 3, "Nested queued payload delivery failed");
    Queue(event, 4, disposer); Queue(event, 5, disposer);
    event.CancelPending(); event.CancelPending();
    Require(disposed == 5 && dispatcher.state.fields.callbackCount == 2,
            "Event cancellation did not dispose pending payloads exactly once");
    dispatcher.Dispatch(true);
    Require(disposed == 5 && values.size() == 3, "Cancelled payload was delivered/disposed again");
    Queue(event, 6, disposer); dispatcher.FreeBlocks();
    Require(disposed == 6 && !dispatcher.callbacks.m_Allocator.m_BlockList,
            "FreeBlocks did not cancel before releasing the pool");
    Queue(event, 7, disposer); dispatcher.Dispatch(true);
    Require(disposed == 7 && values.back() == 7, "Dispatcher did not recover after FreeBlocks");

    QueuedEvent<NoEventData> signal(&dispatcher, "Signal", -1);
    int signals = 0, cleanups = 0;
    Function<FnVoidVoid> signalListener([&] { ++signals; });
    Function<FnVoidVoid> signalCleanup([&] { ++cleanups; });
    signal.Add(signalListener, 0, -1);
    signal.Queue(); signal.Queue(signalCleanup); dispatcher.Dispatch(true);
    Require(signals == 2 && cleanups == 1 && signalCleanup, "No-data queue failed");
    signal.Queue(signalCleanup); dispatcher.Clear();
    Require(signals == 2 && cleanups == 2, "No-data cancellation failed");
}

void DestructionOrder()
{
    int disposed = 0, delivered = 0;
    Function<Payload*> disposer([&](Payload* data) { ++disposed; nlFree(data); });
    EventDispatcher dispatcher;
    {
        QueuedEvent<Payload> event(&dispatcher, "DestroyEvent", -1);
        Function<Payload*> listener([&](Payload*) { ++delivered; });
        event.Add(listener, 0, -1);
        Queue(event, 1, disposer); Queue(event, 2, disposer);
    }
    Require(disposed == 2, "Event destruction did not immediately dispose payloads");
    dispatcher.Dispatch(true);
    Require(disposed == 2 && delivered == 0, "Destroyed event retained a callback target");
    {
        auto host = std::make_unique<EventDispatcher>();
        auto event = std::make_unique<QueuedEvent<Payload>>(host.get(), "DestroyDispatcher", -1);
        Queue(*event, 3, disposer);
        host.reset();
        Require(disposed == 3, "Dispatcher destruction did not dispose payload");
        auto rejected = MakePayload(4);
        Reject([&] { event->Queue(rejected.get(), disposer); });
        Require(disposed == 3 && rejected->value == 4, "Dead dispatcher took caller payload ownership");
    }
}

void CallbackFailures()
{
    EventDispatcher dispatcher;
    QueuedEvent<Payload> event(&dispatcher, "Failures", -1);
    int disposed = 0;
    Function<Payload*> disposer([&](Payload* data) { ++disposed; nlFree(data); });
    Function<Payload*> failing([&](Payload*) { throw std::runtime_error("delivery failure"); });
    event.Add(failing, 0, -1);
    Queue(event, 1, disposer); Queue(event, 2, disposer);
    try { dispatcher.Dispatch(false); throw std::logic_error("Missing delivery failure"); }
    catch (const std::runtime_error& error) { Require(std::strcmp(error.what(), "delivery failure") == 0, "Wrong primary failure"); }
    Require(disposed == 2 && !dispatcher.state.fields.dispatching && dispatcher.callbacks.IsEmpty(),
            "Delivery exception did not dispose current/remaining payloads");
    Function<Payload*> bothFail([&](Payload* data) { ++disposed; nlFree(data); throw std::runtime_error("disposer failure"); });
    Queue(event, 3, bothFail); Queue(event, 4, bothFail);
    try { dispatcher.Dispatch(true); throw std::logic_error("Missing failure"); }
    catch (const std::runtime_error& error) { Require(std::strcmp(error.what(), "delivery failure") == 0, "Cleanup replaced the delivery error"); }
    Require(disposed == 4 && dispatcher.callbacks.IsEmpty(), "Double failure leaked another payload");
    event.RemoveAll();
    Queue(event, 5, bothFail); Queue(event, 6, disposer);
    Reject<std::runtime_error>([&] { dispatcher.FreeBlocks(); });
    Require(disposed == 6 && !dispatcher.callbacks.m_Allocator.m_BlockList
            && !dispatcher.state.fields.callbackCount, "Disposer failure prevented pool cleanup");
    Queue(event, 7, disposer); dispatcher.Dispatch(true);
    Require(disposed == 7, "Dispatcher did not recover after a disposer failure");

    Function<Payload*> cancellation([&](Payload* data) {
        Reject([&] { event.CancelPending(); });
        auto attempt = MakePayload(9);
        Reject([&] { event.Queue(attempt.get(), disposer); });
        ++disposed; nlFree(data);
    });
    Queue(event, 8, cancellation); Queue(event, 8, cancellation);
    event.CancelPending(); dispatcher.Clear();
    Require(disposed == 9, "Cancellation reentrancy changed payload ownership");

    Function<Payload*> clearHost([&](Payload* data) {
        dispatcher.Clear();
        ++disposed; nlFree(data);
    });
    Queue(event, 10, clearHost); Queue(event, 11, disposer);
    event.CancelPending();
    Require(disposed == 11 && dispatcher.callbacks.IsEmpty(),
            "Disposer clearing its dispatcher lost the active cancellation job");
}

void LimitsAndFailures()
{
    EventDispatcher dispatcher;
    QueuedEvent<Payload> event(&dispatcher, "Bounds", -1);
    int cancelled = 0, disposed = 0;
    Function<bool> callback([&](bool delivery) { if (!delivery) ++cancelled; });
    for (unsigned i = 0; i < mscharged::event_queue_limit; ++i) dispatcher.Add(callback);
    Function<Payload*> disposer([&](Payload* data) { ++disposed; nlFree(data); });
    auto rejected = MakePayload(1);
    Reject<std::length_error>([&] { event.Queue(rejected.get(), disposer); });
    Require(disposed == 0 && rejected->value == 1 && dispatcher.state.fields.callbackCount == 4096,
            "Full queue took the caller's payload");
    dispatcher.Clear();
    Require(cancelled == 4096 && dispatcher.callbacks.IsEmpty(), "Full queue cancellation lost callbacks");
    event.Queue(rejected.get(), disposer); rejected.release(); dispatcher.Dispatch(true);
    Require(disposed == 1, "Queue failed after capacity recovery");
    dispatcher.FreeBlocks();

    // Force a callback clone to throw after the dispatcher has reserved a slot.
    struct ThrowingCopy
    {
        bool* fail;
        explicit ThrowingCopy(bool* flag) : fail(flag) {}
        ThrowingCopy(const ThrowingCopy& other) : fail(other.fail)
        { if (*fail) throw std::runtime_error("clone failure"); }
        void operator()(bool) const {}
    };
    bool fail = false;
    Function<bool> clone(ThrowingCopy{&fail});
    fail = true;
    Reject<std::runtime_error>([&] { dispatcher.Add(clone); });
    Require(clone && dispatcher.callbacks.IsEmpty() && !dispatcher.state.fields.callbackCount,
            "Failed callback cloning published an invalid node");
    fail = false;
    dispatcher.Add(clone); dispatcher.Dispatch(true); dispatcher.FreeBlocks();

    std::vector<void*> pressure;
    try { while (true) pressure.push_back(StandardAllocator.Allocate(128,8,false)); }
    catch (const std::bad_alloc&) {}
    try { while (true) pressure.push_back(StandardAllocator.Allocate(1,8,false)); }
    catch (const std::bad_alloc&) {}
    Reject<std::bad_alloc>([&] { dispatcher.Add(callback); });
    Require(callback && dispatcher.callbacks.IsEmpty() && !dispatcher.state.fields.callbackCount,
            "Real clone allocation failure published a callback");
    for (void* allocation : pressure) StandardAllocator.Free(allocation);
    dispatcher.FreeBlocks();
    auto payload = MakePayload(2);
    pressure.clear();
    try { while (true) pressure.push_back(StandardAllocator.Allocate(128,8,false)); }
    catch (const std::bad_alloc&) {}
    try { while (true) pressure.push_back(StandardAllocator.Allocate(1,8,false)); }
    catch (const std::bad_alloc&) {}
    Reject<std::bad_alloc>([&] { event.Queue(payload.get(), disposer); });
    Require(disposed == 1 && payload->value == 2, "Queue OOM took caller ownership");
    for (void* allocation : pressure) StandardAllocator.Free(allocation);
    event.Queue(payload.get(), disposer); payload.release(); dispatcher.Dispatch(true);
    Require(disposed == 2, "Queue did not recover after OOM");

    unsigned calls = 0;
    Function<bool> repeated;
    repeated = Function<bool>([&](bool delivery) { if (delivery) { ++calls; dispatcher.Add(repeated); } });
    dispatcher.Add(repeated);
    Reject<std::length_error>([&] { dispatcher.Dispatch(false); });
    Require(calls == mscharged::event_dispatch_budget && dispatcher.callbacks.IsEmpty()
            && !dispatcher.state.fields.dispatching && !dispatcher.state.fields.callbackCount,
            "Self-generating queue budget did not cancel and recover");
}

void OriginalTask()
{
    Reject([] { fn_80115FB4(); });
    // Leave enough memory for the task itself, but none for its dispatcher's
    // lifetime token. Failed construction must reclaim the game-new allocation.
    alignas(64) std::array<std::byte, 4096> smallArena{};
    const auto alignment = alignof(FreeBlockList);
    const auto capacity = ((sizeof(DispatchEventsTask) + alignment - 1) / alignment + 1) * alignment;
    MemoryAllocator small;
    small.Initialize(smallArena.data(), capacity);
    {
        mscharged::ScopedGameAllocator selected(small);
        Reject<std::bad_alloc>([] { InitializeDispatchEventsTask(); });
        Require(!gDispatchEventsTask && small.TotalFreeMemory() == capacity,
                "Failed dispatch task construction leaked or published its object");
    }
    InitializeDispatchEventsTask();
    auto* task = gDispatchEventsTask;
    Require(std::strcmp(task->GetName(), "Dispatch Events") == 0, "Wrong original task");
    task->StateTransition(1,2); // Exact original default hook extracted from Team.cpp.
    Reject([] { InitializeDispatchEventsTask(); });
    {
        QueuedEvent<Payload> event(&task->dispatcher, "TaskQueue", -1);
        int received = 0, disposed = 0;
        Function<Payload*> listener([&](Payload* data) {
            Reject([] { mscharged::ShutdownNativeDispatchTask(); });
            Reject([] { fn_80115FB4(); });
            received += data->value;
        });
        Function<Payload*> disposer([&](Payload* data) { ++disposed; nlFree(data); });
        event.Add(listener,0,-1);
        Queue(event,1,disposer); task->Run(0.02f);
        Queue(event,100,disposer); fn_80115FB4();
        Require(gDispatchEventsTask == task && received == 1 && disposed == 2,
                "Original task reset changed lifetime or delivered cancellation");
        Queue(event,2,disposer); task->Run(0.02f);
        Require(received == 3 && disposed == 3, "Original task failed after reset");
        mscharged::ShutdownNativeDispatchTask();
        auto rejected = MakePayload(3);
        Reject([&] { event.Queue(rejected.get(),disposer); });
        Require(!gDispatchEventsTask && disposed == 3, "Final task destruction left a live dispatcher");
    }
    mscharged::ShutdownNativeDispatchTask();
}
}
int main()
{
    try
    {
        Reject([] { EventDispatcher beforeMemory; });
        alignas(64) static std::array<std::byte, 2*1024*1024> standard{}, external{};
        StandardAllocator.Initialize(standard.data(),standard.size());
        VirtualAllocator.Initialize(external.data(),external.size());
        CurrentAllocator = &StandardAllocator; gMemoryInitialized = 1;
        for (int round=0;round<3;++round)
        {
            std::cout << mscharged::VerifyStartupQueuedEvents() << '\n';
            InitializeNativeEventRegistry();
            DispatcherModes(); QueuedPayloads(); DestructionOrder(); CallbackFailures(); LimitsAndFailures(); OriginalTask();
            ShutdownNativeEventRegistry();
            Require(StandardAllocator.TotalFreeMemory()==standard.size() && VirtualAllocator.TotalFreeMemory()==external.size(),
                    "Queued teardown did not recover both game arenas");
        }
        gMemoryInitialized=0; CurrentAllocator=nullptr; StandardAllocator={};VirtualAllocator={};
        std::cout << "Queued events: batching, exact disposal, both destruction orders, failure/cancel/reentrancy,\n"
                     "4096 callback capacity, dispatch budget, diagnostic task/reset and three complete teardowns passed\n";
        return 0;
    }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
