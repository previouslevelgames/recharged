#include "Game/EventConnection.h"
#include "Game/EventDispatcher.inl"
#include "Game/StaticEvent.h"
#include "NL/plat/PlatPadManager.h"
#include "NL/nlSmallBlockAllocator.h"

#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <type_traits>
#include <utility>

static void check(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "Original event ABI check failed: %s\n", message);
        std::exit(1);
    }
}

using DataBase = QueuedEventBase<int>;
using DataDispatch = void (DataBase::*)(int*, Function<int*>, unsigned char);
using DataBinding = decltype(Bind<void>(MemFun(std::declval<DataDispatch>()),
    std::declval<DataBase*>(), std::declval<int*>(),
    std::declval<Function<int*>>(), std::declval<Placeholder<0>>()));
using DataFunctor = Function<bool>::FunctorImpl<DataBinding>;

using VoidBase = QueuedEventBase<NoEventData>;
using VoidDispatch = void (VoidBase::*)(VoidBase::Callback, unsigned char);
using VoidBinding = decltype(Bind<void>(MemFun(std::declval<VoidDispatch>()),
    std::declval<VoidBase*>(), std::declval<VoidBase::Callback>(),
    std::declval<Placeholder<0>>()));
using VoidFunctor = Function<bool>::FunctorImpl<VoidBinding>;

int main() {
    static_assert(std::is_same<EventOwnerHandle, std::uintptr_t>::value,
        "Original native owner must retain the full host pointer");
    static_assert(sizeof(EventDispatcherState) == sizeof(u32),
        "Original dispatcher union and 16-bit count retained");
    static_assert(sizeof(DLListEntry<EventListener<int>>) >= sizeof(EventListener<int>),
        "Actual native intrusive node includes source listener");
    static_assert(MSCHARGED_EVENT_ENTRY_OFFSET(DLListEntry<EventListener<int>>) == 2 * sizeof(void*),
        "Actual node prefix is two native pointers");

    // Only source metadata construction is exercised here. No EventBase/event,
    // registry, dispatcher/task object, source pool or game allocation is created.
    // Skipping this raw fixture object's destructor keeps owner lifecycle outside
    // the qualifier; its two owner pointers are unused and remain source-zero.
    alignas(EventConnection) unsigned char storage[sizeof(EventConnection)] = {};
    auto* connection = new (storage) EventConnection;
    check(connection->mOwner == nullptr && connection->mEvent == nullptr,
        "Original connection constructor owns no event or owner");
    connection->mFlags = 0;
    connection->mPendingRemoval = 1;
    check(connection->mFlags == 0x20000000u, "Original pending-removal flag is bit 29");
    connection->mFlags = 0;
    connection->mGroupCount = 0xabcd;
    check(connection->mFlags == 0x0000abcdu, "Original group count occupies low 16 bits");
    connection->mFlagsHigh = 0x1357;
    check(connection->mFlags == 0x1357abcdu, "Original high half retains reserved flags");

    EventDispatcherState state;
    state.fields.dispatching = 1;
    check(state.value == 0x80000000u, "Original dispatcher high flag");
    state.fields.stopDispatch = 1;
    state.fields.callbackCount = 0xffff;
    check(state.value == 0xffffc000u, "Original packed counter and stop bit");
    state.fields.callbackCount++;
    check(state.value == 0xc0000000u, "Original 16-bit counter wrap remains a source quirk");

    std::printf("owner=%zu connection=%zu node-prefix=%zu dispatcher-state=%zu\n",
        sizeof(EventOwnerHandle), sizeof(EventConnection),
        std::size_t(MSCHARGED_EVENT_ENTRY_OFFSET(DLListEntry<EventListener<int>>)),
        sizeof(EventDispatcherState));
    std::printf("data-binding=%zu data-functor=%zu void-binding=%zu void-functor=%zu member-pointer=%zu function-descriptor=%zu\n",
        sizeof(DataBinding), sizeof(DataFunctor), sizeof(VoidBinding), sizeof(VoidFunctor),
        sizeof(DataDispatch), sizeof(Function<bool>));
    std::printf("Source ABI only: no original registry/function-pool/queue/task readiness.\n");
}
