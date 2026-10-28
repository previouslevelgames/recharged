// The rollback restores the original game's arenas and globals, as the sync
// test saves them, except what lives on outside the simulation: the network
// session (but its players' inputs), sockets and lobby, the online services
// (DWC's memory, the stats manager, the friend roster), the input routers and
// manager, the pads, the tasks' clocks and the fixed update task's
// accumulated time, the frame memory (used afresh every frame), the disc
// reads' manager and the audio. Everything else, the drawing included, goes
// back with the match, with the native pool records the game points at (their
// own heap) and the registry of the game's memory (its rollback checkpoint).
#include "platform/rollback_state.h"

#include "Game/FriendManager.h"
#include "Game/NetworkStatsManager.h"

// The DWC heap (NetworkSession.cpp).
extern void* gNetworkMemoryPool;

namespace mscharged::rollback
{
namespace
{
void AddRange(const void* base, std::size_t bytes, const char* name)
{
    State& s = *sState;
    if (!base || !bytes || s.memory.rangeCount == kMaxRanges) return;
    const auto start = reinterpret_cast<std::uintptr_t>(base);
    for (int i = 0; i < s.memory.rangeCount; ++i)
        if (start >= s.memory.ranges[i].start && start + bytes <= s.memory.ranges[i].end) return; // inside one already
    s.memory.ranges[s.memory.rangeCount++] = {start, start + bytes, name};
}

void Exclude(const void* base, std::size_t bytes, const char* name)
{
    State& s = *sState;
    if (!base || !bytes) return;
    if (s.memory.excludedCount == kMaxExcluded)
    {
        Log("rollback test: too many exclusions, %s not excluded\n", name);
        return;
    }
    const auto start = reinterpret_cast<std::uintptr_t>(base);
    s.memory.excluded[s.memory.excludedCount++] = {start, start + bytes, name};
}

// The whole original allocation holding pointer (bytes when it has none).
void ExcludeAllocation(const void* pointer, std::size_t bytes, const char* name)
{
    if (!pointer) return;
    mscharged::platform::GameAllocationSpan span{};
    if (mscharged::platform::FindGameAllocationSpan(pointer, 1, span))
        Exclude(span.base, span.bytes, name);
    else if (bytes)
        Exclude(pointer, bytes, name);
    else if (!sState->stats.starts) // once, not at every start of the tracking
        Log("rollback: %s at %p is no allocation the registry knows; not excluded\n", name, pointer);
}

void Keep(const void* base, std::size_t bytes)
{
    State& s = *sState;
    if (!base || !bytes || s.memory.keptCount == kMaxKept) return;
    const auto start = reinterpret_cast<std::uintptr_t>(base);
    s.memory.kept[s.memory.keptCount++] = {start, start + bytes, "kept"};
}
} // namespace

bool Excluded(std::uintptr_t address)
{
    const State& s = *sState;
    for (int i = 0; i < s.memory.keptCount; ++i)
        if (address >= s.memory.kept[i].start && address < s.memory.kept[i].end) return false;
    for (int i = 0; i < s.memory.excludedCount; ++i)
        if (address >= s.memory.excluded[i].start && address < s.memory.excluded[i].end) return true;
    return false;
}

// What the rollback restores, and what lives on outside the simulation.
void FindRanges(FixedUpdateTask* task, unsigned long taskBytes)
{
    State& s = *sState;
    s.memory.rangeCount = s.memory.excludedCount = s.memory.keptCount = 0;
    if (__rbk_data_start && +__rbk_data_end > +__rbk_data_start)
        AddRange(__rbk_data_start, __rbk_data_end - __rbk_data_start, "game data");
    if (__rbk_bss_start && +__rbk_bss_end > +__rbk_bss_start)
        AddRange(__rbk_bss_start, __rbk_bss_end - __rbk_bss_start, "game bss");
#if defined(__APPLE__)
    if (+__rbk_common_end > +__rbk_common_start)
        AddRange(__rbk_common_start, __rbk_common_end - __rbk_common_start, "game common");
#endif
    AddRange(StandardAllocator.m_memory, StandardAllocator.m_memory_size, "MEM1");
    AddRange(VirtualAllocator.m_memory, VirtualAllocator.m_memory_size, "MEM2");
    AddRange(&StandardAllocator, sizeof(StandardAllocator), "StandardAllocator");
    AddRange(&VirtualAllocator, sizeof(VirtualAllocator), "VirtualAllocator");
    AddRange(&CurrentAllocator, sizeof(CurrentAllocator), "CurrentAllocator");
    AddRange(AllocatorStack, sizeof(AllocatorStack), "AllocatorStack");
    AddRange(&AllocatorStackDepth, sizeof(AllocatorStackDepth), "AllocatorStackDepth");
    AddRange(task, taskBytes, "fixedUpdateTask");
    // Native representations of pool records (markers, texture headers) the game points at.
    const void* records = nullptr;
    std::size_t recordBytes = 0;
    mscharged::platform::GameResourceRecordStorage(&records, &recordBytes);
    AddRange(records, recordBytes, "pool records");

    // Audio: the host plays what it describes.
    if (sAudioHeap) Exclude(sAudioHeap, kAudioHeapBytes, "audio heap");
    if (g_pAudioBackend)
    {
        Exclude(g_pAudioBackend->m_AudioAllocator.m_memory, g_pAudioBackend->m_AudioAllocator.m_memory_size,
                "audio memory");
        Exclude(g_pAudioBackend, sizeof(AudioBackend), "AudioBackend");
    }
    // The network: the session goes on receiving, sending and pausing. Its
    // players' inputs (applied from the router before every step) are match state.
    if (NetworkSession* session = g_pNetworkSession)
    {
        ExcludeAllocation(session, sizeof(NetworkSession), "NetworkSession");
        for (int machine = 0; machine < 4; ++machine)
            for (int channel = 0; channel < 4; ++channel)
            {
                NetworkPeerChannel* peerChannel = session->GetPeer(machine)->GetNetworkPeerChannel(channel);
                Keep(&peerChannel->mPrevInput, sizeof(DetInput));
                Keep(&peerChannel->mInput, sizeof(DetInput));
            }
        ExcludeAllocation(session->mDirectSocket, 0, "NetworkSocket");
        ExcludeAllocation(session->mTransport, 0, "LANLobby");
        ExcludeAllocation(session->mLobby, 0, "NetworkLobby");
        ExcludeAllocation(session->mStatsReporter, 0, "NetworkStatsReporter");
        ExcludeAllocation(session->mRankingReporter, 0, "NetworkRanking");
    }
    // Online (WFC): DWC and GameSpy keep their connections in the network
    // memory pool (the login the server checks a ranking report against, the
    // match's transport, the ranking requests), the stats manager queues the
    // reports a match's end makes, and the friend roster follows DWC. They go
    // on outside the simulation; a restore would rewind them under their
    // sockets and their globals, which stay.
    ExcludeAllocation(gNetworkMemoryPool, 0, "network memory pool (DWC)");
    ExcludeAllocation(NetworkStatsManager::Instance(), 0, "NetworkStatsManager");
    ExcludeAllocation(g_pFriendManager, 0, "FriendManager");
    // The transport's slot pools keep their free lists in its globals, left out
    // with the network, and their slots in blocks of the arenas: the blocks go too.
    for (SlotPoolBase* pool : {&gTransportPacketPool, &gNetworkMessagePool, &gTransportConnectionPool})
        for (SlotPoolBlock* block = pool->m_BlockList; block; block = block->next)
            ExcludeAllocation(block, 0, "network transport pool");
    ExcludeAllocation(gSimpleInputRouter, 0, "SimpleInputRouter");
    ExcludeAllocation(gNetworkInputRouter, 0, "NetworkInputRouter");
    ExcludeAllocation(gInputManager, 0, "NetworkInputManager");
    ExcludeAllocation(gNetworkSyncState, 0, "NetworkSyncState");
    // The pads: read again only for new steps.
    if (PadManager* pads = g_pPadManager)
    {
        ExcludeAllocation(pads, sizeof(PadManager), "PadManager");
        if (pads->m_aPads)
        {
            ExcludeAllocation(pads->m_aPads, 0, "PadManager pads");
            for (int i = 0; i < pads->mPadCount * pads->mPadSetCount; ++i)
                if (cGlobalPad* pad = pads->m_aPads[i])
                {
                    ExcludeAllocation(pad, 0, "cGlobalPad");
                    ExcludeAllocation(pad->mBackend, 0, "PadBackend");
                }
        }
    }
    ExcludeAllocation(g_pPlatPadManager, 0, "PlatPadManager");
    // The disc reads under way: the audio's streams read while rollbacks happen.
    const void* reads[2];
    const int readCount = mscharged_rollback_async_read_state(reads, 2);
    for (int i = 0; i < readCount; ++i) ExcludeAllocation(reads[i], 0, "disc reads");
    // The frame memory: used afresh every frame, with its cursors in glxMemory's statics.
    const void* frame[2];
    const int frameCount = mscharged_rollback_frame_memory(frame, 2);
    for (int i = 0; i < frameCount; ++i) ExcludeAllocation(frame[i], 0, "frame memory");
    // The clocks: each task's last run, and the time the fixed steps have still to catch up.
    if (nlTaskManager* manager = nlTaskManager::m_pInstance; manager && manager->mTaskList)
    {
        nlTask* first = manager->mTaskList;
        nlTask* t = first;
        do
        {
            Exclude(&t->mPreviousTicker, sizeof(t->mPreviousTicker), "task clock");
            Exclude(&t->mExecutionTime, sizeof(t->mExecutionTime), "task execution time");
            t = t->m_next;
        } while (t && t != first);
    }
    Exclude(&task->mAccumulatedDeltaT, sizeof(task->mAccumulatedDeltaT), "fixed steps' accumulated time");
    Exclude(&task->mInterpolationDeltaT, sizeof(task->mInterpolationDeltaT), "fixed steps' interpolation time");
}

// The snapshot regions: the ranges without the excluded ones, and the kept
// ones inside those. False when they do not fit.
bool BuildRegions()
{
    State& s = *sState;
    s.memory.regionCount = 0;
    Range cuts[kMaxExcluded];
    std::copy(s.memory.excluded, s.memory.excluded + s.memory.excludedCount, cuts);
    std::sort(cuts, cuts + s.memory.excludedCount, [](const Range& a, const Range& b) { return a.start < b.start; });
    auto add = [&](std::uintptr_t start, std::uintptr_t end) {
        if (start >= end) return true;
        if (s.memory.regionCount == kMaxRegions) return false;
        s.memory.regions[s.memory.regionCount++] = {reinterpret_cast<void*>(start), end - start};
        return true;
    };
    for (int r = 0; r < s.memory.rangeCount; ++r)
    {
        std::uintptr_t at = s.memory.ranges[r].start;
        for (int c = 0; c < s.memory.excludedCount; ++c)
        {
            if (cuts[c].end <= at || cuts[c].start >= s.memory.ranges[r].end) continue;
            if (!add(at, std::max(at, cuts[c].start))) return false;
            at = std::max(at, cuts[c].end);
        }
        if (!add(at, s.memory.ranges[r].end)) return false;
    }
    // Kept ranges lie inside excluded ones, so no region overlaps them;
    // each is restored only where a saved range holds it.
    for (int k = 0; k < s.memory.keptCount; ++k)
        for (int r = 0; r < s.memory.rangeCount; ++r)
        {
            const std::uintptr_t start = std::max(s.memory.kept[k].start, s.memory.ranges[r].start),
                                 end = std::min(s.memory.kept[k].end, s.memory.ranges[r].end);
            if (start < end && !add(start, end)) return false;
        }
    return true;
}
} // namespace mscharged::rollback
