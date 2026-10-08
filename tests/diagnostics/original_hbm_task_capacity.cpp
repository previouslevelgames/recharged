#include "revolution/hbm/nw4hbm/snd/TaskNativeStorage.h"
#include <array>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <new>
#include <stdexcept>

// Explicit caller-owned leaf. Only this test TU bypasses private access for
// observation. The original TaskManager/MEM methods construct and own the
// pool, allocate, queue, execute, cancel and recycle every test task.
namespace {
using nw4hbm::snd::SoundArchivePlayer;
using nw4hbm::snd::detail::Task;
using nw4hbm::snd::detail::TaskManager;
using Geometry = TaskManager::NativeTaskStorage;
unsigned checks;
std::array<unsigned, TASK_NUM + 8> executed;
unsigned executed_count;
unsigned cancelled_count;

void Check(bool value, const char* message) {
    ++checks;
    if (!value) throw std::runtime_error(message);
}
u32 FreeBlocks(TaskManager& manager) {
    return MEMCountFreeBlockForUnitHeap(manager.mHeapHandle);
}
void CheckEmpty(TaskManager& manager) {
    Check(manager.mCurrentTask == nullptr, "Original current task survived execution");
    Check(manager.mMutex.thread == nullptr && manager.mMutex.count == 0,
        "Original task execution retained its mutex owner");
    for (unsigned priority = 0; priority < TaskManager::PRIORITY_MAX; ++priority)
        Check(manager.mTaskList[priority].IsEmpty(), "Original priority list retained a task");
    Check(FreeBlocks(manager) == 128, "Original task pool did not recover all 128 blocks");
}
class ObservedTask final : public Task {
public:
    ObservedTask(unsigned serial, u32 id) : Task(id), serial_(serial) {
        std::memset(payload_, static_cast<unsigned char>(serial ^ 0xa5), sizeof(payload_));
    }
    void Execute() override {
        for (auto byte : payload_)
            Check(byte == static_cast<unsigned char>(serial_ ^ 0xa5),
                "Original queue/execute changed a live task payload");
        Check(executed_count < executed.size(), "Unexpected extra source task execution");
        executed[executed_count++] = serial_;
    }
    void Cancel() override { ++cancelled_count; }
private:
    unsigned serial_;
    unsigned char payload_[48];
};

ObservedTask* Append(TaskManager& manager, unsigned serial, u32 id,
    TaskManager::TaskPriority priority) {
    auto* storage = manager.Alloc();
    Check(sizeof(ObservedTask) <= manager.GetTaskBufferSize(),
        "Test task does not fit the original returned native buffer size");
    auto* task = new (storage) ObservedTask(serial, id);
    manager.AppendTask(task, priority);
    return task;
}

using TaskId = decltype(static_cast<const Task*>(nullptr)->GetTaskId());
struct IdTask final : Task {
    unsigned* executed;
    unsigned* cancelled;
    TaskId opposite;
    bool cancel_self;
    IdTask(TaskId id, unsigned& execution, unsigned& cancellation,
           TaskId other, bool self)
        : Task(id), executed(&execution), cancelled(&cancellation),
          opposite(other), cancel_self(self) {}
    void Execute() override {
        ++*executed;
        if (cancel_self) {
            auto& manager = TaskManager::GetInstance();
            manager.CancelByTaskId(opposite);
            Check(*cancelled == 0, "A distinct full-width ID cancelled the executing task");
            manager.CancelByTaskId(GetTaskId());
        }
    }
    void Cancel() override { ++*cancelled; }
};
void CheckPointerTaskIds(TaskManager& manager) {
    static_assert(sizeof(TaskId) == sizeof(std::uintptr_t));
    static_assert(sizeof(u32) == 4);
    unsigned owners[2] = {0x1234, 0x5678}; // Retained caller-owned native objects.
    unsigned executions{}, cancellations{};
    for (auto& owner : owners) {
        const auto id = reinterpret_cast<std::uintptr_t>(&owner);
        auto* task = new (manager.Alloc()) IdTask(id, executions, cancellations, 0, false);
        Check(task->GetTaskId() == id, "Original Task lost native pointer carrier bits");
        manager.Free(task);
    }
    // The task API compares integers; this second value is deliberately an
    // integer collision oracle, never converted into or dereferenced as a
    // pointer. The separate Linux fixture uses two actual mmap-backed owners.
    const TaskId a = reinterpret_cast<std::uintptr_t>(&owners[0]);
    const TaskId b = a ^ (std::uintptr_t{1} << 32);
    Check(a != b && static_cast<u32>(a) == static_cast<u32>(b),
        "The full-width ID equality oracle does not distinguish high bits");
    auto append = [&](TaskId id, TaskId opposite, bool self,
                      TaskManager::TaskPriority priority) {
        Check(sizeof(IdTask) <= manager.GetTaskBufferSize(), "ID observer exceeds the real MEM task block");
        auto* task = new (manager.Alloc()) IdTask(id, executions, cancellations, opposite, self);
        manager.AppendTask(task, priority);
    };
    for (unsigned priority = 0; priority < TaskManager::PRIORITY_MAX; ++priority) {
        append(a,b,false,static_cast<TaskManager::TaskPriority>(priority));
        append(b,a,false,static_cast<TaskManager::TaskPriority>(priority));
    }
    manager.CancelByTaskId(a);
    Check(FreeBlocks(manager) == 125 && executions == 0 && cancellations == 0,
        "Original queued cancellation aliased high bits or changed its free-without-callback behavior");
    for (unsigned priority = 0; priority < TaskManager::PRIORITY_MAX; ++priority) {
        auto prio = static_cast<TaskManager::TaskPriority>(priority);
        auto* task = manager.PopTask(prio);
        Check(task && task->GetTaskId() == b, "Original queue discarded the distinct full-width ID");
        manager.Free(task);
        Check(!manager.PopTask(prio), "Original cancel left an unexpected queued ID");
    }
    append(a,b,true,TaskManager::PRIORITY_HIGH);
    Check(manager.ExecuteSingle() && executions == 1 && cancellations == 1,
        "Original current-task cancellation did not use complete ID equality");
    cancellations = 0;
    append(b,a,true,TaskManager::PRIORITY_LOW);
    Check(manager.ExecuteSingle() && executions == 2 && cancellations == 1,
        "Original reused current task aliased the opposite ID");
    Check(!manager.ExecuteSingle() && owners[0] == 0x1234 && owners[1] == 0x5678,
        "Source ID equality changed retained owners or left executable tasks");
    CheckEmpty(manager);
}

void CheckPriorityOrder() {
    unsigned index = 0;
    for (int priority = TaskManager::PRIORITY_HIGH; priority >= TaskManager::PRIORITY_LOW; --priority)
        for (unsigned serial = 0; serial < 128; ++serial)
            if (serial % 3 == static_cast<unsigned>(priority))
                Check(executed[index++] == serial,
                    "Original task priority/FIFO execution order changed");
    Check(index == 128 && executed_count == 128, "Original execution omitted a queued task");
}
}

