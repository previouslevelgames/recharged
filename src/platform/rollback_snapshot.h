#pragma once

#include <cstddef>
#include <cstdint>

namespace mscharged::platform
{
// Incremental snapshots of the rollback state (the original game's arenas and
// globals), with a history of the last saves to restore any of them.
//
// The memory is tracked by pages. Hot pages (written recently) stay writable
// and are compared with the last save at every save; cold pages are read-only
// and the first write to one faults, which makes it writable and hot. A hot
// page unchanged for a while turns cold (read-only) again. A save copies the
// pages that changed into the shadow (the state of the last save) and keeps
// what they held before as the undo of that save; a restore copies the last
// save back over what changed since, then undoes the later saves one by one.
//
// Only the bytes of the regions are saved and restored: a page the regions
// share with other memory is tracked, but its other bytes are never written.
// Other threads may write those bytes (their faults are handled the same way);
// they must not write the regions' own bytes between a save and a restore.
// Saves and restores run on one thread. Linux and macOS (write protection with
// signals) and Windows (with a vectored exception handler); elsewhere
// ConfigureRollbackSnapshots returns false.
struct RollbackRegion
{
    void* base;
    std::size_t bytes;
};

struct RollbackSnapshotStats
{
    std::size_t pageSize, trackedPages, hotPages;
    std::uint64_t saves, saveNs, maxSaveNs, savedPages, maxSavedPages;
    std::uint64_t restores, restoreNs, maxRestoreNs, restoredPages, maxRestoredPages;
    std::uint64_t faults, promoted, demoted;
    std::uint64_t evicted; // saves dropped to make room in the history before their time
};

// Regions that do not overlap, depth saves kept besides the oldest one, and
// room for historyPages pages of undo. The same configuration again keeps the
// hot pages; a new one forgets them. False: unsupported or out of memory.
bool ConfigureRollbackSnapshots(const RollbackRegion* regions, int count, int depth, std::size_t historyPages);
// The first save, a full copy; tracking starts. Returns its id: later saves
// count up from it.
std::uint64_t BeginRollbackSnapshots();
// An incremental save of everything that changed since the last one.
std::uint64_t SaveRollbackSnapshot();
// The regions as of save id (between OldestRollbackSnapshot and the last save);
// the saves after it are dropped. False when that save is no longer kept.
bool RestoreRollbackSnapshot(std::uint64_t id);
std::uint64_t OldestRollbackSnapshot();
// Tracking stops: every page writable again. The hot pages are remembered.
void EndRollbackSnapshots();
// Diagnostic (reads all the memory): cold pages whose bytes differ from the
// last save without having faulted, written behind the tracking's back (the
// kernel, a device). Call it right before a save. The first one is returned in
// first when there is any.
std::size_t FindUntrackedRollbackWrites(std::uintptr_t* first);
// Diagnostic: every run of region bytes that differs from the last save, in
// address order within a page, passed to visit. Call it right before a save.
void VisitRollbackChanges(void (*visit)(std::uintptr_t address, std::size_t bytes, void* context), void* context);
const RollbackSnapshotStats& RollbackSnapshotStatistics();
}
