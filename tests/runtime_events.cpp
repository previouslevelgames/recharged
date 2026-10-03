#include "runtime/events.h"
#include "Game/EventRegistry.h"
#include "Game/EventConnection.h"
#include "Game/UnidentifiedStaticEvent.h"
#include "Game/UnidentifiedStaticEvent3.h"
#include "NL/nlMemory.h"
#include "NL/MemAlloc.h"
#include "NL/nlFunction.inl"
#include <array>
#include <cstddef>
#include <cstdint>
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
    try { action(); }
    catch (const Exception&) { return; }
    throw std::runtime_error("Invalid event operation was accepted");
}
EventOwnerHandle Handle(EventConnectionOwner& owner)
{ return reinterpret_cast<EventOwnerHandle>(&owner); }
void Increment(int* value) { ++*value; }

void RegistryAndOwners()
{
    UnidentifiedEvent<int> event("Immediate", -1), other("Other", -1);
    Require(UnidentifiedFindEvent<int>("IMMEDIATE", -1) == &event, "Original case-insensitive lookup failed");
    Require(UnidentifiedFindEvent<float>("Immediate", -1) == nullptr, "Wrong event type accepted");
    Require(UnidentifiedFindEvent<int>("Missing", -1) == nullptr, "Missing event found");
    Reject([] { UnidentifiedEvent<int> duplicate("Immediate", -1); });
    Reject<std::invalid_argument>([] { UnidentifiedEvent<int> invalid(nullptr, -1); });
    Reject([] { ShutdownNativeEventRegistry(); });
    bool wrongThreadRejected = false;
    std::thread worker([&] {
        try { UnidentifiedFindEvent<int>("Immediate", -1); }
        catch (const std::logic_error&) { wrongThreadRejected = true; }
    });
    worker.join();
    Require(wrongThreadRejected, "Registry accepted access from another thread");

    int calls = 0;
    auto lifetime = std::make_shared<int>(7);
    std::weak_ptr<int> weak = lifetime;
    {
        EventConnectionOwner owner;
        if constexpr (sizeof(void*) > 4)
            Require(Handle(owner) > UINT32_MAX, "Owner test did not exercise a full-width address");
        // Original Add consumes a callback held by const reference, including
        // actually const objects. Its native callable storage is mutable.
        const Function<int*> callback([&, lifetime](int*) { calls += *lifetime; });
        event.Add(callback, Handle(owner), 3);
        Require(!callback && owner.mConnection && owner.mConnection->mOwner == &owner,
                "Callback transfer or owner address failed");
        lifetime.reset();
        Function<int*> duplicate(Increment), empty;
        Reject<std::invalid_argument>([&] { event.Add(duplicate, Handle(owner), 3); });
        Reject<std::invalid_argument>([&] { event.Add(empty, 0, 3); });
        Reject<std::invalid_argument>([&] { event.Add(duplicate, Handle(owner) + 1, 3); });
        Require(duplicate && owner.mConnection, "Rejected registration consumed callback/owner");
        other.Disconnect(&owner);
        Require(owner.mConnection, "Another event disconnected this owner");
        int value = 0;
        owner.mConnection->mFlags &= ~0x80000000u;
        event.Deliver(&value);
        Require(calls == 0, "Disabled listener was delivered");
        owner.mConnection->mFlags |= 0x80000000u;
        event.Deliver(&value);
        Require(calls == 7 && !weak.expired(), "Transferred capture did not survive delivery");
    }
    Require(weak.expired(), "Owner destruction retained callback capture");

    // An owner with auto-disconnect disabled leaves an anonymous listener;
    // destroying the event must never access that former owner's stack slot.
    {
        EventConnectionOwner owner;
        Function<int*> callback(Increment);
        event.Add(callback, Handle(owner), 4);
        owner.mConnection->mFlags &= ~0x40000000u;
    }
    int value = 0;
    event.Deliver(&value);
    Require(value == 1, "Non-owning listener was lost with its owner");
    event.RemoveAll();
    EventConnectionOwner survivor;
    {
        UnidentifiedEvent<int> temporary("Temporary", -1);
        Function<int*> callback(Increment);
        temporary.Add(callback, Handle(survivor), -1);
    }
    Require(!survivor.mConnection, "Event destruction left a dangling owner");
    DisconnectEventOwner(&survivor);
    DisconnectEventOwner(nullptr);
}

