// The rollback of the native port. It does nothing unless one of these is set:
//
// RECHARGED_ROLLBACK=<file> plays network matches with rollback: a step
// whose inputs have not arrived from another machine runs with predicted ones,
// and goes back when they arrive and differ (rollback_netplay.cpp, tweak
// patch 0030).
//
// RECHARGED_ROLLBACK_TEST=<file> rolls matches back while they are played,
// offline and in network matches, to test the rollback before it predicts
// anything: after every fixed step it saves the match (platform/rollback_snapshot.h),
// and every RECHARGED_ROLLBACK_TEST_INTERVAL steps (default 1) it goes back 1
// to RECHARGED_ROLLBACK_TEST_FRAMES steps (default 8, in turn) and runs them
// again with the inputs they first had (tweak patch 0029). It goes back at
// the start of the next frame, before any task sets up its cameras and views,
// and replays through the end of the last step's frame, frames without steps
// as one with the next (RECHARGED_ROLLBACK_TEST_MERGE=0: one by one). The
// game's checksum after each replayed step must be the one the step first
// gave; in a network match the machines also compare their checksums as the
// original game does.
//
// <file> logs the session; <file>.summary (rollback_summary.cpp) holds its
// counts and costs. Diagnostic only.
//
// A replay runs whole frames again, as the task manager ran them, each task
// with the time step it had and the frame rate option's state each frame
// started with: the drawing recomputes its own state with the match's (with
// nowhere to draw, GLView drops it), and the frame's end empties the views
// and the frame memory without sending anything. The pads, sound, network,
// disc and movies stay out, and the fixed update runs the recorded steps: no
// pads, no capture, nothing sent, no deterministic data queued (it went out
// the first time). The mega strike meter's presses, which come from the
// shooter's machine outside the inputs, go again to the steps that received
// them.
//
// A rollback is skipped when a disc read into the game's memory was in flight
// or started, or the game or task state changed, in the steps it would undo
// (their completion, presentation and loading went into memory it restores).
// Tracking stops when no step comes for a quarter of a second (pause, end of
// match).
//
// The hooks the game calls are at the end of this file; the other files of
// the rollback are listed in rollback_state.h.
#include "Game/Team.h"
#include "Game/AI/Fielder.h"
#include "Game/Goalie.h"
#include "platform/rollback_state.h"

#if !defined(_WIN32)
#include <dlfcn.h>
#endif
#include <cstdarg>
#include <iterator>
#include <typeinfo>

// The game's own flag (Ball.cpp): false while a mega strike freezes everyone
// but its shooter, from its result through the goalie's part.
extern bool lbl_806DB5A8;

