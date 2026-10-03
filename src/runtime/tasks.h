#pragma once
#include <string>

class nlTask;
namespace mscharged
{
// The original ring borrows tasks: detach before freeing a task or its arena.
void RemoveNativeTask(nlTask* task);
void ShutdownNativeTaskManager();
std::string VerifyStartupTaskScheduler();

// Entry checks used by the prepared original scheduler.
void PrepareNativeTaskStartup();
void FinishNativeTaskStartup();
void CheckNativeTaskThread();
void CheckNativeTaskInsertion(nlTask* task);
void CheckNativeTaskDilation(float dilation);
void DestroyNativeTask(nlTask* task) noexcept;

class NativeTaskFrame
{
public:
    NativeTaskFrame();
    ~NativeTaskFrame();
    NativeTaskFrame(const NativeTaskFrame&) = delete;
    NativeTaskFrame& operator=(const NativeTaskFrame&) = delete;
private:
    int exceptions;
};
}
