#pragma once

// The rollback's state, shared by its files in the game module:
//   rollback_engine.cpp   saves after every step, goes back, replays whole
//                         frames; the hooks the game calls (start here)
//   rollback_memory.cpp   what a rollback restores and what it leaves out
//   rollback_netplay.cpp  RECHARGED_ROLLBACK: network matches that predict
//   rollback_compare.cpp  RECHARGED_ROLLBACK_TEST_COMPARE: memory after a replay
//   rollback_audio.cpp    the audio's own heap, outside the rollback
//   rollback_summary.cpp  <file>.summary
// rollback_snapshot.cpp saves and restores the memory, rollback_platform.cpp
// holds what differs between Linux and Windows.
#if !defined(MSCHARGED_NATIVE) || !defined(MSCHARGED_GAME_MODULE)
#error The rollback belongs to the isolated original game module
#endif
#include "mscharged_rollback.h"
#include "mscharged_rollback_audio.h"
#include "mscharged_rollback_netplay.h"

#include "Game/Audio/AudioBackend.h"
#include "Game/Game.h"
#include "Game/InputManager.h"
#include "Game/InputRouter.h"
#include "Game/NetworkInput.h"
#include "Game/NetworkInputMessages.h"
#include "Game/NetworkPeer.h"
#include "Game/NetworkRandomSeed.h"
#include "Game/NetworkSession.h"
#include "Game/NetworkSync.h"
#include "Game/Task/GameTaskState.h"
#include "Game/Effects/EffectsTemplate.h"
#include "Game/Task/DispatchEventsTask.h"
#include "Game/Task/FixedUpdateTask.h"
#include "NL/MemAlloc.h"
#include "NL/gl/glView.h"
#include "NL/globalpad.h"
#include "NL/nlMemory.h"
#include "NL/nlSlotPool.h"
#include "NL/nlTask.h"
#include "NL/platpad.h"
#include "platform/game_allocation_ownership.h"
#include "platform/game_resource_records.h"
#include "platform/rollback_snapshot.h"
#include "platform/tweak_frame_state.h"

#include "platform/rollback_platform.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

// Bounds of the simulation's globals (cmake/rollback_sections.ld; on Windows,
// tools/rollback_sections.py and rollback_platform.cpp; on macOS,
// tools/rollback_sections.py and the bounds the linker defines for a section).
#if defined(_WIN32)
extern "C" char __rbk_data_start[], __rbk_data_end[], __rbk_bss_start[], __rbk_bss_end[];
#elif defined(__APPLE__)
extern "C" char __rbk_data_start[] __asm("section$start$__DATA$__rbkd");
extern "C" char __rbk_data_end[] __asm("section$end$__DATA$__rbkd");
extern "C" char __rbk_bss_start[] __asm("section$start$__DATA$__rbkb");
extern "C" char __rbk_bss_end[] __asm("section$end$__DATA$__rbkb");
// The external zeroed globals, which Mach-O keeps apart (__DATA,__common).
extern "C" char __rbk_common_start[] __asm("section$start$__DATA$__rbkc");
extern "C" char __rbk_common_end[] __asm("section$end$__DATA$__rbkc");
#else
extern "C" char __rbk_data_start[] __attribute__((weak, visibility("hidden")));
extern "C" char __rbk_data_end[] __attribute__((weak, visibility("hidden")));
extern "C" char __rbk_bss_start[] __attribute__((weak, visibility("hidden")));
extern "C" char __rbk_bss_end[] __attribute__((weak, visibility("hidden")));
#endif
// The routers (InputRouter.cpp).
extern InputRouter* gSimpleInputRouter;
extern NetworkInputRouter* gNetworkInputRouter;
// The network transport's slot pools (NL/plat/Transport*.cpp), by their common
// base: a variable's symbol is its name, and SlotPool<NetworkMessageStorage>
// has no header.
extern SlotPoolBase gTransportPacketPool;
extern SlotPoolBase gNetworkMessagePool;
extern SlotPoolBase gTransportConnectionPool;

