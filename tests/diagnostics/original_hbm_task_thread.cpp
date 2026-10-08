#include "revolution/hbm/nw4hbm/snd/TaskManager.h"
#include "revolution/hbm/nw4hbm/snd/TaskThread.h"
#include <cstddef>

// Test-only interfaces. The original methods own construction, worker queue
// setup, task execution, voluntary DONE and join. Private access is read-only.
using nw4hbm::snd::detail::TaskManager;
using nw4hbm::snd::detail::TaskThread;
#define TEST_API extern "C" __attribute__((visibility("default")))

TEST_API void* charged_hbm_task_thread_owner() { return &TaskThread::GetInstance(); }
TEST_API bool charged_hbm_task_thread_create(s32 priority) {
    return TaskThread::GetInstance().Create(priority);
}
TEST_API BOOL charged_hbm_task_thread_destroy() { return TaskThread::GetInstance().Destroy(); }
TEST_API void charged_hbm_task_thread_wake() { TaskThread::GetInstance().SendWakeupMessage(); }
TEST_API void* charged_hbm_task_manager_owner() { return &TaskManager::GetInstance(); }
TEST_API u32 charged_hbm_task_free_blocks() {
    return MEMCountFreeBlockForUnitHeap(TaskManager::GetInstance().mHeapHandle);
}
TEST_API u32 charged_hbm_task_block_size() {
    const auto* common = TaskManager::GetInstance().mHeapHandle;
    const auto* unit = reinterpret_cast<const MEMiUntHeapHead*>(
        reinterpret_cast<const unsigned char*>(common) + sizeof(MEMiHeapHead));
    return unit->blockSize;
}