void DeliveryMutation()
{
    UnidentifiedEvent<int> event("Mutation", -1), nested("Nested", -1);
    EventConnectionOwner first, second, third;
    int calls = 0, value = 0;
    Function<int*> a([&](int*) { ++calls; event.Disconnect(&second); event.Disconnect(&first); });
    Function<int*> b([&](int*) { calls += 100; });
    Function<int*> c([&](int*) { calls += 10; });
    event.Add(a, Handle(first), 3);
    event.Add(b, Handle(second), 3);
    event.Add(c, Handle(third), 3);
    event.Deliver(&value);
    Require(calls == 11 && !first.mConnection && !second.mConnection && third.mConnection,
            "Delivery used a disconnected listener or skipped a survivor");
    event.RemoveAll();

    // Rebind the same owner while its former callback is still on the stack.
    Function<int*> replacement([&](int*) { calls += 5; });
    Function<int*> rebind([&](int*) {
        ++calls;
        event.Disconnect(&first);
        event.Add(replacement, Handle(first), 8);
    });
    calls = 0;
    event.Add(rebind, Handle(first), 7);
    event.Deliver(&value);
    Require(calls == 6 && first.mConnection, "Appended listener order or rebound owner failed");
    event.Deliver(&value);
    Require(calls == 11 && first.mConnection, "Old listener destroyed the rebound owner");
    event.RemoveAll();

    Function<int*> nestedCallback([&](int*) { ++calls; });
    nested.Add(nestedCallback, 0, -1);
    Function<int*> recursive([&](int* data) {
        Reject([&] { event.Deliver(data); });
        Reject([] { PushEventConnectionState(); });
        nested.Deliver(data);
        ++calls;
    });
    event.Add(recursive, 0, 9);
    calls = 0;
    event.Deliver(&value);
    event.Deliver(&value);
    Require(calls == 4, "Cross-event delivery or recursion recovery failed");
    event.RemoveAll();

    struct CallbackFailure {};
    Function<int*> throwing([&](int*) { event.RemoveAll(); throw CallbackFailure{}; });
    Function<int*> skipped([&](int*) { ++calls; });
    event.Add(throwing, Handle(first), 9);
    event.Add(skipped, Handle(second), 9);
    Reject<CallbackFailure>([&] { event.Deliver(&value); });
    Require(!first.mConnection && !second.mConnection, "Exception retained disconnected owners");
    Function<int*> recovered([&](int*) { ++calls; event.RemoveAll(); });
    event.Add(recovered, 0, 10);
    event.Deliver(&value);
    event.Deliver(&value);
    Require(calls == 5, "Delivery did not recover after callback failure and RemoveAll");
}

void StaticEvents()
{
    UnidentifiedStaticEvent<int, 2> event("Fixed", -1);
    EventConnectionOwner first, second, third;
    Function<int*> a(Increment), b(Increment), c(Increment);
    event.Add(a, Handle(first), 11);
    event.Add(b, Handle(second), 11);
    Reject<std::bad_alloc>([&] { event.Add(c, Handle(third), 11); });
    Require(c && !third.mConnection, "Full fixed pool consumed the new callback");
    int value = 0;
    event.Deliver(&value);
    Require(value == 2, "Fixed event delivery failed");
    event.Disconnect(&first);
    event.Add(c, Handle(third), 11);
    event.RemoveAll();
    Require(!second.mConnection && !third.mConnection, "Fixed pool reuse left dangling owners");
    Function<int*> clear([&](int*) { ++value; event.RemoveAll(); });
    Function<int*> skip(Increment);
    event.Add(clear, Handle(first), 11);
    event.Add(skip, Handle(second), 11);
    event.Deliver(&value);
    Require(value == 3 && !first.mConnection && !second.mConnection,
            "Fixed event RemoveAll during delivery failed");

    UnidentifiedEvent<UnidentifiedEventNoData> dynamicNoData("NoData", -1);
    UnidentifiedStaticEvent<UnidentifiedEventNoData, 2> fixedNoData("FixedNoData", -1);
    Function<FnVoidVoid> d([&] { ++value; }), e([&] { ++value; });
    dynamicNoData.Add(d, Handle(first), -1);
    fixedNoData.Add(e, Handle(second), -1);
    dynamicNoData.Deliver(); fixedNoData.Deliver();
    Require(value == 5 && !d && !e, "No-argument event delivery/transfer failed");

    UnidentifiedStaticEvent<void(int), 2> byValue("ByValue", -1);
    Function<void(int)> f([&](int data) { value += data; });
    byValue.Add(f, 0, -1);
    byValue.Deliver(3);
    Require(value == 8, "Value-argument event delivery failed");

    PadDeviceChangedEvent device;
    Function<void(int, int, int)> changed([&](int a, int b, int c) {
        value = a * 100 + b * 10 + c;
        device.Disconnect(&third);
    });
    device.Add(changed, Handle(third), 12);
    device.Deliver(1, 2, 3);
    device.Deliver(9, 9, 9);
    Require(value == 123 && changed && !third.mConnection,
            "Three-argument event copy/delivery/self-disconnect failed");
}