// No std containers: in the game module the global operator new is the
// original nlMalloc, and the rollback must not allocate in the arenas it restores.
namespace mscharged::rollback
{
constexpr int kMaxDepth = 8;
constexpr int kRing = kMaxDepth + 2;     // steps kept: the deepest rollback and the step it starts from
constexpr int kMaxRanges = 16;
constexpr int kMaxExcluded = 160;
constexpr int kMaxKept = 16;             // ranges inside excluded ones that are restored after all
constexpr int kMaxRegions = 256;
constexpr std::uint64_t kIdleNs = 250'000'000;
constexpr int kSummaryEvery = 120;       // steps (two seconds)
constexpr int kUntrackedEvery = 600;     // steps between checks for writes behind the tracking (reads everything)
constexpr int kListedMismatches = 20;
constexpr int kMaxMeter = 64;            // mega strike meter presses received before one step
constexpr int kMaxWatched = 256;         // ranges the comparison watches although no rollback restores them
constexpr int kMaxPlaces = 4096;         // places the comparison tells apart
constexpr int kDetailedCompares = 3;     // comparisons listed in full in the log
constexpr int kListedPlaces = 60;        // places per detailed comparison, and in the summary
constexpr int kMaxTasks = 32;            // tasks the task manager runs
constexpr float kLongestFrame = 0.1f;    // the task manager's bound on a frame (g_fTaskTimeUpperBound)
constexpr int kFrameRing = 256;          // frames kept: those of the deepest rollback, at any frame rate
constexpr int kMachines = 4;
constexpr int kMachineEvents = 4;        // deterministic data events in one input message
constexpr int kInputRing = 32;           // RECHARGED_ROLLBACK: steps whose inputs are kept
constexpr int kConfirmedRing = 64;       // RECHARGED_ROLLBACK: checksums of steps whose inputs all arrived
constexpr int kRemoteChecks = 32;        // RECHARGED_ROLLBACK: other machines' checksums waiting for ours
constexpr unsigned kSettleSteps = 25;    // RECHARGED_ROLLBACK: steps after tracking starts that predict nothing
constexpr int kMaxInputDelay = 6;        // RECHARGED_ROLLBACK: steps, while predicting
constexpr int kMaxLockstepDelay = 10;    // RECHARGED_ROLLBACK: steps, in a phase without prediction

// The parts of a machine's inputs a misprediction can get wrong: the fields of
// PackedDetInput (include/Game/PackedDetInput.h), the player states, the
// events, and the rest (player count, remap angle).
enum InputPart
{
    kButtons,
    kTriggers,
    kRemoteAccel,
    kNunchukAccel,
    kPointerTargets,
    kPointer,
    kSticks,
    kPlayerState,
    kEvents,
    kOtherInput,
    kInputParts
};

// What a replayed frame does with each task: run it as it ran, leave it out
// (pads, sound, network, disc, movies, saving), or its replay part.
enum TaskReplay
{
    kSkip,
    kRun,
    kFixed, // its steps, after its work before them (RollbackReplayFixedFrame)
    kEnd    // the frame's end without sending it (RollbackReplayEndFrame)
};

// One frame as the task manager ran it: each task's time step and real time,
// < 0 for a task that did not run, and the frame rate option's state at its
// start.
struct FrameRecord
{
    float dt[kMaxTasks];
    float real[kMaxTasks];
    MschargedTweakFrameState tweak;
    // The fixed update's time left over after the frame's steps, which the
    // drawing interpolates with (left out of the rollback, as the clocks).
    float accumulated, interpolation;
};

struct Range
{
    std::uintptr_t start, end;
    const char* name;
};
// What a rollback must not cross: the game's own state and the task manager's.
struct Phases
{
    int game;
    unsigned current, pending;
    bool megaStrike; // a mega strike, from its meter through the goalie's part
    bool operator==(const Phases&) const = default;
};
// One fixed step as it first ran: the save after it, and what its replay needs.
struct Step
{
    unsigned frame;
    std::uint64_t snapshot;
    mscharged::platform::GameAllocationMark mark;
    MschargedRollbackInputs inputs; // what the router applied before it
    unsigned checksum;              // after it, as it first ran (in a network match, as it ran last)
    unsigned seed;                  // the network random seed after it
    Phases phases;                  // after it
    bool reading;                   // a disc read in flight after it
    std::uint64_t readsStarted;     // disc reads started until then
    std::uint64_t hostFrame;        // the frame it ran in
    // The mega strike meter's presses received from the shooter's machine
    // before it, outside the router's inputs.
    bool meter[kMaxMeter];
    int meterCount;
    bool meterOverflowed;
};

// RECHARGED_ROLLBACK: one machine's inputs for one step, as its message gave
// them (each message holds what changed since the machine's previous one) or
// as predicted: its last inputs that arrived, without events.
struct MachineInput
{
    unsigned char players;
    unsigned short remap;
    unsigned char records[4][16];   // PackedDetInput, by the machine's player
    unsigned char states[4];
    int eventCount;
    unsigned char events[kMachineEvents][0x24]; // DetermDataEvent
};
struct StepInputs
{
    unsigned frame; // the step they are for; another value: none kept
    int machineCount;
    MachineInput machines[kMachines];
    bool predicted[kMachines];
};
struct SyncPoint
{
    unsigned frame, checksum, seed;
    int machine;
};

// RECHARGED_ROLLBACK_TEST_COMPARE: a place whose bytes after a replay were not
// those before the restore. A global by its module offset, an allocation by
// its first word's (a vtable names the class) and its size, or a range with
// no live allocation.
enum PlaceKind
{
    kGlobal = 1,
    kAllocation,
    kUnowned,
    kNotRestored // a replay changed it, and nothing takes it back
};
struct Place
{
    int kind;
    std::uintptr_t key;
    std::size_t size;
    std::uint64_t compares, bytes; // comparisons it differed in, bytes that differed in all
    std::uintptr_t firstOffset;    // in the allocation, the first time
    std::uint64_t lastCompare;
    const char* region;
    std::uint8_t before[16], after[16];
};

// The input delay a network match starts with, in steps, and its shortest:
// the same on every machine, since each fills that many first steps with
// the router's starting inputs (mscharged_rollback_network_reset) and a
// machine that filled another number would apply every message at another
// step. Not a setting for that reason.
constexpr int kStartingDelay = 1;

// What the environment asked for, read once at startup.
struct Options
{
    bool netplay;            // RECHARGED_ROLLBACK: network matches predict
    int inputDelay;          // kStartingDelay: the starting and shortest input delay
    unsigned fakeDesyncFrom; // RECHARGED_ROLLBACK_TEST_DESYNC
    int maxDepth;            // RECHARGED_ROLLBACK_TEST_FRAMES
    int interval;            // RECHARGED_ROLLBACK_TEST_INTERVAL
    bool merge;              // frames without steps replayed as one with the next
    int compareEvery;        // RECHARGED_ROLLBACK_TEST_COMPARE
};

// The memory a rollback restores (rollback_memory.cpp): the ranges, less the
// excluded ones, plus the kept ones inside those, as snapshot regions.
struct Memory
{
    Range ranges[kMaxRanges];
    int rangeCount;
    Range excluded[kMaxExcluded];
    int excludedCount;
    Range kept[kMaxKept];
    int keptCount;
    mscharged::platform::RollbackRegion regions[kMaxRegions];
    int regionCount;
};

// The task manager's tasks, in its order, and what a replay does with each.
struct Tasks
{
    nlTask* list[kMaxTasks];
    TaskReplay replays[kMaxTasks];
    int count;
};

// Counts and costs, for the summary.
struct Stats
{
    int starts;
    std::uint64_t stepCount, rollbacks, byDepth[kMaxDepth + 1], replayed;
    std::uint64_t skippedReads, skippedStarts, skippedPhases, skippedEvents, skippedShort;
    std::uint64_t meterReceived, meterRedelivered, framesReplayed, framesMerged;
    std::uint64_t mismatches, nisInReplay, lossyFrees, failedRestores, untracked;
    unsigned firstMismatchFrame;
    std::uint64_t rollbackNs, maxRollbackNs, restoreNs, maxRestoreNs;
    // Where the replays' time goes: each task's, and in the fixed update's the
    // steps' and the saves'.
    std::uint64_t taskNs[kMaxTasks], stepNs, saveNs;
    std::uint64_t replayFaults, replaySavedPages, replayHotPages; // of the replayed steps and their saves
    std::uint64_t markNs; // of the replayed saves, the allocation registry's part
    std::uint64_t savesSkipped;
};

// RECHARGED_ROLLBACK_TEST_COMPARE=N: every Nth rollback, the tracked memory
// after the replay against a copy taken before the restore (rollback_compare.cpp).
struct Compare
{
    std::uint8_t* compareCopy;
    std::size_t compareBytes;
    std::uint64_t compares, comparesDiffering, compareDiffBytes;
    std::uintptr_t moduleBase;
    Range watched[kMaxWatched];
    int watchedCount;
    Place* places;
    int placeCount;
    std::uint64_t placesDropped;
};

// RECHARGED_ROLLBACK: network matches that predict (rollback_netplay.cpp).
struct Netplay
{
    StepInputs inputs[kInputRing];
    unsigned lastInputFrame;          // the last step whose inputs were applied
    unsigned firstPredicted[kMachines]; // by machine: its first step still predicted, 0: none
    bool haveLastReal;
    MachineInput lastReal[kMachines]; // by machine: its last inputs that arrived
    unsigned waitThrough;             // steps through this one wait for all their inputs (they were undone)
    unsigned undoFrom;                // a predicted step changed the match's phase: undone at the frame start
    bool captured;
    unsigned capturedFrame;           // the local inputs were captured through this frame
    bool haveConfirmed;
    unsigned confirmed;               // the last step whose inputs all arrived
    SyncPoint history[kConfirmedRing];
    SyncPoint remote[kRemoteChecks];
    int remoteCount;
    std::uint64_t predictedSteps, lockstepWaits, windowWaits, mispredictions[kMachines], correctionRollbacks;
    std::uint64_t mispredictedParts[kInputParts]; // mispredicted steps that got each part wrong
    std::uint64_t undone, truncated, impossible, desyncs, remoteChecks, predictedDepth[kMaxDepth + 1];
    unsigned firstDesyncFrame;
    // This match's first divergence, for the game's own sync error.
    unsigned matchDesyncFrame;
    bool matchDesync, seedDesync, desyncShown;
    // The time sync: by machine, how far ahead its inputs come here (messages
    // queued, less steps predicted; smoothed), and the same as it measures
    // ours. Half the difference is how far this machine runs ahead of it.
    float queuedAhead[kMachines], reportedAhead[kMachines];
    bool haveReported[kMachines];
    int reportedDelay[kMachines]; // each machine's input delay, as it reports it
    // The input delay: this machine's own, in steps, which it lengthens (one
    // message more) or shortens (one capture less) so that the others predict
    // its inputs about a step; at least the starting one.
    int delay;
    int delayChange;              // +n: n steps more at the next capture; -n: the next n captures left out
    unsigned delayChangedAt;
    std::uint64_t delayLonger, delayShorter;
    unsigned shownDepth; // the most steps predicted at once since the overlay last read it
    float lead;       // steps this machine runs ahead of the furthest behind
    bool slowing;
    double slowedSeconds, leadSum, maxLead;
    std::uint64_t leadSamples;
    bool isPrevFramePhasePredictable; // the phase at the last frame start predicted
    int addedStepsInLockstep;         // input delay added for a phase without prediction, given back after it
};

// Exists from startup when the rollback is on, and never goes away: the
// game's hooks keep one behaviour all session (mscharged_rollback_active).
struct State
{
    FILE* out;         // the log, null without one (RECHARGED_ROLLBACK=1)
    char* summaryPath; // null without a log
    bool stopped;      // given up (tracking could not start) or the game exits: no saves, no rollbacks
    Options options;
    Memory memory;
    Tasks tasks;

