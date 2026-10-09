#include "runtime/events.h"
#include "Game/EventConnection.h"
#include "Game/EventRegistry.h"
#include "NL/nlFunction.inl"
#include "NL/nlMemory.h"
#include "NL/MemAlloc.h"
#include <stdexcept>

namespace mscharged
{
std::string VerifyStartupEvents()
{
    const auto standard = StandardAllocator.TotalFreeMemory();
    const auto external = VirtualAllocator.TotalFreeMemory();
    InitializeNativeEventRegistry();
    bool statePushed = false;
    try
    {
        PushEventConnectionState();
        statePushed = true;
        {
            Event<int> event("NativeStartup", -1);
            EventConnectionOwner owner;
            int received = 0;
            Function<int*> callback([&](int* value) {
                received += *value;
                DisconnectEventOwner(&owner);
            });
            event.Add(callback, reinterpret_cast<EventOwnerHandle>(&owner), 1);
            if (callback || !owner.mConnection)
                throw std::runtime_error("Diagnostic event callback transfer failed");
            int value = 7;
            event.Deliver(&value);
            event.Deliver(&value);
            if (received != value || owner.mConnection)
                throw std::runtime_error("Diagnostic event delivery/disconnection failed");
        }
        PopEventConnectionState();
        statePushed = false;
        ShutdownNativeEventRegistry();
    }
    catch (...)
    {
        // All scoped objects unwind before the registry, while arenas are alive.
        if (statePushed) PopEventConnectionState();
        if (NativeEventRegistryReady()) ShutdownNativeEventRegistry();
        throw;
    }
    if (StandardAllocator.TotalFreeMemory() != standard || VirtualAllocator.TotalFreeMemory() != external)
        throw std::runtime_error("Diagnostic events did not recover their game allocations");
    return "Diagnostic event registry, callback transfer, delivery, self-disconnect and state cleanup verified; both arenas recovered.";
}
}
