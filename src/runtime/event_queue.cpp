#include "runtime/event_queue.h"
#include "Game/EventDispatcher.inl"

namespace mscharged
{
namespace { unsigned dispatchers = 0, jobs = 0; }
std::shared_ptr<DispatcherLifetime> CreateDispatcherLifetime()
{
    CheckNativeEventThread();
    auto lifetime = std::allocate_shared<DispatcherLifetime>(EventAllocator<DispatcherLifetime>{});
    ++dispatchers;
    return lifetime;
}
void ReleaseDispatcherLifetime(std::shared_ptr<DispatcherLifetime>& lifetime)
{
    CheckNativeEventThread();
    lifetime.reset();
    --dispatchers;
}
void RequireEventQueuesShutdown()
{
    if (dispatchers || jobs) throw std::logic_error("Queued dispatchers or callbacks survived event shutdown");
}
void AddQueuedCallback(EventDispatcher& dispatcher, const Function<bool>& callback)
{ dispatcher.Add(callback); }
std::shared_ptr<QueuedEventLifetime> CreateQueuedEventLifetime(EventDispatcher* dispatcher, void* event)
{
    CheckNativeEventThread();
    if (!dispatcher || !dispatcher->mNativeLifetime->alive)
        throw std::invalid_argument("Queued event requires a live dispatcher");
    return std::allocate_shared<QueuedEventLifetime>(EventAllocator<QueuedEventLifetime>{}, event, dispatcher->mNativeLifetime);
}
QueuedJob::QueuedJob(std::shared_ptr<QueuedEventLifetime> target) : lifetime(std::move(target))
{
    ++jobs;
}
void QueuedJob::Accept()
{
    accepted = true;
    next = lifetime->first;
    if (next) next->previous = this;
    lifetime->first = this;
}
QueuedJob::~QueuedJob() { Detach(); --jobs; }
void QueuedJob::Detach()
{
    if (previous) previous->next = next;
    else if (lifetime->first == this) lifetime->first = next;
    if (next) next->previous = previous;
    previous = next = nullptr;
}
void CancelQueuedEvent(std::shared_ptr<QueuedEventLifetime> lifetime, bool close)
{
    CheckNativeEventThread();
    if (lifetime->cancelling) throw std::logic_error("Recursive queued event cancellation");
    if (close) { lifetime->closed = true; lifetime->event = nullptr; }
    lifetime->cancelling = true;
    std::exception_ptr failure;
    while (lifetime->first)
    {
        // A disposer can clear the dispatcher and release its queued callback.
        // Keep both the job and event token alive until cancellation returns.
        auto job = lifetime->first->shared_from_this();
        try { job->Run(false); } catch (...) { if (!failure) failure = std::current_exception(); }
    }
    lifetime->cancelling = false;
    if (failure) std::rethrow_exception(failure);
}
void CloseQueuedEvent(const std::shared_ptr<QueuedEventLifetime>& lifetime) noexcept
{
    try { CancelQueuedEvent(lifetime, true); } catch (...) { std::terminate(); }
}
}