    // The match as tracked: a save after every step since tracking started.
    bool active;                     // tracking
    Step steps[kRing];
    unsigned latest;                 // frame of the last step saved
    int stored;                      // consecutive steps saved since tracking started
    unsigned trackingFrom;           // the step tracking started after
    std::uint64_t lastHookNs;
    // What the step under way got before the hook between steps saves it.
    bool haveInputs;
    MschargedRollbackInputs pending; // reported for the step under way
    bool received[kMaxMeter];        // meter presses received since the last step
    int receivedCount;
    bool receivedOverflowed;
    // The frames, as the task manager ran them.
    FrameRecord frames[kFrameRing];
    std::uint64_t hostFrame;         // frames since tracking started, the one under way
    // The random seeds the game's presentation draws from, as they were before
    // a task a replay leaves out (the audio's sounds and the transitions also
    // draw from the default one): put back after it, so that the first run
    // draws what the replay does.
    bool seedsSaved;
    unsigned int defaultSeed, effectsSeed;

    // The rollback and its replay.
    int nextDepth;                   // the forced test's next depth, in turn
    int pendingDepth;                // steps the next frame start goes back, 0: none
    int (*replay)(const MschargedRollbackInputs*);
    unsigned (*checksum)(void);
    bool resimulating;               // a replay is running
    unsigned replayEnd;              // the last step a replay runs
    unsigned saveFrom;               // the first replayed step a later rollback may go back to (0: any)
    mscharged::platform::GameAllocationMark replayMark; // the last kept during a replay

