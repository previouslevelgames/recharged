#include "runtime/tasks.h"
#include "runtime/events.h"
#include "Game/EventRegistry.h"
#include "Game/EventDispatcher.inl"
#include "NL/nlTask.h"
#include "NL/nlTicker.h"
#include "NL/MemAlloc.h"
#include "NL/nlFunction.inl"
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <thread>
#include <vector>

// Deterministic OS boundary only: the production nlTicker conversion and
// nlTaskManager algorithms are compiled unchanged into this executable.
namespace { std::uint32_t ticks = 0; }
extern "C" std::uint32_t ChargedFixtureGetTick() { return ticks; }
extern "C" std::uint32_t ChargedFixtureGetBusClock() { return 243000000; }
extern float g_fTaskTimeLowerBound;
extern float g_fTaskTimeUpperBound;

namespace
{
void Require(bool condition, const char* message)
{ if (!condition) throw std::runtime_error(message); }
void Near(float actual, float expected, const char* message)
{ Require(std::isfinite(actual) && std::fabs(actual - expected) < 0.00001f, message); }
template<class Exception = std::logic_error, class Action> void Reject(Action action)
{
    try { action(); } catch (const Exception&) { return; }
    throw std::runtime_error("Invalid scheduler operation was accepted");
}
struct Task : nlTask
{
    int id;
    std::vector<int>* order;
    std::vector<std::pair<unsigned, unsigned>> transitions;
    std::function<void(float)> action;
    std::function<void()> transitionAction;
    std::vector<float> deltas, realDeltas;
    explicit Task(int value = 0, std::vector<int>* trace = nullptr) : id(value), order(trace) {}
    void Run(float delta) override
    {
        if (order) order->push_back(id);
        deltas.push_back(delta);
        realDeltas.push_back(nlTaskManager::m_pInstance->mRealTimeDelta);
        if (action) action(delta);
    }
    const char* GetName() override { return "Scheduler fixture"; }
    void StateTransition(unsigned before, unsigned after) override
    {
        transitions.emplace_back(before, after);
        if (transitionAction) transitionAction();
    }
};

void OrderingAndStates()
{
    nlTaskManager::Startup(1);
    Require(nlTaskManager::m_pInstance->mCurrentTimeDelta == 0
        && nlTaskManager::m_pInstance->mRealTimeDelta == 0, "Initial deltas are undefined");
    nlTaskManager::SetNextState(2);
    nlTaskManager::RunAllTasks();
    Require(nlTaskManager::m_pInstance->mCurrentState == 1, "Empty scheduler changed original transition semantics");
    std::vector<int> order;
    std::array<Task, 16> tasks;
    // Original main registration order through the DPD task (same-priority
    // movie/game and frontend/DPD ordering matters).
    const std::array<unsigned, 16> priority{0,4,2,24,5,8,9,11,11,13,12,15,16,1,17,13};
    for (std::size_t i=0; i<tasks.size(); ++i)
    {
        tasks[i].id=static_cast<int>(i); tasks[i].order=&order;
        nlTaskManager::AddTask(&tasks[i], priority[i], i == 4 ? 1 : 2);
        tasks[i].transitionAction=[&,i] {
            Require(order.empty(), "A task ran before all transition callbacks");
            Require(nlTaskManager::m_pInstance->mCurrentState == 1,
                    "State committed before original transition notification finished");
        };
    }
    nlTaskManager::RunAllTasks();
    Require(order == std::vector<int>({0,13,2,1,5,6,8,7,10,15,9,11,12,14,3}),
            "Original priority/equal-priority or state mask order changed");
    for (const auto& task : tasks)
        Require(task.transitions == std::vector<std::pair<unsigned,unsigned>>({{1,2}}),
                "Transitions did not reach inactive tasks exactly once");
    Require(nlTaskManager::m_pInstance->mPreviousState==1 && nlTaskManager::m_pInstance->mCurrentState==2,
            "State history failed");
    for (auto& task : tasks) task.transitionAction={};
    tasks[0].action=[](float) { nlTaskManager::SetNextState(1); };
    order.clear(); nlTaskManager::RunAllTasks();
    Require(order.size()==15 && nlTaskManager::m_pInstance->mCurrentState==2,
            "Run changed current state within the same frame");
    tasks[0].action={}; order.clear(); nlTaskManager::RunAllTasks();
    Require(order==std::vector<int>({4}) && nlTaskManager::m_pInstance->mPreviousState==2,
            "Pending state was not adopted on the next frame");
    mscharged::ShutdownNativeTaskManager();
    for (const auto& task : tasks)
        Require(!task.mNativeRegistered && !task.m_next && !task.m_prev,
                "Manager freed or retained a borrowed task");

    // Retain the matching game's ring-head edge behavior. Inserting before
    // its first task updates the ring's tail, so that new task runs last.
    nlTaskManager::Startup(1);
    Task first(1,&order), lower(2,&order), equal(3,&order);
    nlTaskManager::AddTask(&first,5,1); nlTaskManager::AddTask(&lower,1,1);
    nlTaskManager::AddTask(&equal,5,1);
    order.clear(); nlTaskManager::RunAllTasks();
    Require(order==std::vector<int>({1,2,3}), "Matching ring-head insertion behavior changed");
    mscharged::ShutdownNativeTaskManager();
}

void Timing()
{
    ticks=0; nlTaskManager::Startup(1);
    Task a, b, inactive;
    nlTaskManager::AddTask(&a,0,1); nlTaskManager::AddTask(&b,1,1); nlTaskManager::AddTask(&inactive,2,2);
    b.mTimeDilated=false;
    nlTaskManager::SetTimeDilation(0.5f);
    a.action=[](float) { ticks+=60750*2; }; // Two milliseconds of execution.
    ticks=60750*20; nlTaskManager::RunAllTasks();
    Near(a.deltas.back(),0.01f,"Dilated task delta failed");
    Near(a.realDeltas.back(),0.02f,"Unclamped real delta failed");
    Near(a.mExecutionTime,2.0f,"Task execution duration must remain milliseconds");
    Near(b.deltas.back(),0.022f,"Each task must sample its own ticker");
    Require(inactive.mPreviousTicker==ticks && inactive.deltas.empty(), "Inactive task did not refresh its ticker");
    a.action={}; ticks+=60750*200; nlTaskManager::RunAllTasks();
    Near(a.deltas.back(),0.05f,"Upper clamp must precede time dilation");
    Near(a.realDeltas.back(),0.202f,"Real delta was incorrectly clamped");
    Near(b.deltas.back(),0.1f,"Undilated upper clamp failed");
    g_fTaskTimeLowerBound=0.01f; nlTaskManager::RunAllTasks();
    Near(a.deltas.back(),0.005f,"Lower clamp must precede time dilation");
    Near(b.deltas.back(),0.01f,"Undilated lower clamp failed");
    nlTaskManager::SetTimeDilation(0); ticks+=60750; nlTaskManager::RunAllTasks();
    Near(a.deltas.back(),0,"Zero dilation did not pause a dilated task");
    g_fTaskTimeLowerBound=0; nlTaskManager::SetTimeDilation(1);
    ticks=0xffff0000; a.mPreviousTicker=ticks; b.mPreviousTicker=ticks;
    ticks+=60750*20; nlTaskManager::RunAllTasks();
    Near(a.deltas.back(),0.02f,"Unsigned ticker rollover failed");
    Near(nlGetTickerDifference(0xffff0000,0xffff0000+60750u*20),20,"Ticker rollover conversion failed");
    Near(nlTicksToMilliseconds(0x20000000),0,"Original 32-bit pre-division shift semantics changed");
    mscharged::ShutdownNativeTaskManager();
}

void OwnershipAndFailure()
{
    Task a,b;
    Reject([] { nlTaskManager::RunAllTasks(); });
    Reject([&] { nlTaskManager::AddTask(&a,0,1); });
    Reject([] { nlTaskManager::SetNextState(1); });
    Reject([] { nlTaskManager::SetTimeDilation(1); });
    nlTaskManager::Startup(1);
    Reject([] { nlTaskManager::Startup(1); });
    Reject<std::invalid_argument>([] { nlTaskManager::AddTask(nullptr,0,1); });
    nlTaskManager::AddTask(&a,0,1);
    Reject([&] { nlTaskManager::AddTask(&a,1,1); });
    Reject<std::invalid_argument>([] { nlTaskManager::SetTimeDilation(-1); });
    Reject<std::invalid_argument>([] { nlTaskManager::SetTimeDilation(std::numeric_limits<float>::infinity()); });
    Reject<std::invalid_argument>([] { nlTaskManager::SetTimeDilation(std::numeric_limits<float>::quiet_NaN()); });
    bool rejected=false;
    std::thread worker([&] { try { nlTaskManager::RunAllTasks(); } catch (const std::logic_error&) { rejected=true; } });
    worker.join(); Require(rejected,"Scheduler accepted a different thread");
    a.action=[&](float) {
        Reject([] { nlTaskManager::RunAllTasks(); });
        Reject([&] { nlTaskManager::AddTask(&b,1,1); });
        Reject([&] { mscharged::RemoveNativeTask(&a); });
        Reject([] { mscharged::ShutdownNativeTaskManager(); });
    };
    nlTaskManager::RunAllTasks(); a.action={};
    g_fTaskTimeLowerBound=1;
    Reject<std::invalid_argument>([] { nlTaskManager::RunAllTasks(); });
    g_fTaskTimeLowerBound=0; nlTaskManager::RunAllTasks();
    {
        Task scoped;
        nlTaskManager::AddTask(&scoped,1,1);
    }
    Require(a.m_next==&a && a.m_prev==&a,"Scoped task destruction left a dangling ring entry");
    mscharged::RemoveNativeTask(&a); mscharged::RemoveNativeTask(&a);
    Require(!nlTaskManager::m_pInstance->mTaskList,"Last task removal did not clear the ring");
    nlTaskManager::AddTask(&a,0,1);
    a.action=[](float) { throw std::runtime_error("fixture run failure"); };
    Reject<std::runtime_error>([] { nlTaskManager::RunAllTasks(); });
    a.action={}; Reject([] { nlTaskManager::RunAllTasks(); });
    mscharged::ShutdownNativeTaskManager();
    nlTaskManager::Startup(1); nlTaskManager::AddTask(&a,0,1);
    a.transitionAction=[] { throw std::runtime_error("fixture transition failure"); };
    nlTaskManager::SetNextState(2);
    Reject<std::runtime_error>([] { nlTaskManager::RunAllTasks(); });
    Require(nlTaskManager::m_pInstance->mCurrentState==1,"Failed transition published a new state");
    Reject([] { nlTaskManager::RunAllTasks(); });
    mscharged::ShutdownNativeTaskManager();
    mscharged::ShutdownNativeTaskManager();

    const auto free=StandardAllocator.TotalFreeMemory();
    std::vector<void*> pressure;
    try { while (true) pressure.push_back(nlMalloc(128,8,false)); }
    catch (const std::bad_alloc&) {}
    try { while (true) pressure.push_back(nlMalloc(1,8,false)); }
    catch (const std::bad_alloc&) {}
    Reject<std::bad_alloc>([] { nlTaskManager::Startup(1); });
    Require(!nlTaskManager::m_pInstance,"Failed startup published a manager");
    for (void* allocation : pressure) nlFree(allocation);
    Require(StandardAllocator.TotalFreeMemory()==free,"Task startup failure leaked memory");
}

void RealDispatch()
{
    InitializeNativeEventRegistry(); fn_80115F10(); nlTaskManager::Startup(1);
    nlTaskManager::AddTask(gDispatchEventsTask,24,1);
    int received=0,cancelled=0;
    Function<bool> callback([&](bool deliver) {
        if (deliver) ++received; else ++cancelled;
    });
    gDispatchEventsTask->dispatcher.Add(callback);
    nlTaskManager::RunAllTasks();
    Require(received==1 && !cancelled,"Original scheduled dispatch failed");
    gDispatchEventsTask->dispatcher.Add(callback);
    mscharged::ShutdownNativeDispatchTask();
    Require(!nlTaskManager::m_pInstance->mTaskList && cancelled==1,
            "Dispatch task teardown did not detach and cancel its queue");
    nlTaskManager::RunAllTasks();
    mscharged::ShutdownNativeTaskManager(); ShutdownNativeEventRegistry();
}
}

int main()
{
    try
    {
        Reject([] { nlTaskManager::Startup(1); });
        alignas(64) static std::array<std::byte,2*1024*1024> standard{},external{};
        StandardAllocator.Initialize(standard.data(),standard.size());
        VirtualAllocator.Initialize(external.data(),external.size());
        CurrentAllocator=&StandardAllocator; gMemoryInitialized=1;
        for (int round=0; round<3; ++round)
        {
            OrderingAndStates(); Timing(); OwnershipAndFailure(); RealDispatch();
            Require(StandardAllocator.TotalFreeMemory()==standard.size()
                && VirtualAllocator.TotalFreeMemory()==external.size(),"Task teardown leaked game allocations");
        }
        gMemoryInitialized=0; CurrentAllocator=nullptr; StandardAllocator={}; VirtualAllocator={};
        std::cout << "Original scheduling: priorities/masks/transitions, per-task timing, clamps/dilation/rollover,\n"
                     "borrowed lifetimes, OOM/reentrancy/failure and dispatch task; three arena recoveries passed\n";
        return 0;
    }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