void ConnectionStates()
{
    UnidentifiedStaticEvent<int, 4> event("Scopes", -1);
    EventConnectionOwner outer, inner;
    Function<int*> a(Increment), b(Increment);
    event.Add(a, Handle(outer), 13);
    PushEventConnectionState();
    event.Add(b, Handle(inner), 13);
    Reject([] { PopEventConnectionState(); });
    event.Disconnect(&outer); // Old nodes can be removed inside a newer state.
    event.Disconnect(&inner);
    PopEventConnectionState();
    Require(!outer.mConnection && !inner.mConnection, "Restoring a state revived old owners");
    for (int i = 0; i < 5; ++i) PushEventConnectionState();
    Reject<std::length_error>([] { PushEventConnectionState(); });
    for (int i = 0; i < 5; ++i) PopEventConnectionState();
    Reject([] { PopEventConnectionState(); });
    const auto baseline = StandardAllocator.TotalFreeMemory();
    for (int i = 0; i < 100; ++i)
    {
        PushEventConnectionState();
        Function<int*> callback(Increment);
        event.Add(callback, Handle(inner), 100 + i);
        event.Disconnect(&inner);
        PopEventConnectionState();
        Require(StandardAllocator.TotalFreeMemory() == baseline,
                "Empty connection groups leaked across state restoration");
    }
}

void AllocationFailures()
{
    UnidentifiedStaticEvent<int, 2> fixed("OOMFixed", -1);
    UnidentifiedEvent<int> dynamic("OOMDynamic", -1);
    EventConnectionOwner owner;
    Function<int*> callback(Increment);
    // Exhaust the native arena without injecting a substitute allocator.
    std::vector<void*> pressure;
    try
    {
        while (true) pressure.push_back(StandardAllocator.Allocate(128, 8, false));
    }
    catch (const std::bad_alloc&) {}
    try
    {
        while (true) pressure.push_back(StandardAllocator.Allocate(1, 8, false));
    }
    catch (const std::bad_alloc&) {}
    const auto exhausted = StandardAllocator.TotalFreeMemory();
    Reject<std::bad_alloc>([&] { fixed.Add(callback, Handle(owner), 20); });
    Require(callback && !owner.mConnection && StandardAllocator.TotalFreeMemory() == exhausted,
            "Failed group allocation changed callback ownership or allocator state");
    for (auto* allocation : pressure) StandardAllocator.Free(allocation);
    fixed.Add(callback, Handle(owner), 20);
    fixed.RemoveAll();

    Function<int*> second(Increment);
    const auto free = VirtualAllocator.LargestFreeBlock();
    auto* block = VirtualAllocator.Allocate(free - alignof(FreeBlockList), 8, false);
    Reject<std::bad_alloc>([&] { dynamic.Add(second, Handle(owner), 21); });
    Require(second && !owner.mConnection, "Failed listener pool allocation consumed callback");
    VirtualAllocator.Free(block);
    dynamic.Add(second, Handle(owner), 21);
    dynamic.RemoveAll();
}

void ManyConnections()
{
    UnidentifiedEvent<int> event("Many", -1);
    std::array<EventConnectionOwner, 4096> owners;
    for (unsigned i = 0; i < owners.size(); ++i)
    {
        Function<int*> callback(Increment);
        event.Add(callback, Handle(owners[i]), i % 47);
    }
    Function<int*> overflow(Increment);
    Reject<std::length_error>([&] { event.Add(overflow, 0, -1); });
    Require(overflow, "Connection limit consumed callback");
    int calls = 0;
    event.Deliver(&calls);
    Require(calls == 4096, "Growing original listener pool lost callbacks");
    for (unsigned i = 0; i < owners.size(); ++i)
        event.Disconnect(&owners[(i * 113) % owners.size()]);
    for (const auto& owner : owners) Require(!owner.mConnection, "Grouped removal left an owner");
    event.Deliver(&calls);
    Require(calls == 4096, "Disconnected callback survived group removal");
}
}

int main()
{
    try
    {
        Reject([] { InitializeNativeEventRegistry(); });
        Reject([] { UnidentifiedEvent<int> event("BeforeInit", -1); });
        alignas(64) static std::array<std::byte, 2 * 1024 * 1024> mem1{}, mem2{};
        StandardAllocator.Initialize(mem1.data(), mem1.size());
        VirtualAllocator.Initialize(mem2.data(), mem2.size());
        CurrentAllocator = &StandardAllocator;
        gMemoryInitialized = 1;
        for (int round = 0; round < 3; ++round)
        {
            std::cout << mscharged::VerifyStartupEvents() << '\n';
            InitializeNativeEventRegistry();
            Reject([] { InitializeNativeEventRegistry(); });
            RegistryAndOwners();
            DeliveryMutation();
            StaticEvents();
            ConnectionStates();
            AllocationFailures();
            ManyConnections();
            ShutdownNativeEventRegistry();
            Require(!NativeEventRegistryReady(), "Registry survived shutdown");
            Require(StandardAllocator.TotalFreeMemory() == mem1.size() && VirtualAllocator.TotalFreeMemory() == mem2.size(),
                    "Event shutdown leaked game memory");
        }
        gMemoryInitialized = 0;
        StandardAllocator = {}; VirtualAllocator = {}; CurrentAllocator = nullptr;
        std::cout << "Native events: owners above 4 GiB, immediate/static/no-data/three-argument delivery, mutation,\n"
                     "exceptions, allocation failures, nested states and 4096 grouped listeners passed; three clean teardowns\n";
        return 0;
    }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