    Stats stats;
    Compare compare;
    Netplay net;
};
extern State* sState;
extern std::mutex sMutex; // the hooks (main thread) and the watchdog

// Whether the rollback saves and rolls back now.
inline bool Running()
{
    return sState && !sState->stopped;
}

inline std::uint64_t NowNs()
{
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                          std::chrono::steady_clock::now().time_since_epoch())
                                          .count());
}

inline int EnvInt(const char* name, int fallback, int low, int high)
{
    const char* text = std::getenv(name);
    if (!text || !*text) return fallback;
    return std::clamp(std::atoi(text), low, high);
}

// rollback_engine.cpp
void Log(const char* format, ...) __attribute__((format(printf, 1, 2)));
void FlushLog();
Phases CurrentPhases();
FrameRecord& Frame(std::uint64_t frame);
int fixedIndexOf();
void Stop(const char* why);
bool RollBack(unsigned target, unsigned replayEnd, int (*replay)(const MschargedRollbackInputs*),
              unsigned (*checksum)(void));

// rollback_memory.cpp
void FindRanges(FixedUpdateTask* task, unsigned long taskBytes);
bool BuildRegions();
bool Excluded(std::uintptr_t address);

// rollback_netplay.cpp
StepInputs& InputsFor(unsigned frame);
bool AnyPredicted(const StepInputs& inputs);
unsigned OldestPredicted();
void StopReplayAt(unsigned frame);
void UpdateConfirmed();
void NetplayFrameStart();
bool OverlayNumbers(int& delay, int& predicted);

// rollback_compare.cpp
void BuildWatched();
bool CopyForCompare();
void CompareAfterReplay(unsigned frame, int depth);
void PrintPlace(FILE* file, const Place& place);

// rollback_audio.cpp
constexpr std::size_t kAudioHeapBytes = std::size_t{12} << 20;
extern void* sAudioHeap;
extern MemoryAllocator sAudioAllocator;
void StartAudioAudit();
void WriteAudit(const char* summaryPath);

// rollback_summary.cpp
void WriteSummary();
} // namespace mscharged::rollback