namespace mscharged::rollback
{
State* sState;
std::mutex sMutex;

void Log(const char* format, ...)
{
    if (!sState->out) return;
    va_list arguments;
    va_start(arguments, format);
    std::vfprintf(sState->out, format, arguments);
    va_end(arguments);
}

void FlushLog()
{
    if (sState->out) std::fflush(sState->out);
}

namespace
{
// A mega strike, from its meter through the goalie's part: the game reads the
// shooter's pad and the goalie's pointer outside the inputs and starts disc
// reads, so no step of it is predicted (and the step that starts it, when
// predicted, runs again with the inputs that arrive, as a phase change does).
bool MegaStrikeUnderway()
{
    if (!lbl_806DB5A8) return true;
    for (int i = 0; i < 2; ++i)
    {
        cTeam* team = g_pTeams[i];
        if (!team) continue;
        const cFielder* captain = team->GetCaptain();
        if (captain && (captain->m_eActionState == ACTION_SHOOT_TO_SCORE
                        || captain->m_eActionState == ACTION_MEGA_STRIKE))
            return true;
        const Goalie* goalie = team->GetGoalie();
        if (goalie && goalie->mGoalieActionState == GOALIEACTION_MEGA_STRIKE) return true;
    }
    return false;
}
} // namespace

Phases CurrentPhases()
{
    Phases phases{-2, 0, 0, false};
    if (g_pGame) phases.game = g_pGame->GetGameState();
    if (nlTaskManager::m_pInstance)
    {
        phases.current = nlTaskManager::m_pInstance->mCurrentState;
        phases.pending = nlTaskManager::m_pInstance->mPendingState;
    }
    phases.megaStrike = g_pGame && MegaStrikeUnderway();
    return phases;
}

namespace
{
// What a rollback past this step must check.
void RecordFrame(Step& step)
{
    step.phases = CurrentPhases();
    step.timeScale = FixedUpdateTask::GetTargetTimeScale();
    const auto producers = mscharged::platform::CurrentGameByteProducers();
    step.reading = producers.pending != 0;
    step.readsStarted = producers.started;
}

// In the log, as a step first ran or replayed: a change of the target time
// scale since the step before (a slow motion's start or end, which the game
// sets between steps, so the step after shows it) and the deterministic data
// events it applied, by type (cGame's, such as 222, a slow motion's end, which
// the host sends). A replay that starts or ends a slow motion otherwise than
// the first run did shows here.
void LogStep(const Step& step)
{
    State& s = *sState;
    if (!s.out) return;
    const char* run = s.resimulating ? "replay" : "first run";
    const Step& before = s.steps[(step.frame - 1) % kRing];
    if (before.frame == step.frame - 1 && before.timeScale != step.timeScale)
        Log("step %u (%s): time scale target %.2f -> %.2f\n", step.frame, run, before.timeScale, step.timeScale);
    const int events = std::min(step.inputs.eventCount, MSCHARGED_ROLLBACK_MAX_EVENTS);
    for (int i = 0; i < events; ++i)
        Log("step %u (%s): determ data %u from machine %d\n", step.frame, run, step.inputs.events[i][4],
            step.inputs.eventMachines[i]);
}

void Record(std::uint64_t value, std::uint64_t& total, std::uint64_t& max)
{
    total += value;
    max = std::max(max, value);
}

// The tasks a replayed frame runs as they ran; FindTasks gives the others
// their TaskReplay.
constexpr const char* kRunTasks[] = {"Begin Frame",          "World",     "Game Render",    "Particle Update",
                                     "NIS Transition Wiper", "Front End", "Dispatch Events"};

// The tasks in the task manager's order, and what a replay does with each.
bool FindTasks(FixedUpdateTask* fixed)
{
    State& s = *sState;
    s.tasks.count = 0;
    nlTaskManager* manager = nlTaskManager::m_pInstance;
    if (!manager || !manager->mTaskList) return false;
    nlTask* first = manager->mTaskList;
    nlTask* t = first;
    Log("tasks:");
    do
    {
        if (s.tasks.count == kMaxTasks) return false;
        const char* name = t->GetName();
        TaskReplay replay = kSkip;
        if (t == fixed)
            replay = kFixed;
        else if (name && !std::strcmp(name, "End Frame"))
            replay = kEnd;
        else
            for (const char* run : kRunTasks)
                if (name && !std::strcmp(name, run)) replay = kRun;
        Log(" %s (%s)", name ? name : "?",
                     replay == kSkip ? "left out" : replay == kRun ? "run" : "its replay part");
        s.tasks.list[s.tasks.count] = t;
        s.tasks.replays[s.tasks.count++] = replay;
        t = t->m_next;
    } while (t && t != first);
    Log("\n");
    return true;
}

void RestoreSeeds()
{
    State& s = *sState;
    if (!s.seedsSaved) return;
    s.seedsSaved = false;
    nlDefaultSeed = s.defaultSeed;
    gEffectsRandomSeed = s.effectsSeed;
}
} // namespace

int fixedIndexOf()
{
    const State& s = *sState;
    int index = 0;
    while (index < s.tasks.count - 1 && s.tasks.replays[index] != kFixed) ++index;
    return index;
}

FrameRecord& Frame(std::uint64_t frame)
{
    return sState->frames[frame % kFrameRing];
}

namespace
{
void BeginFrameRecord()
{
    State& s = *sState;
    FrameRecord& record = Frame(s.hostFrame);
    std::fill(std::begin(record.dt), std::end(record.dt), -1.0f);
    std::fill(std::begin(record.real), std::end(record.real), -1.0f);
    mscharged_tweak_save_frame_state(&record.tweak);
}

bool Start(FixedUpdateTask* task, unsigned long taskBytes, unsigned (*checksum)(void))
{
    State& s = *sState;
    if (!FindTasks(task))
    {
        Log("rollback test: cannot list the tasks; stopped\n");
        FlushLog();
        return false;
    }
    FindRanges(task, taskBytes);
    if (!BuildRegions()
        || !mscharged::platform::ConfigureRollbackSnapshots(s.memory.regions, s.memory.regionCount, kMaxDepth + 1,
                                                            static_cast<std::size_t>(kMaxDepth + 1) * 512 + 2048))
    {
        Log("rollback test: cannot track the match; stopped\n");
        FlushLog();
        return false;
    }
    if (s.options.compareEvery) BuildWatched();
    const unsigned frame = static_cast<unsigned>(task->GetFrame());
    mscharged::platform::BeginGameAllocationCheckpoint(Excluded);
    Step& step = s.steps[frame % kRing];
    step.frame = frame;
    step.snapshot = mscharged::platform::BeginRollbackSnapshots();
    step.mark = mscharged::platform::MarkGameAllocationCheckpoint();
    step.checksum = RecordedChecksum(checksum, frame);
    step.seed = GetNetworkRandomSeed();
    step.hostFrame = 0;
    RecordFrame(step);
    for (SentData& sent : s.sent) sent.frame = ~0u; // another match's steps
    s.latest = frame;
    s.trackingFrom = frame;
    s.stored = 1;
    s.haveInputs = false;
    s.receivedCount = 0;
    s.receivedOverflowed = false;
    s.hostFrame = 0;
    s.seedsSaved = false;
    BeginFrameRecord();
    s.active = true;
    ++s.stats.starts;
    Log("tracking from frame %u\n", frame);
    FlushLog();
    return true;
}
} // namespace

void Stop(const char* why)
{
    State& s = *sState;
    if (!s.active) return;
    mscharged::platform::EndRollbackSnapshots();
    mscharged::platform::EndGameAllocationCheckpoint();
    s.active = false;
    s.pendingDepth = 0;
    s.net.undoFrom = 0;
    if (OldestPredicted())
    {
        // Nothing can go back before them any more.
        Log("STOPPED WITH INPUTS STILL PREDICTED: a misprediction now desyncs\n");
        for (unsigned& first : s.net.firstPredicted) first = 0;
    }
    Log("tracking stopped after frame %u: %s\n", s.latest, why);
    FlushLog();
    WriteSummary();
}

namespace
{
// Replays frames `first` to `last` as one, their tasks as the task manager ran
// them, each with the frames' time added up, and the steps in them from `next`
// (the steps with their inputs, a save after each). The first frame's tasks
// before `firstTask` ran before the save the rollback went back to: that frame
// adds nothing to them. Only the last frame has steps, or the first alone
// (from its fixed update: the steps after the target in the target's frame).
// The time scale's transition goes frame by frame: the steps read it, and it
// changes with itself.
void ReplayFrames(std::uint64_t first, std::uint64_t last, int firstTask, unsigned& next,
                  int (*replay)(const MschargedRollbackInputs*), unsigned (*checksum)(void), int depth)
{
    State& s = *sState;
    const FrameRecord& record = Frame(last);
    nlTaskManager* manager = nlTaskManager::m_pInstance;
    for (int i = 0; i < s.tasks.count; ++i)
    {
        if (s.tasks.replays[i] == kSkip) continue;
        float dt = 0, real = 0;
        bool ran = false;
        for (std::uint64_t f = first; f <= last; ++f)
            if (Frame(f).dt[i] >= 0 && (f != first || i >= firstTask))
            {
                dt += Frame(f).dt[i];
                real += Frame(f).real[i];
                ran = true;
            }
        if (!ran) continue;
        manager->mCurrentTimeDelta = dt;
        manager->mRealTimeDelta = real;
        const std::uint64_t taskStart = NowNs();
        switch (s.tasks.replays[i])
        {
        case kRun:
            s.tasks.list[i]->Run(dt);
            break;
        case kEnd:
            RollbackReplayEndFrame();
            break;
        case kFixed:
        {
            for (std::uint64_t f = first; f <= last; ++f)
                if (Frame(f).dt[i] >= 0 && (f != first || firstTask == 0)) // done before the target's frame's save
                    RollbackReplayFixedFrame(Frame(f).dt[i]);
            s.stats.beforeStepsNs += NowNs() - taskStart;
            // The steps as in their frame.
            if (record.dt[i] >= 0)
            {
                manager->mCurrentTimeDelta = record.dt[i];
                manager->mRealTimeDelta = record.real[i];
            }
            for (; next <= s.replayEnd && s.steps[next % kRing].hostFrame <= last; ++next)
            {
                Step& step = s.steps[next % kRing];
                const std::uint64_t stepStart = NowNs();
                const auto& snapshots = mscharged::platform::RollbackSnapshotStatistics();
                const std::uint64_t faultsBefore = snapshots.faults;
                RollbackRedeliverMegaStrikeMeter(step.meter, step.meterCount);
                s.stats.meterRedelivered += step.meterCount;
                if (replay(&step.inputs) < 0) ++s.stats.nisInReplay;
                const std::uint64_t checksumStart = NowNs();
                const unsigned now = RecordedChecksum(checksum, next);
                const std::uint64_t saveStart = NowNs();
                s.stats.checksumNs += saveStart - checksumStart;
                s.stats.stepNs += saveStart - stepStart;
                if (s.options.netplay)
                {
                    // Other inputs, another match: what it is now.
                    step.checksum = now;
                    step.seed = GetNetworkRandomSeed();
                    RecordFrame(step);
                    LogStep(step);
                }
                else if (now != step.checksum)
                {
                    if (!s.stats.mismatches) s.stats.firstMismatchFrame = next;
                    if (s.stats.mismatches++ < kListedMismatches)
                        Log("REPLAYED FRAME %u (%d back from %u): checksum %08x, first run %08x\n",
                                     next, depth, s.latest, now, step.checksum);
                }
                const std::uint64_t pagesBefore = snapshots.savedPages;
                s.stats.replayHotPages += snapshots.hotPages;
                if (next < s.saveFrom)
                {
                    // Its inputs and those before it have all arrived: no
                    // rollback goes back to it, and the next save holds its changes.
                    step.snapshot = 0;
                    step.mark = s.replayMark;
                    ++s.stats.savesSkipped;
                }
                else
                {
                    step.snapshot = mscharged::platform::SaveRollbackSnapshot();
                    const std::uint64_t markStart = NowNs();
                    step.mark = mscharged::platform::MarkGameAllocationCheckpoint();
                    s.stats.markNs += NowNs() - markStart;
                    s.replayMark = step.mark;
                }
                s.stats.saveNs += NowNs() - saveStart;
                s.stats.replayFaults += snapshots.faults - faultsBefore;
                s.stats.replaySavedPages += snapshots.savedPages - pagesBefore;
                ++s.stats.replayed;
                if (s.options.netplay && !(step.phases == s.steps[(next - 1) % kRing].phases)) StopReplayAt(next);
            }
            auto* fixed = static_cast<FixedUpdateTask*>(s.tasks.list[i]);
            fixed->mAccumulatedDeltaT = record.accumulated;
            fixed->mInterpolationDeltaT = record.interpolation;
            break;
        }
        case kSkip:
            break;
        }
        s.stats.taskNs[i] += NowNs() - taskStart;
    }
    ++s.stats.framesReplayed;
    s.stats.framesMerged += last - first;
}

// Why a rollback cannot go back to `target`, or null when it can: a disc
// read in flight or started since (its completion went into memory a restore
// takes back), more frames than are kept, or (in the test) a change of the
// game's or tasks' state, or more events than a step keeps.
const char* CannotRollBack(unsigned target)
{
    State& s = *sState;
    // The end of the last step's frame counts as well.
    const auto producers = mscharged::platform::CurrentGameByteProducers();
    if (producers.pending)
    {
        ++s.stats.skippedReads;
        return "a disc read in flight";
    }
    if (producers.started != s.steps[target % kRing].readsStarted)
    {
        ++s.stats.skippedStarts;
        return "a disc read started";
    }
    if (!s.options.netplay && !(CurrentPhases() == s.steps[s.latest % kRing].phases))
    {
        ++s.stats.skippedPhases;
        return "a state change";
    }
    if (s.receivedOverflowed || s.hostFrame - s.steps[target % kRing].hostFrame >= kFrameRing)
    {
        ++s.stats.skippedEvents;
        return "too many events or frames";
    }
    for (unsigned f = target; f <= s.latest; ++f)
    {
        const Step& step = s.steps[f % kRing];
        if (step.reading)
        {
            ++s.stats.skippedReads;
            return "a disc read in flight";
        }
        if (step.readsStarted != s.steps[target % kRing].readsStarted)
        {
            ++s.stats.skippedStarts;
            return "a disc read started";
        }
        if (!s.options.netplay && !(step.phases == s.steps[s.latest % kRing].phases))
        {
            ++s.stats.skippedPhases;
            return "a state change";
        }
        if (f > target && (step.inputs.overflowed || step.meterOverflowed))
        {
            ++s.stats.skippedEvents;
            return "too many events or frames";
        }
    }
    return nullptr;
}
} // namespace

// At the start of a frame: back to the save after step `target`, then
// forward again through the end of the frame that just ended, running the
// steps through `replayEnd` with the inputs their records hold. False when
// it cannot (CannotRollBack).
bool RollBack(unsigned target, unsigned replayEnd, int (*replay)(const MschargedRollbackInputs*),
              unsigned (*checksum)(void))
{
    State& s = *sState;
    const int depth = static_cast<int>(s.latest - target);
    if (const char* why = CannotRollBack(target))
    {
        if (s.options.netplay)
        {
            ++s.net.impossible;
            Log("ROLLBACK IMPOSSIBLE to frame %u from %u: %s\n", target, s.latest, why);
        }
        return false;
    }
    const std::uint64_t firstFrame = s.steps[target % kRing].hostFrame;

    const bool compare = s.options.compareEvery && s.compare.places && (s.stats.rollbacks + 1) % s.options.compareEvery == 0 && CopyForCompare();
    const std::uint64_t start = NowNs();
    const Step& from = s.steps[target % kRing];
    if (!mscharged::platform::RestoreRollbackSnapshot(from.snapshot))
    {
        ++s.stats.failedRestores;
        Stop("a save to roll back to was gone");
        return false;
    }
    s.stats.lossyFrees += mscharged::platform::RollbackGameAllocationCheckpointTo(from.mark, Excluded);
    s.replayMark = from.mark;
    Record(NowNs() - start, s.stats.restoreNs, s.stats.maxRestoreNs);

    // The frames from the target step's on, the first from its fixed update,
    // through the end of the frame that just ended. The frame rate option's
    // state is put back for each frame's start, and after the target's start
    // for the first. Frames without steps go as one with the next frame (up to
    // the task manager's longest frame): above the original's frame rate, a
    // step spans several. So does the rest of the target's frame, its tasks
    // after the fixed update, unless more steps ran in it after the target.
    MschargedTweakFrameState now{};
    mscharged_tweak_save_frame_state(&now);
    auto* fixed = static_cast<FixedUpdateTask*>(s.tasks.list[fixedIndexOf()]);
    const float accumulated = fixed->mAccumulatedDeltaT, interpolation = fixed->mInterpolationDeltaT;
    // In a network match, the steps whose inputs were applied (some after the
    // last saved, when a step's presentation stopped the hook).
    const unsigned ran = s.options.netplay ? std::max(s.net.lastInputFrame, s.latest) : s.latest;
    s.replayEnd = replayEnd;
    s.resimulating = true;
    unsigned next = target + 1;
    const int fixedIndex = fixedIndexOf();
    const auto frameTime = [&](std::uint64_t frame) { return std::max(Frame(frame).dt[fixedIndex], 0.0f); };
    const bool laterSteps = next <= s.replayEnd && s.steps[next % kRing].hostFrame <= firstFrame;
    std::uint64_t rangeFrom = firstFrame + 1;
    int fromTask = 0;
    float time = 0;
    if (s.options.merge && !laterSteps && firstFrame < s.hostFrame
        && frameTime(firstFrame) + frameTime(firstFrame + 1) <= kLongestFrame)
    {
        rangeFrom = firstFrame;
        fromTask = fixedIndex + 1;
        time = frameTime(firstFrame);
    }
    else
    {
        mscharged_tweak_load_frame_state(firstFrame + 1 <= s.hostFrame ? &Frame(firstFrame + 1).tweak : &now);
        ReplayFrames(firstFrame, firstFrame, fixedIndex, next, replay, checksum, depth);
    }
    for (std::uint64_t frame = firstFrame + 1; frame <= s.hostFrame; ++frame)
    {
        time += frameTime(frame);
        const bool steps = next <= s.replayEnd && s.steps[next % kRing].hostFrame <= frame;
        if (s.options.merge && !steps && frame < s.hostFrame && time + frameTime(frame + 1) <= kLongestFrame)
            continue;
        mscharged_tweak_load_frame_state(&Frame(std::max(rangeFrom, firstFrame + 1)).tweak);
        ReplayFrames(rangeFrom, frame, fromTask, next, replay, checksum, depth);
        rangeFrom = frame + 1;
        fromTask = 0;
        time = 0;
    }
    mscharged_tweak_load_frame_state(&now);
    fixed->mAccumulatedDeltaT = accumulated;
    fixed->mInterpolationDeltaT = interpolation;
    // And what the network gave the match after the last step.
    RollbackRedeliverMegaStrikeMeter(s.received, s.receivedCount);
    s.stats.meterRedelivered += s.receivedCount;
    s.resimulating = false;
    // The audio transitions as the replay left the match wanting them: those a
    // replayed step started or ended otherwise than its first run.
    mscharged_rollback_audio_reconcile();
    s.saveFrom = 0;
    if (next != s.replayEnd + 1) Log("REPLAY MISSED STEPS %u to %u\n", next, s.replayEnd);
    if (s.options.netplay && s.replayEnd < ran)
    {
        // Steps undone: they run again when their inputs have all arrived (the
        // inputs that did arrive are kept), and their time goes back to the
        // fixed update, to catch up.
        s.latest = s.replayEnd;
        s.net.lastInputFrame = s.replayEnd;
        s.net.waitThrough = std::max(s.net.waitThrough, ran);
        for (unsigned& first : s.net.firstPredicted)
            if (first > s.replayEnd) first = 0;
        fixed->mAccumulatedDeltaT += static_cast<float>(ran - s.replayEnd) * g_fFixedUpdateTick;
    }
    ++s.stats.rollbacks;
    ++s.stats.byDepth[std::min(depth, kMaxDepth)];
    Record(NowNs() - start, s.stats.rollbackNs, s.stats.maxRollbackNs);
    if (compare) CompareAfterReplay(s.latest, depth);
    return true;
}

namespace
{
void Watchdog()
{
    for (;;)
    {
        mscharged::platform::RollbackSleepMs(50);
        std::lock_guard lock(sMutex);
        if (sState->stopped) return;
        if (sState->active && NowNs() - sState->lastHookNs > kIdleNs && !OldestPredicted())
            Stop("no step for 250 ms");
    }
}

#if !defined(_WIN32)
// The game module's own code that throws, logged while the test runs: the
// runtime only reports what() when the game stops. (Not on Windows, where the
// module links the C++ runtime's __cxa_throw itself.)
constexpr int kListedThrows = 8;
int sThrows;

void LogThrow(const std::type_info* type)
{
    State& s = *sState;
    void* frames[24];
    const int depth = mscharged::platform::RollbackCallStack(frames, 24);
    const std::uintptr_t self = mscharged::platform::RollbackModuleBase(reinterpret_cast<void*>(&LogThrow));
    Log("THROW %s at frame %u, call stack in the game module:", type ? type->name() : "?",
                 s.latest);
    for (int i = 2; i < depth; ++i)
        if (mscharged::platform::RollbackModuleBase(frames[i]) == self)
            Log(" +0x%lx",
                         static_cast<unsigned long>(reinterpret_cast<std::uintptr_t>(frames[i]) - self - 1));
    Log("\n");
    FlushLog();
}
#endif

struct Install
{
    Install()
    {
        // <file>, or 1: on without a log (recharged.ini's [online] rollback and rollback_test).
        const char* path = std::getenv("RECHARGED_ROLLBACK_TEST");
        const char* network = std::getenv("RECHARGED_ROLLBACK");
        const bool netplay = network && *network;
        if (netplay && path && *path)
        {
            std::fprintf(stderr, "RECHARGED_ROLLBACK: not with RECHARGED_ROLLBACK_TEST\n");
            return;
        }
        if (netplay) path = network;
        if (!path || !*path) return;
        auto* state = static_cast<State*>(std::calloc(1, sizeof(State)));
        if (!state) return;
        if (std::strcmp(path, "1") != 0)
        {
            const std::size_t length = std::strlen(path);
            state->summaryPath = static_cast<char*>(std::malloc(length + sizeof(".summary")));
            state->out = std::fopen(path, "w");
            if (!state->summaryPath || !state->out)
            {
                std::fprintf(stderr, "RECHARGED_ROLLBACK_TEST: cannot prepare %s\n", path);
                return;
            }
            std::memcpy(state->summaryPath, path, length);
            std::memcpy(state->summaryPath + length, ".summary", sizeof(".summary"));
        }
        state->options.maxDepth = EnvInt("RECHARGED_ROLLBACK_TEST_FRAMES", kMaxDepth, 1, kMaxDepth);
        state->options.interval = EnvInt("RECHARGED_ROLLBACK_TEST_INTERVAL", 1, 1, 1 << 20);
        state->options.compareEvery = EnvInt("RECHARGED_ROLLBACK_TEST_COMPARE", 0, 0, 1 << 20);
        state->options.merge = EnvInt("RECHARGED_ROLLBACK_TEST_MERGE", 1, 0, 1) != 0;
        state->options.netplay = netplay;
        if (EnvInt("RECHARGED_ROLLBACK_AUDIO_AUDIT", 0, 0, 1)) StartAudioAudit();
        state->options.inputDelay = kStartingDelay;
        state->options.fakeDesyncFrom = static_cast<unsigned>(EnvInt("RECHARGED_ROLLBACK_TEST_DESYNC", 0, 0, 1 << 30));
        state->options.poses = EnvInt("RECHARGED_ROLLBACK_POSES", 0, 0, 1) != 0;
        state->options.bodies = EnvInt("RECHARGED_ROLLBACK_BODIES", 0, 0, 1) != 0;
        state->options.contacts = EnvInt("RECHARGED_ROLLBACK_CONTACTS", 0, 0, 1) != 0;
        if (netplay) state->options.compareEvery = 0;
        if (state->options.compareEvery)
        {
            state->compare.moduleBase =
                mscharged::platform::RollbackModuleBase(reinterpret_cast<void*>(&mscharged_rollback_active));
            state->compare.places = static_cast<Place*>(std::calloc(kMaxPlaces, sizeof(Place)));
        }
        sState = state;
        if (netplay)
            Log("rollback: network matches predict, input delay %d steps; summary in %s%s%s%s\n",
                state->options.inputDelay, state->summaryPath,
                state->options.poses ? "; the players' poses join the checksum (RECHARGED_ROLLBACK_POSES)" : "",
                state->options.bodies ? "; the physics bodies join the checksum (RECHARGED_ROLLBACK_BODIES)" : "",
                state->options.contacts ? "; the collisions join the checksum (RECHARGED_ROLLBACK_CONTACTS)" : "");
        else
            Log("rollback test: up to %d steps back every %d steps; summary in %s\n", state->options.maxDepth,
                state->options.interval, state->summaryPath);
        FlushLog();
        std::fprintf(stderr, "rollback: on (%s), input delay %d steps, %s\n",
                     netplay ? "network matches predict" : "test", state->options.inputDelay,
                     state->out ? path : "no log");
        mscharged::platform::RollbackStartThread(Watchdog);
    }
    ~Install()
    {
        if (!sState) return;
        std::lock_guard lock(sMutex);
        if (!sState->stopped)
        {
            Stop("exit");
            WriteSummary();
            sState->stopped = true;
        }
        if (sState->out) std::fclose(sState->out);
        sState->out = nullptr;
    }
} sInstall;
} // namespace
} // namespace mscharged::rollback

