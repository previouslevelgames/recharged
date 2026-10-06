#include "runtime/events.h"
#include "runtime/tasks.h"
#include "Game/EventConnection.h"
#include "Game/EventRegistry.h"
#include "Game/EventDispatcher.inl"
#include "NL/MemAlloc.h"
#include <memory>

namespace mscharged
{
void ShutdownNativeDispatchTask()
{
    CheckNativeEventThread();
    if (!gDispatchEventsTask) return;
    auto* task = gDispatchEventsTask;
    if (task->dispatcher.state.fields.dispatching || task->dispatcher.mNativeClearing)
        throw std::logic_error("Cannot destroy an active dispatch task");
    RemoveNativeTask(task);
    std::exception_ptr failure;
    try { task->dispatcher.FreeBlocks(); } catch (...) { failure = std::current_exception(); }
    gDispatchEventsTask = nullptr;
    nlDeleteGameObject(task);
    if (failure) std::rethrow_exception(failure);
}

std::string VerifyStartupQueuedEvents()
{
    const auto standard = StandardAllocator.TotalFreeMemory();
    const auto external = VirtualAllocator.TotalFreeMemory();
    InitializeNativeEventRegistry();
    try
    {
        fn_80115F10();
        int received = 0, disposed = 0;
        {
            UnidentifiedQueuedEvent<int> event(&gDispatchEventsTask->dispatcher, "NativeQueuedStartup", -1);
            EventConnectionOwner owner;
            Function<int*> listener([&](int* data) { received += *data; });
            Function<int*> disposer([&](int* data) { ++disposed; nlFree(data); });
            event.Add(listener, reinterpret_cast<EventOwnerHandle>(&owner), 1);
            auto queue = [&](int value) {
                std::unique_ptr<int, void(*)(int*)> payload(static_cast<int*>(nlMalloc(sizeof(int))),
                    [](int* data) { nlFree(data); });
                *payload = value;
                event.Queue(payload.get(), disposer);
                payload.release(); // Only a successful Queue transfers the payload.
            };
            queue(7);
            gDispatchEventsTask->Run(0.02f);
            queue(100);
            fn_80115FB4(); // Diagnostic unload/reset: dispose without delivering.
            queue(3);
            gDispatchEventsTask->Run(0.02f);
            queue(100); // Event destruction cancels this payload immediately.
        }
        if (received != 10 || disposed != 4)
            throw std::runtime_error("Diagnostic queued delivery/reset/disposal failed");
        ShutdownNativeDispatchTask();
        ShutdownNativeEventRegistry();
    }
    catch (...)
    {
        if (NativeEventRegistryReady())
        {
            ShutdownNativeDispatchTask();
            ShutdownNativeEventRegistry();
        }
        throw;
    }
    if (StandardAllocator.TotalFreeMemory() != standard || VirtualAllocator.TotalFreeMemory() != external)
        throw std::runtime_error("Queued event teardown did not recover game allocations");
    return "Diagnostic DispatchEventsTask delivery, reset and queued payload cleanup verified; both arenas recovered.";
}
}
