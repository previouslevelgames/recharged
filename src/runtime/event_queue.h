#pragma once

#include "runtime/event_support.h"
#include "NL/nlMemory.h"
#include "NL/nlFunction.inl"
#include <exception>
#include <limits>
#include <memory>
#include <utility>

class EventDispatcher;

namespace mscharged
{
inline constexpr unsigned event_queue_limit = 4096;
inline constexpr unsigned event_dispatch_budget = 65536;

// Lifetime metadata follows the same owning-arena rules as NL callbacks.
template<class T> struct EventAllocator
{
    using value_type = T;
    EventAllocator() = default;
    template<class U> EventAllocator(const EventAllocator<U>&) {}
    T* allocate(std::size_t count)
    {
        if (count > std::numeric_limits<unsigned long>::max() / sizeof(T))
            throw std::bad_alloc();
        return static_cast<T*>(nlMalloc(count * sizeof(T), alignof(T), false));
    }
    void deallocate(T* pointer, std::size_t) noexcept { nlFree(pointer); }
    template<class U> bool operator==(const EventAllocator<U>&) const { return true; }
    template<class U> bool operator!=(const EventAllocator<U>&) const { return false; }
};
struct DispatcherLifetime { bool alive = true; };
std::shared_ptr<DispatcherLifetime> CreateDispatcherLifetime();
void ReleaseDispatcherLifetime(std::shared_ptr<DispatcherLifetime>& lifetime);
void RequireEventQueuesShutdown();
void AddQueuedCallback(EventDispatcher& dispatcher, const Function<bool>& callback);

struct QueuedJob;
struct QueuedEventLifetime
{
    void* event;
    std::weak_ptr<DispatcherLifetime> dispatcher;
    QueuedJob* first = nullptr;
    bool cancelling = false;
    bool closed = false;
    QueuedEventLifetime(void* event, std::weak_ptr<DispatcherLifetime> dispatcher)
        : event(event), dispatcher(std::move(dispatcher)) {}
};
std::shared_ptr<QueuedEventLifetime> CreateQueuedEventLifetime(EventDispatcher* dispatcher, void* event);
void CancelQueuedEvent(std::shared_ptr<QueuedEventLifetime> lifetime, bool close);
void CloseQueuedEvent(const std::shared_ptr<QueuedEventLifetime>& lifetime) noexcept;

struct QueuedJob : std::enable_shared_from_this<QueuedJob>
{
    std::shared_ptr<QueuedEventLifetime> lifetime;
    QueuedJob* next = nullptr;
    QueuedJob* previous = nullptr;
    bool accepted = false;
    bool finished = false;
    explicit QueuedJob(std::shared_ptr<QueuedEventLifetime> lifetime);
    virtual ~QueuedJob();
    void Accept();
    void Detach();
    virtual void Run(bool deliver) = 0;
};

template<class Deliver, class Dispose>
void InvokeQueuedPayload(Deliver deliver, Dispose dispose)
{
    std::exception_ptr failure;
    try { deliver(); } catch (...) { failure = std::current_exception(); }
    try { dispose(); } catch (...) { if (!failure) failure = std::current_exception(); }
    if (failure) std::rethrow_exception(failure);
}

template<class Deliver, class Dispose>
struct TypedQueuedJob final : QueuedJob
{
    Deliver deliver;
    Dispose dispose;
    TypedQueuedJob(std::shared_ptr<QueuedEventLifetime> lifetime, Deliver delivery, Dispose disposal)
        : QueuedJob(std::move(lifetime)), deliver(std::move(delivery)), dispose(std::move(disposal)) {}
    ~TypedQueuedJob()
    {
        // Rejected Queue calls retain caller ownership. Accepted jobs are
        // disposed even if their last callback is released without invocation.
        if (accepted && !finished)
        {
            try { Run(false); } catch (...) { std::terminate(); }
        }
    }
    void Run(bool shouldDeliver) override
    {
        CheckNativeEventThread();
        if (finished) return;
        if (!accepted) throw std::logic_error("Queued callback ran before publication");
        finished = true;
        Detach();
        InvokeQueuedPayload([&] {
            if (shouldDeliver && !lifetime->closed) deliver(lifetime->event);
        }, [&] { dispose(); });
    }
};

template<class Deliver, class Dispose>
void QueueNativeEvent(const std::shared_ptr<QueuedEventLifetime>& lifetime,
                      EventDispatcher* dispatcher, Deliver deliver, Dispose dispose)
{
    CheckNativeEventThread();
    const auto host = lifetime->dispatcher.lock();
    if (!host || !host->alive || lifetime->closed || lifetime->cancelling)
        throw std::logic_error("Queued event or its dispatcher is closed or cancelling");
    using Job = TypedQueuedJob<Deliver, Dispose>;
    auto job = std::allocate_shared<Job>(EventAllocator<Job>{}, lifetime, std::move(deliver), std::move(dispose));
    Function<bool> callback([job](bool dispatch) { job->Run(dispatch); });
    AddQueuedCallback(*dispatcher, callback);
    job->Accept();
}

template<class Dispatcher>
void AddDispatcherCallback(Dispatcher& dispatcher, const typename Dispatcher::Callback& callback)
{
    CheckNativeEventThread();
    if (dispatcher.mNativeClosing || dispatcher.mNativeClearing)
        throw std::logic_error("Cannot add a callback while the dispatcher is closing or clearing");
    if (!callback) throw std::invalid_argument("Cannot queue an empty callback");
    if (dispatcher.state.fields.callbackCount >= event_queue_limit)
        throw std::length_error("Native queued callback limit exceeded");
    auto& list = dispatcher.callbacks;
    decltype(list.m_Head) entry = nullptr;
    list.m_Allocator.Allocate(entry);
    if (!entry) throw std::bad_alloc();
    using Entry = std::remove_pointer_t<decltype(entry)>;
    try { new (entry) Entry(callback); }
    catch (...) { list.m_Allocator.Free(entry); throw; }
    nlDLRingAddEnd(&list.m_Head, entry);
    ++dispatcher.state.fields.callbackCount;
}

template<class Dispatcher>
void ClearDispatcher(Dispatcher& dispatcher)
{
    CheckNativeEventThread();
    if (dispatcher.state.fields.dispatching || dispatcher.mNativeClearing)
        throw std::logic_error("Recursive dispatcher clear or clear during delivery");
    dispatcher.mNativeClearing = true;
    std::exception_ptr failure;
    while (dispatcher.callbacks.m_Head)
    {
        auto* entry = nlDLRingRemoveStart(&dispatcher.callbacks.m_Head);
        --dispatcher.state.fields.callbackCount;
        try { entry->entry(false); } catch (...) { if (!failure) failure = std::current_exception(); }
        dispatcher.callbacks.DeleteEntry(entry);
    }
    dispatcher.state.fields.stopDispatch = false;
    dispatcher.mNativeClearing = false;
    if (failure) std::rethrow_exception(failure);
}

template<class Dispatcher>
void DispatchQueuedCallbacks(Dispatcher& dispatcher, bool oneBatch)
{
    CheckNativeEventThread();
    if (dispatcher.state.fields.dispatching || dispatcher.mNativeClearing || dispatcher.mNativeClosing)
        throw std::logic_error("Recursive dispatch or delivery during dispatcher cleanup");
    dispatcher.state.fields.dispatching = true;
    std::exception_ptr failure;
    unsigned delivered = 0;
    try
    {
        do
        {
            unsigned batch = dispatcher.state.fields.callbackCount;
            while (batch && !dispatcher.state.fields.stopDispatch)
            {
                if (delivered++ == event_dispatch_budget)
                    throw std::length_error("Native event dispatch budget exceeded");
                auto* entry = nlDLRingRemoveStart(&dispatcher.callbacks.m_Head);
                --dispatcher.state.fields.callbackCount;
                try { entry->entry(true); }
                catch (...) { dispatcher.callbacks.DeleteEntry(entry); throw; }
                dispatcher.callbacks.DeleteEntry(entry);
                --batch;
            }
        } while (!oneBatch && dispatcher.state.fields.callbackCount && !dispatcher.state.fields.stopDispatch);
    }
    catch (...) { failure = std::current_exception(); }
    const bool cancel = failure || dispatcher.state.fields.stopDispatch;
    dispatcher.state.fields.dispatching = false;
    dispatcher.state.fields.stopDispatch = false;
    if (cancel)
    {
        try { ClearDispatcher(dispatcher); }
        catch (...) { if (!failure) failure = std::current_exception(); }
    }
    if (failure) std::rethrow_exception(failure);
}

template<class Dispatcher>
void FreeDispatcherBlocks(Dispatcher& dispatcher)
{
    CheckNativeEventThread();
    if (dispatcher.state.fields.dispatching || dispatcher.mNativeClearing)
        throw std::logic_error("Cannot release dispatcher blocks during delivery or clear");
    std::exception_ptr failure;
    try { ClearDispatcher(dispatcher); } catch (...) { failure = std::current_exception(); }
    dispatcher.callbacks.m_Allocator.FreeBlocks();
    if (failure) std::rethrow_exception(failure);
}

template<class Dispatcher>
void CloseDispatcher(Dispatcher& dispatcher) noexcept
{
    if (dispatcher.mNativeClosing) return;
    dispatcher.mNativeClosing = true;
    dispatcher.mNativeLifetime->alive = false;
    try
    {
        FreeDispatcherBlocks(dispatcher);
        ReleaseDispatcherLifetime(dispatcher.mNativeLifetime);
    }
    catch (...) { std::terminate(); }
}
}