using namespace mscharged::rollback;

// On from startup to the end, even once the rollback has stopped: what the
// hooks do with it (deterministic data outside the arenas) must not change.
extern "C" bool mscharged_rollback_active(void)
{
    return sState;
}

extern "C" bool mscharged_rollback_resimulating(void)
{
    return sState && sState->resimulating;
}

// Only the main thread queues deterministic data, from the steps and the
// replays (which run under the hooks' lock already): no lock of its own.
extern "C" bool mscharged_rollback_determ_data_new(int frame, const void* data, unsigned size)
{
    if (!Running() || !sState->active || frame < 0) return true;
    SentData& sent = sState->sent[static_cast<unsigned>(frame) % kRing];
    if (sent.frame != static_cast<unsigned>(frame))
    {
        sent.frame = static_cast<unsigned>(frame);
        sent.count = 0;
    }
    size = std::min<unsigned>(size, sizeof(sent.data[0]));
    for (int i = 0; i < sent.count; ++i)
        if (sent.sizes[i] == size && !std::memcmp(sent.data[i], data, size)) return false;
    if (sent.count < kMaxSent)
    {
        sent.sizes[sent.count] = static_cast<unsigned char>(size);
        std::memcpy(sent.data[sent.count++], data, size);
    }
    return true;
}

