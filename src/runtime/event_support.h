#pragma once

#include <cstdint>
#include <new>
#include <stdexcept>
#include <type_traits>

// Shared declarations for the prepared original event templates and registry.
using EventOwnerHandle = std::uintptr_t;
void InitializeNativeEventRegistry();
void ShutdownNativeEventRegistry();
bool NativeEventRegistryReady();
void CheckNativeEventThread();
void ValidateNativeEventConnection(void* event, EventOwnerHandle owner);
void RegisterEventConnection(void*, void*, EventOwnerHandle, int);
void UnregisterEventConnection(void*, void*);

namespace mscharged
{
inline constexpr std::uint32_t event_enabled = 0x80000000u;
inline constexpr std::uint32_t event_pending_removal = 0x20000000u;

template<class List, class Listener>
auto EventEntry(List& list, Listener* listener)
{
    auto* entry = static_cast<decltype(list.m_Head)>(listener->mNativeEntry);
    if (!entry || &entry->entry != listener)
        throw std::logic_error("Invalid native event listener entry");
    return entry;
}

template<class List, class Listener>
void DeleteEventListener(List& list, Listener* listener)
{
    auto* entry = EventEntry(list, listener);
    nlDLRingRemove(&list.m_Head, entry);
    list.DeleteEntry(entry);
}

template<class Event, class List, class Listener>
void RemoveEventListener(Event& event, List& list, Listener* listener)
{
    if (!listener) return;
    UnregisterEventConnection(&event, listener);
    if (event.NativeDelivering())
        listener->mFlags |= event_pending_removal;
    else
        DeleteEventListener(list, listener);
}

template<class Event, class List>
void RemoveAllEventListeners(Event& event, List& list)
{
    CheckNativeEventThread();
    auto* entry = nlDLRingGetStart(list.m_Head);
    const auto count = nlDLRingCountElements(list.m_Head);
    for (unsigned i = 0; i < count; ++i)
    {
        auto* next = entry->m_next;
        RemoveEventListener(event, list, &entry->entry);
        entry = next;
    }
}

template<class List>
void CollectRemovedEventListeners(List& list)
{
    auto* entry = nlDLRingGetStart(list.m_Head);
    const auto count = nlDLRingCountElements(list.m_Head);
    for (unsigned i = 0; i < count; ++i)
    {
        auto* next = entry->m_next;
        if (entry->entry.mFlags & event_pending_removal)
            DeleteEventListener(list, &entry->entry);
        entry = next;
    }
}

template<class Event, class List, class Invoke>
void DeliverEvent(Event& event, List& list, Invoke invoke)
{
    CheckNativeEventThread();
    event.NativeBeginDelivery();
    try
    {
        auto iterator = list.Begin();
        while (iterator.hasNext())
        {
            auto* current = iterator.CurrentEntry();
            auto& listener = *iterator;
            event.NativeSetCurrent(&listener);
            if ((listener.mFlags & (event_enabled | event_pending_removal)) == event_enabled)
            {
                invoke(listener.callback);
                // Preserve the original traversal when a callback appends listeners.
                iterator = list.Begin();
                iterator.m_Curr = current;
            }
            iterator.next();
        }
    }
    catch (...)
    {
        event.NativeEndDelivery();
        CollectRemovedEventListeners(list);
        throw;
    }
    event.NativeEndDelivery();
    CollectRemovedEventListeners(list);
}

template<class Event, class List, class Callback>
void AddEventListener(Event& event, List& list, Callback& callback,
                      EventOwnerHandle owner, int group)
{
    ValidateNativeEventConnection(&event, owner);
    if (!callback) throw std::invalid_argument("Cannot add an empty event callback");
    decltype(list.m_Head) entry = nullptr;
    list.m_Allocator.Allocate(entry);
    if (!entry) throw std::bad_alloc();
    using Entry = std::remove_pointer_t<decltype(entry)>;
    try { new (entry) Entry; }
    catch (...) { list.m_Allocator.Free(entry); throw; }
    auto& listener = entry->entry;
    listener.mNativeEntry = entry;
    try { RegisterEventConnection(&event, &listener, owner, group); }
    catch (...) { entry->~Entry(); list.m_Allocator.Free(entry); throw; }
    listener.callback.UnidentifiedTransfer(callback);
    nlDLRingAddEnd(&list.m_Head, entry);
}
}
