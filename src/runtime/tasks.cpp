#include "runtime/tasks.h"
#include "NL/nlTask.h"
#include "NL/nlDLRing.h"
#include "NL/nlMemory.h"
#include <cmath>
#include <exception>
#include <limits>
#include <stdexcept>
#include <thread>

extern float g_fTaskTimeLowerBound;
extern float g_fTaskTimeUpperBound;
namespace mscharged
{
namespace
{
std::thread::id taskThread;
bool running = false;
bool failed = false;
}

void PrepareNativeTaskStartup()
{
    if (nlTaskManager::m_pInstance) throw std::logic_error("Task manager is already initialized");
    if (!gMemoryInitialized) throw std::logic_error("Task manager requires initialized game memory");
}
void FinishNativeTaskStartup()
{
    taskThread = std::this_thread::get_id();
    running = failed = false;
}
void CheckNativeTaskThread()
{
    if (!nlTaskManager::m_pInstance || taskThread != std::this_thread::get_id())
        throw std::logic_error("Task operation requires its initialized owner thread");
}
void CheckNativeTaskInsertion(nlTask* task)
{
    CheckNativeTaskThread();
    if (running || failed) throw std::logic_error("Cannot insert into a running or failed task manager");
    if (!task) throw std::invalid_argument("Cannot register a null task");
    if (task->mNativeRegistered) throw std::logic_error("Task is already registered");
}
void CheckNativeTaskDilation(float dilation)
{
    CheckNativeTaskThread();
    if (!std::isfinite(dilation) || dilation < 0.0f
        || !std::isfinite(g_fTaskTimeUpperBound)
        || g_fTaskTimeUpperBound < 0.0f
        || (g_fTaskTimeUpperBound > 1.0f
            && dilation > std::numeric_limits<float>::max() / g_fTaskTimeUpperBound))
        throw std::invalid_argument("Task time dilation must produce a finite nonnegative delta");
}
NativeTaskFrame::NativeTaskFrame() : exceptions(std::uncaught_exceptions())
{
    CheckNativeTaskThread();
    if (running || failed) throw std::logic_error("Cannot run a recursive or previously failed task frame");
    if (!std::isfinite(g_fTaskTimeLowerBound) || g_fTaskTimeLowerBound < 0.0f
        || g_fTaskTimeLowerBound > g_fTaskTimeUpperBound)
        throw std::invalid_argument("Invalid task delta bounds");
    CheckNativeTaskDilation(nlTaskManager::m_pInstance->mTimeDilation);
    running = true;
}
NativeTaskFrame::~NativeTaskFrame()
{
    failed = failed || std::uncaught_exceptions() > exceptions;
    running = false;
}
void RemoveNativeTask(nlTask* task)
{
    if (!task) throw std::invalid_argument("Cannot detach a null task");
    if (!task->mNativeRegistered) return;
    CheckNativeTaskThread();
    if (running) throw std::logic_error("Cannot detach a task during scheduler callbacks");
    nlDLRingRemove(&nlTaskManager::m_pInstance->mTaskList, task);
    task->m_next = task->m_prev = nullptr;
    task->mNativeRegistered = false;
}
void DestroyNativeTask(nlTask* task) noexcept
{
    try { RemoveNativeTask(task); }
    catch (...) { std::terminate(); } // Never leave a freed task in the intrusive ring.
}
void ShutdownNativeTaskManager()
{
    auto* manager = nlTaskManager::m_pInstance;
    if (!manager) return;
    CheckNativeTaskThread();
    if (running) throw std::logic_error("Cannot destroy the task manager during callbacks");
    while (manager->mTaskList) RemoveNativeTask(manager->mTaskList);
    nlTaskManager::m_pInstance = nullptr;
    manager->~nlTaskManager();
    nlFree(manager);
    taskThread = {};
    failed = false;
}
}