extern "C" void mscharged_rollback_inputs_applied(const MschargedRollbackInputs* inputs)
{
    if (!Running() || sState->resimulating) return;
    std::lock_guard lock(sMutex);
    sState->pending = *inputs;
    sState->haveInputs = true;
}

extern "C" void mscharged_rollback_task_time(const void* task, float dt)
{
    if (!Running() || sState->resimulating) return;
    std::lock_guard lock(sMutex);
    State& s = *sState;
    if (!s.active) return;
    RestoreSeeds();
    FrameRecord& record = Frame(s.hostFrame);
    for (int i = 0; i < s.tasks.count; ++i)
        if (task == s.tasks.list[i])
        {
            record.dt[i] = dt;
            record.real[i] = nlTaskManager::m_pInstance->mRealTimeDelta;
            if (s.tasks.replays[i] == kSkip)
            {
                s.seedsSaved = true;
                s.defaultSeed = nlDefaultSeed;
                s.effectsSeed = gEffectsRandomSeed;
            }
            return;
        }
}

extern "C" void mscharged_rollback_meter_received(const bool* values, int count)
{
    if (!Running()) return;
    std::lock_guard lock(sMutex);
    State& s = *sState;
    s.stats.meterReceived += count;
    for (int i = 0; i < count; ++i)
    {
        if (s.receivedCount == kMaxMeter)
        {
            s.receivedOverflowed = true;
            return;
        }
        s.received[s.receivedCount++] = values[i];
    }
}

