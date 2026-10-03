#include "runtime/tasks.h"
#include "runtime/events.h"
#include "runtime/frame_timing.h"
#include "Game/EventConnection.h"
#include "Game/EventRegistry.h"
#include "Game/EventDispatcher.inl"
#include "Game/Sys/movie.h"
#include "NL/MemAlloc.h"
#include "NL/nlFunction.inl"
#include <cmath>

namespace mscharged
{
std::string VerifyStartupTaskScheduler()
{
    const auto standard = StandardAllocator.TotalFreeMemory();
    const auto external = VirtualAllocator.TotalFreeMemory();
    InitializeNativeEventRegistry();
    try
    {
        fn_80115F10();
        nlTaskManager::Startup(0x10000);
        nlTaskManager::AddTask(gDispatchEventsTask, 0x18, 0xFE07FFFF);
        FrameCounter timing("tasks", "completion");
        auto run = [&] {
            timing.StartTimer(0);
            nlTaskManager::RunAllTasks();
            timing.StartTimer(1);
            timing.FinishTiming();
        };
        {
            UnidentifiedQueuedEvent<UnidentifiedEventNoData> event(&gDispatchEventsTask->dispatcher, "NativeScheduledStartup", -1);
            int received = 0, disposed = 0;
            Function<FnVoidVoid> dispose([&] { ++disposed; });
            Function<FnVoidVoid> listener([&] { if (++received == 1) event.Queue(dispose); });
            event.Add(listener, 0, -1);
            event.Queue(dispose);
            run();
            if (received != 1 || disposed != 1)
                throw std::runtime_error("Scheduled dispatcher did not retain its one-batch contract");
            nlTaskManager::SetNextState(0x80000); // Original loading-state exclusion.
            run();
            if (received != 1 || disposed != 1)
                throw std::runtime_error("Inactive scheduled task delivered an event");
            nlTaskManager::SetNextState(4);
            run();
            if (received != 2 || disposed != 2 || IsMovieActive() || MoviePlay()
                || !std::isfinite(gDispatchEventsTask->mExecutionTime)
                || !std::isfinite(nlTaskManager::m_pInstance->mCurrentTimeDelta))
                throw std::runtime_error("Original scheduled event/state/clock check failed");
            const auto snapshot = ReadFrameTiming(timing);
            if (snapshot.pending_frames != 3 || !std::isfinite(snapshot.last_ms[0]) || !std::isfinite(snapshot.last_ms[1]))
                throw std::runtime_error("Original frame timing did not record the scheduled checks");
        }
        ShutdownNativeTaskManager(); // Borrowed task survives manager shutdown.
        ShutdownNativeDispatchTask();
        ShutdownNativeEventRegistry();
    }
    catch (...)
    {
        ShutdownNativeTaskManager();
        ShutdownNativeDispatchTask();
        ShutdownNativeEventRegistry();
        throw;
    }
    if (StandardAllocator.TotalFreeMemory() != standard || VirtualAllocator.TotalFreeMemory() != external)
        throw std::runtime_error("Scheduled task teardown did not recover game allocations");
    return "Original task manager scheduled DispatchEventsTask across state masks and batches; inactive movie path and arena recovery verified.";
}
}