#define TEST_API extern "C" __attribute__((visibility("default")))
TEST_API const void* charged_hbm_task_storage_owner() { return TaskManager::mTaskArea; }
TEST_API std::size_t charged_hbm_task_storage_bytes() { return Geometry::StorageBytes; }

TEST_API unsigned charged_hbm_task_capacity() {
    checks = executed_count = cancelled_count = 0;
    auto& manager = TaskManager::GetInstance(); // Caller-labelled source construction.
    Check(TASK_NUM == 128, "Original logical task capacity changed");
    CheckEmpty(manager); // The old 126-block projection fails here, before exhaustion.
    const auto size = manager.GetTaskBufferSize();
    const auto* unit = reinterpret_cast<const MEMiUntHeapHead*>(
        reinterpret_cast<const unsigned char*>(manager.mHeapHandle) + sizeof(MEMiHeapHead));
    Check(size == unit->blockSize, "Source query did not read the real native unit header");
    Check(size >= sizeof(SoundArchivePlayer::SeqLoadTask)
        && size >= sizeof(SoundArchivePlayer::StrmHeaderLoadTask)
        && size >= sizeof(SoundArchivePlayer::StrmDataLoadTask),
        "At least one original SoundArchivePlayer task exceeds its real pool block");

    const auto storage = reinterpret_cast<std::uintptr_t>(TaskManager::mTaskArea);
    const auto first = reinterpret_cast<std::uintptr_t>(manager.mHeapHandle->heapStart);
    const auto end = reinterpret_cast<std::uintptr_t>(manager.mHeapHandle->heapEnd);
    Check(storage % 32 == 0 && first >= storage && end >= first
        && end <= storage + Geometry::StorageBytes,
        "Real source heap left its actual native static storage");
    Check((end - first) / size == 128, "Real source heap geometry does not contain 128 blocks");

    std::array<void*, 128> blocks{};
    for (unsigned i = 0; i < blocks.size(); ++i) {
        blocks[i] = manager.Alloc();
        const auto address = reinterpret_cast<std::uintptr_t>(blocks[i]);
        Check(address >= first && address <= end && size <= end - address,
            "Original allocation does not contain its complete returned buffer");
        Check(address % alignof(SoundArchivePlayer::SeqLoadTask) == 0
            && address % alignof(SoundArchivePlayer::StrmHeaderLoadTask) == 0
            && address % alignof(SoundArchivePlayer::StrmDataLoadTask) == 0,
            "Original buffer is not aligned for a real source task");
        Check(FreeBlocks(manager) == 127 - i, "Original allocation lost a free-list block");
        std::memset(blocks[i], static_cast<unsigned char>(i ^ 0x5a), size);
    }
    auto ordered = blocks;
    std::sort(ordered.begin(), ordered.end(), [](const void* left, const void* right) {
        return reinterpret_cast<std::uintptr_t>(left) < reinterpret_cast<std::uintptr_t>(right);
    });
    for (unsigned i = 1; i < ordered.size(); ++i)
        Check(reinterpret_cast<std::uintptr_t>(ordered[i - 1]) + size
            <= reinterpret_cast<std::uintptr_t>(ordered[i]),
            "Two simultaneously live original allocations overlap");
    for (unsigned i = 0; i < blocks.size(); ++i) {
        const auto* bytes = static_cast<const unsigned char*>(blocks[i]);
        for (u32 j = 0; j < size; ++j)
            Check(bytes[j] == static_cast<unsigned char>(i ^ 0x5a),
                "A full source-sized task buffer changed through another allocation");
    }
    for (auto i = blocks.size(); i != 0; --i) manager.Free(blocks[i - 1]);
    CheckEmpty(manager);

    // Fill the real pool again with abstract-observer tasks. Their payload is
    // fixture-only; original source still owns every execution and recycle.
    for (unsigned serial = 0; serial < 128; ++serial)
        Append(manager, serial, serial + 1,
            static_cast<TaskManager::TaskPriority>(serial % 3));
    Check(FreeBlocks(manager) == 0, "Original queue did not own all 128 task blocks");
    auto* borrowed = manager.Alloc(); // Literal original exhausted-pool ExecuteSingle retry.
    Check(executed_count == 1 && executed[0] == 2 && borrowed == blocks[2],
        "Original allocation retry did not execute/reuse the first highest-priority task");
    manager.Free(borrowed);
    manager.Execute();
    CheckPriorityOrder();
    CheckEmpty(manager);

    executed_count = 0;
    for (unsigned serial = 0; serial < 6; ++serial)
        Append(manager, serial, serial % 2 ? 9 : 7,
            static_cast<TaskManager::TaskPriority>(serial % 3));
    manager.CancelByTaskId(7);
    Check(FreeBlocks(manager) == 125 && cancelled_count == 0,
        "Original queued cancellation/free quirk changed");
    manager.Execute();
    Check(executed_count == 3 && executed[0] == 5 && executed[1] == 1 && executed[2] == 3,
        "Original queued cancellation changed survivor priority/FIFO order");
    CheckEmpty(manager);
    CheckPointerTaskIds(manager);
    std::printf("Original HBM TaskManager: %u checks, 128 live full-buffer allocations, "
        "source priority/FIFO/retry/cancel/recycle and full-width ID equality; block %u, storage %zu. "
        "Actual SoundArchivePlayer loading/full HBM remain unqualified.\n",
        checks, size, Geometry::StorageBytes);
    return checks;
}