extern "C" void mscharged_rollback_between_steps(int (*replay)(const MschargedRollbackInputs* inputs),
                                                 unsigned (*checksum)(void), void* task, unsigned long taskBytes)
{
    if (!Running()) return;
    if (sState->options.netplay && !mscharged_rollback_predicting()) return; // offline: nothing to predict
    std::lock_guard lock(sMutex);
    State& s = *sState;
    s.lastHookNs = NowNs();
    auto* fixed = static_cast<FixedUpdateTask*>(task);
    const unsigned frame = static_cast<unsigned>(fixed->GetFrame());
    if (s.active && (frame != s.latest + 1 || !s.haveInputs)) Stop("a step the rollback did not see");
    if (!s.active)
    {
        if (!Start(fixed, taskBytes, checksum))
            s.stopped = true;
        else if (s.options.netplay)
        {
            s.replay = replay;
            s.checksum = checksum;
            UpdateConfirmed();
        }
        return;
    }

    ++s.stats.stepCount;
    if (s.stats.stepCount % kUntrackedEvery == 0)
    {
        std::uintptr_t first = 0;
        s.stats.untracked += mscharged::platform::FindUntrackedRollbackWrites(&first);
    }
    Step& step = s.steps[frame % kRing];
    step.frame = frame;
    step.snapshot = mscharged::platform::SaveRollbackSnapshot();
    step.mark = mscharged::platform::MarkGameAllocationCheckpoint();
    step.inputs = s.pending;
    std::copy_n(s.received, s.receivedCount, step.meter);
    step.meterCount = s.receivedCount;
    step.meterOverflowed = s.receivedOverflowed;
    s.receivedCount = 0;
    s.receivedOverflowed = false;
    step.hostFrame = s.hostFrame;
    step.checksum = RecordedChecksum(checksum, frame);
    step.seed = GetNetworkRandomSeed();
    RecordFrame(step);
    LogStep(step);
    // A step that predicted must leave the match's phase alone: undone at the
    // frame start, it runs again when its inputs have all arrived.
    if (s.options.netplay && AnyPredicted(InputsFor(frame)) && !s.net.undoFrom
        && !(step.phases == s.steps[(frame - 1) % kRing].phases))
        s.net.undoFrom = frame;
    s.haveInputs = false;
    s.latest = frame;
    ++s.stored;
    // No rollback returns past the oldest step kept: the registry forgets the journal before it.
    const unsigned oldest = frame - static_cast<unsigned>(std::min(s.stored, kRing) - 1);
    mscharged::platform::TrimGameAllocationCheckpoint(s.steps[oldest % kRing].mark);

    // The rollback waits for the start of the next frame: here the frame has
    // set up its cameras and views already, which a restore would take back.
    s.replay = replay;
    s.checksum = checksum;
    s.pendingDepth = 0;
    if (s.options.netplay)
    {
        UpdateConfirmed();
        if (s.stats.stepCount % kSummaryEvery == 0) WriteSummary();
        return;
    }
    if (s.stats.stepCount % s.options.interval == 0)
    {
        const int depth = s.nextDepth % s.options.maxDepth + 1;
        if (depth < s.stored)
        {
            ++s.nextDepth;
            s.pendingDepth = depth;
        }
        else
            ++s.stats.skippedShort;
    }
    if (s.stats.stepCount % kSummaryEvery == 0) WriteSummary();
}

extern "C" void mscharged_rollback_frame_start(void)
{
    if (!Running()) return;
    std::lock_guard lock(sMutex);
    State& s = *sState;
    if (!s.active) return;
    RestoreSeeds(); // after the frame's last task
    // The frame that ended, then the rollback it asked for, then the frame
    // that starts now.
    const auto* fixed = static_cast<const FixedUpdateTask*>(s.tasks.list[fixedIndexOf()]);
    Frame(s.hostFrame).accumulated = fixed->mAccumulatedDeltaT;
    Frame(s.hostFrame).interpolation = fixed->mInterpolationDeltaT;
    if (s.options.netplay)
        NetplayFrameStart();
    else if (s.pendingDepth)
    {
        const int depth = s.pendingDepth;
        s.pendingDepth = 0;
        RollBack(s.latest - static_cast<unsigned>(depth), s.latest, s.replay, s.checksum);
    }
    ++s.hostFrame;
    BeginFrameRecord();
}

#if !defined(_WIN32)
// Calls to __cxa_throw from the game module bind to this one (hidden), which
// logs the throw and goes on to the C++ runtime's. It has a name of its own
// and takes __cxa_throw's symbol: GCC 15 declares __cxa_throw itself, with
// void* for the type, and refuses another declaration.
#define MSCHARGED_STRING(text) #text
#define MSCHARGED_SYMBOL(prefix, name) MSCHARGED_STRING(prefix) name
extern "C" __attribute__((visibility("hidden"), noreturn)) void LoggedThrow(void* thrown, std::type_info* type,
    void (*destroy)(void*)) __asm__(MSCHARGED_SYMBOL(__USER_LABEL_PREFIX__, "__cxa_throw"));
extern "C" void LoggedThrow(void* thrown, std::type_info* type, void (*destroy)(void*))
{
    if (Running() && sState->active && sThrows++ < kListedThrows) LogThrow(type);
    using Throw = void (*)(void*, std::type_info*, void (*)(void*));
    static const Throw next = reinterpret_cast<Throw>(dlsym(RTLD_DEFAULT, "__cxa_throw"));
    next(thrown, type, destroy);
    __builtin_unreachable();
}
#endif
