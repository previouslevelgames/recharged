// RECHARGED_ROLLBACK: network matches that predict. The network router's
// queues hold each machine's input messages in the order it sent them, one
// per step after the input delay; a step whose message has not arrived runs
// with that machine's last inputs that did, and when its message arrives (at
// the start of a frame) and says otherwise, the match goes back before the
// first step it got wrong and runs again with what arrived.
// RECHARGED_ROLLBACK_TEST_DESYNC=<n> (a test aid) has this machine's
// checksums say otherwise from step n of each match: every machine shows
// the game's sync error.
#include "platform/rollback_state.h"

#include <cmath>

namespace mscharged::rollback
{
namespace
{
// The input delay (ChooseDelay), in steps of how far ahead of need the others
// get this machine's inputs, as they report it.
constexpr float kLengthenBelow = -2.5f;  // predicted for longer than two steps and a half: a step more
constexpr float kShortenAbove = 0.25f;   // early by more than a quarter of a step: a step less
constexpr unsigned kDelayEvery = 150;    // steps between changes of the input delay
// The time sync (SyncTime), in steps.
constexpr float kAheadSmoothing = 0.05f; // weight of each new measure of how far ahead inputs come
constexpr float kSlowAbove = 0.75f;      // ahead of the others by more than this: slow down
constexpr float kSlowUntil = 0.25f;      // until within this
constexpr float kSlowdown = 0.1f;        // of each frame's time taken from the fixed steps while slowing
} // namespace

StepInputs& InputsFor(unsigned frame)
{
    return sState->net.inputs[frame % kInputRing];
}

bool AnyPredicted(const StepInputs& inputs)
{
    for (int machine = 0; machine < inputs.machineCount; ++machine)
        if (inputs.predicted[machine]) return true;
    return false;
}

// RECHARGED_ROLLBACK: a replayed step changed the match's phase (the inputs
// that arrived made a goal, say): the replay ends with it, and the steps after
// it run again when their inputs have all arrived. A step with inputs still
// predicted does not change the phase either, and is undone as well, unless a
// disc read was in flight at the save before it or started since: a restore
// would take back the read's reservation (its completion then fails) or what
// it wrote, so the step is kept, and a misprediction in it can no longer be
// corrected.
void StopReplayAt(unsigned frame)
{
    State& s = *sState;
    if (!AnyPredicted(InputsFor(frame)))
    {
        s.replayEnd = frame;
        ++s.net.truncated;
        return;
    }
    const Step& before = s.steps[(frame - 1) % kRing];
    const Step& step = s.steps[frame % kRing];
    if (before.reading || step.readsStarted != before.readsStarted)
    {
        s.replayEnd = frame;
        ++s.net.truncated;
        Log("KEPT PREDICTED FRAME %u: a disc read %s; a misprediction now desyncs\n",
                frame, before.reading ? "in flight" : "started");
        return;
    }
    if (!mscharged::platform::RestoreRollbackSnapshot(before.snapshot))
    {
        ++s.stats.failedRestores;
        Log("ROLLBACK IMPOSSIBLE: the save after frame %u was gone\n", frame - 1);
        s.replayEnd = frame;
        return;
    }
    s.stats.lossyFrees += mscharged::platform::RollbackGameAllocationCheckpointTo(before.mark, Excluded);
    s.replayEnd = frame - 1;
    ++s.net.undone;
}

namespace
{
NetworkInputRouter* Router()
{
    return gNetworkInputRouter;
}

int MachineCount()
{
    return std::min<int>(g_pNetworkSessionBase->GetNumMachines(), kMachines);
}

// The fixed update's frame: the last step that ran.
unsigned CurrentStep()
{
    return static_cast<unsigned>(GetFixedUpdateTask()->GetFrame());
}

// A machine's inputs as the router holds them (before any arrived, and while
// the router repeats them for the input delay).
MachineInput FromRouter(int machine)
{
    NetworkInputRouter* router = Router();
    MachineInput input{};
    input.players = static_cast<unsigned char>(
        std::min<int>(g_pNetworkSessionBase->GetPeer(static_cast<s8>(machine))->mPlayerCount, 4));
    input.remap = router->mRemapAngles[machine];
    for (int player = 0; player < input.players; ++player)
    {
        const s8 id = GetNetworkPlayerId(static_cast<s8>(player), static_cast<s8>(machine));
        std::memcpy(input.records[player], &router->mInputRecords[id], sizeof(PackedDetInput));
        input.states[player] = router->mInputStates[id];
    }
    return input;
}

// A machine's inputs from its message: what changed since `previous`, its
// previous message's.
MachineInput FromMessage(NetMessageInput& message, const MachineInput& previous)
{
    MachineInput input = previous;
    message.GetNetworkInputMessageRemapAngle(&input.remap);
    for (int player = 0; player < input.players; ++player)
    {
        PackedDetInput record;
        std::memcpy(&record, input.records[player], sizeof(record));
        message.ApplyNetworkInputMessageRecord(static_cast<s8>(player), &record);
        std::memcpy(input.records[player], &record, sizeof(record));
        input.states[player] = message.GetNetworkInputMessagePlayerState(static_cast<s8>(player));
    }
    input.eventCount = std::min<int>(message.mEventCount, kMachineEvents);
    for (int event = 0; event < input.eventCount; ++event)
        std::memcpy(input.events[event], message.GetNetworkInputMessageEvent(event), sizeof(input.events[event]));
    return input;
}

MachineInput Predicted(int machine)
{
    MachineInput input = sState->net.lastReal[machine];
    input.eventCount = 0;
    return input;
}

bool Same(const MachineInput& a, const MachineInput& b)
{
    if (a.players != b.players || a.remap != b.remap || a.eventCount != b.eventCount) return false;
    for (int player = 0; player < a.players; ++player)
        if (std::memcmp(a.records[player], b.records[player], sizeof(a.records[player]))
            || a.states[player] != b.states[player])
            return false;
    return !std::memcmp(a.events, b.events, sizeof(a.events[0]) * static_cast<std::size_t>(a.eventCount));
}

// What a misprediction got wrong, one bit per InputPart: the summary counts
// them, to tell a pad's noise (accelerometers, pointer) from real presses.
unsigned MispredictedParts(const MachineInput& a, const MachineInput& b)
{
    constexpr struct
    {
        InputPart part;
        std::size_t from, to; // bytes of PackedDetInput
    } kFields[] = {{kButtons, 0x0, 0x2},     {kTriggers, 0x2, 0x3},       {kRemoteAccel, 0x3, 0x6},
                   {kNunchukAccel, 0x6, 0x9}, {kPointerTargets, 0x9, 0xA}, {kPointer, 0xA, 0xC},
                   {kSticks, 0xC, 0x10}};
    unsigned parts = 0;
    if (a.players != b.players || a.remap != b.remap) parts |= 1u << kOtherInput;
    for (int player = 0; player < std::min(a.players, b.players); ++player)
    {
        for (const auto& field : kFields)
            if (std::memcmp(a.records[player] + field.from, b.records[player] + field.from, field.to - field.from))
                parts |= 1u << field.part;
        if (a.states[player] != b.states[player]) parts |= 1u << kPlayerState;
    }
    if (a.eventCount != b.eventCount
        || std::memcmp(a.events, b.events,
                       sizeof(a.events[0]) * static_cast<std::size_t>(std::min(a.eventCount, b.eventCount))))
        parts |= 1u << kEvents;
    return parts;
}

// What the router applies, in its order: machine by machine, its events, then
// its players' inputs.
void Compose(const StepInputs& step, MschargedRollbackInputs& out)
{
    out = {};
    out.machineCount = step.machineCount;
    for (int machine = 0; machine < step.machineCount; ++machine)
    {
        const MachineInput& input = step.machines[machine];
        out.machines[machine] = static_cast<signed char>(machine);
        out.players[machine] = input.players;
        out.remapAngles[machine] = input.remap;
        for (int player = 0; player < input.players; ++player)
        {
            const s8 id = GetNetworkPlayerId(static_cast<s8>(player), static_cast<s8>(machine));
            std::memcpy(out.records[id], input.records[player], sizeof(out.records[id]));
            out.states[id] = input.states[player];
        }
        for (int event = 0; event < input.eventCount; ++event)
        {
            if (out.eventCount == MSCHARGED_ROLLBACK_MAX_EVENTS)
            {
                out.overflowed = 1;
                break;
            }
            out.eventMachines[out.eventCount] = static_cast<signed char>(machine);
            std::memcpy(out.events[out.eventCount++], input.events[event], sizeof(out.events[0]));
        }
    }
}

// The router's records follow the inputs that arrived: the next message
// changes them, and the original router reads them.
void Mirror(int machine, const MachineInput& input)
{
    NetworkInputRouter* router = Router();
    router->mRemapAngles[machine] = input.remap;
    for (int player = 0; player < input.players; ++player)
    {
        const s8 id = GetNetworkPlayerId(static_cast<s8>(player), static_cast<s8>(machine));
        std::memcpy(&router->mInputRecords[id], input.records[player], sizeof(PackedDetInput));
        router->mInputStates[id] = input.states[player];
    }
}

// The checksum another machine sends: that of the last step whose inputs had
// all arrived there, compared when ours are in too.
void NoteSync(int machine, const NetMessageInput& message)
{
    State& s = *sState;
    if (machine == g_pNetworkSessionBase->GetLocalMachineId()) return;
    if ((message.mFlags & 6) == 2)
    {
        // The input delay in the top byte, the measure in 256ths below it.
        const std::int32_t measure = static_cast<std::int32_t>(message.mFrame << 8) >> 8;
        s.net.reportedAhead[machine] = static_cast<float>(measure) / 256.0f;
        s.net.reportedDelay[machine] = static_cast<int>(message.mFrame >> 24);
        s.net.haveReported[machine] = true;
        return;
    }
    if (!(message.mFlags & 4)) return;
    if (s.net.remoteCount == kRemoteChecks)
    {
        std::memmove(s.net.remote, s.net.remote + 1, sizeof(s.net.remote[0]) * (kRemoteChecks - 1));
        --s.net.remoteCount;
    }
    s.net.remote[s.net.remoteCount++] = {message.mFrame, message.mChecksum, message.mRandomSeed, machine};
}

MachineInput Arrived(int machine, NetMessageInput& message)
{
    State& s = *sState;
    NoteSync(machine, message);
    MachineInput input = FromMessage(message, s.net.lastReal[machine]);
    s.net.lastReal[machine] = input;
    Mirror(machine, input);
    return input;
}

// The phases a step may predict in: the match being played, nothing paused.
bool PredictablePhases(const Phases& phases)
{
    return (phases.game == GS_KICKOFF || phases.game == GS_GAMEPLAY || phases.game == GS_OVERTIME)
        && phases.current == TASK_GAMEPLAY && phases.pending == TASK_GAMEPLAY
        && !phases.megaStrike && !g_pNetworkSession->GetPausedMachineMask();
}
} // namespace

unsigned OldestPredicted()
{
    const State& s = *sState;
    unsigned oldest = 0;
    for (const unsigned first : s.net.firstPredicted)
        if (first && (!oldest || first < oldest)) oldest = first;
    return oldest;
}

namespace
{
// Whether step `frame` may run with predicted inputs: the rollback follows the
// match and could go back before the oldest step predicted, through as many
// frames as it keeps and without a disc read in between.
bool CanPredict(unsigned frame)
{
    State& s = *sState;
    if (!s.active || s.latest + 1 != frame || frame <= s.net.waitThrough) return false;
    // Tracking starts as play does, after a goal, a NIS or a pause: the disc
    // reads the game starts then (a rollback cannot cross them) are let pass.
    if (frame <= s.trackingFrom + kSettleSteps) return false;
    const Phases phases = CurrentPhases();
    if (!PredictablePhases(phases) || !(phases == s.steps[s.latest % kRing].phases)) return false;
    const unsigned oldest = OldestPredicted();
    const unsigned target = oldest ? oldest - 1 : s.latest;
    if (frame - target > static_cast<unsigned>(kMaxDepth)) return false;
    const Step& from = s.steps[target % kRing];
    if (from.frame != target || s.hostFrame - from.hostFrame >= kFrameRing - 2) return false;
    const auto producers = mscharged::platform::CurrentGameByteProducers();
    return !producers.pending && producers.started == from.readsStarted;
}
} // namespace

// The steps whose inputs have all arrived: their checksums are the match's,
// sent to the other machines and compared with theirs.
void UpdateConfirmed()
{
    State& s = *sState;
    if (!s.active) return;
    unsigned through = std::min(s.net.lastInputFrame, s.latest);
    if (const unsigned oldest = OldestPredicted()) through = std::min(through, oldest - 1);
    const unsigned from = s.net.haveConfirmed && s.net.confirmed < through ? s.net.confirmed + 1 : through;
    for (unsigned f = from; f <= through && f + kRing > s.latest; ++f)
    {
        const Step& step = s.steps[f % kRing];
        if (step.frame == f)
            s.net.history[f % kConfirmedRing] = {f, step.checksum ^ (s.options.fakeDesyncFrom && f >= s.options.fakeDesyncFrom), step.seed, -1};
    }
    if (!s.net.haveConfirmed || through > s.net.confirmed)
    {
        s.net.confirmed = through;
        s.net.haveConfirmed = true;
    }
    int kept = 0;
    for (int i = 0; i < s.net.remoteCount; ++i)
    {
        const SyncPoint& remote = s.net.remote[i];
        if (remote.frame > s.net.confirmed && remote.frame < s.net.confirmed + kConfirmedRing)
        {
            s.net.remote[kept++] = remote; // ours is not in yet
            continue;
        }
        const SyncPoint& ours = s.net.history[remote.frame % kConfirmedRing];
        if (ours.frame != remote.frame) continue; // not kept (or a step the hook did not see)
        ++s.net.remoteChecks;
        if (ours.checksum != remote.checksum || ours.seed != remote.seed)
        {
            if (!s.net.desyncs) s.net.firstDesyncFrame = remote.frame;
            if (!s.net.matchDesync)
            {
                s.net.matchDesync = true;
                s.net.matchDesyncFrame = remote.frame;
                s.net.seedDesync = ours.checksum == remote.checksum;
            }
            if (s.net.desyncs++ < kListedMismatches)
                Log("DESYNC at frame %u: checksum %08x seed %08x here, %08x %08x on machine %d\n",
                             remote.frame, ours.checksum, ours.seed, remote.checksum, remote.seed, remote.machine);
            FlushLog();
        }
    }
    s.net.remoteCount = kept;
}

namespace
{
// At the start of a frame: the messages that arrived for the steps predicted.
// Each one replaces its prediction, the steps still predicted are predicted
// again from the last that arrived, and when any changed the match goes back
// before the first.
void Resolve()
{
    State& s = *sState;
    NetworkInputRouter* router = Router();
    unsigned earliest = 0;
    const auto changed = [&](unsigned frame) { earliest = earliest ? std::min(earliest, frame) : frame; };
    for (int machine = 0; machine < MachineCount(); ++machine)
    {
        unsigned frame = s.net.firstPredicted[machine];
        if (!frame) continue;
        NetworkInputMessageQueue& queue = router->m_InputQueue[machine];
        for (; frame <= s.net.lastInputFrame && queue.GetCount() > 0; ++frame)
        {
            StepInputs& step = InputsFor(frame);
            const MachineInput input = Arrived(machine, queue.Pop());
            if (!Same(step.machines[machine], input))
            {
                changed(frame);
                ++s.net.mispredictions[machine];
                const unsigned parts = MispredictedParts(step.machines[machine], input);
                for (int part = 0; part < kInputParts; ++part)
                    if (parts & (1u << part)) ++s.net.mispredictedParts[part];
            }
            step.machines[machine] = input;
            step.predicted[machine] = false;
        }
        for (unsigned later = frame; later <= s.net.lastInputFrame; ++later)
        {
            const MachineInput input = Predicted(machine);
            if (!Same(InputsFor(later).machines[machine], input)) changed(later);
            InputsFor(later).machines[machine] = input;
        }
        s.net.firstPredicted[machine] = frame <= s.net.lastInputFrame ? frame : 0;
    }
    if (!earliest || earliest > s.latest) return;
    for (unsigned f = earliest; f <= s.latest; ++f) Compose(InputsFor(f), s.steps[f % kRing].inputs);
    // A later rollback goes back at most to the last step whose inputs, and
    // those of every step before, have all arrived.
    const unsigned oldest = OldestPredicted();
    s.saveFrom = oldest ? oldest - 1 : s.net.lastInputFrame;
    ++s.net.correctionRollbacks;
    RollBack(earliest - 1, s.latest, s.replay, s.checksum);
}

// How far ahead of need this machine's inputs reach the machine they reach
// the latest, as the others report it (negative: predicted for that many
// steps); false while none has reported yet.
bool WorstReportedAhead(float& worst)
{
    const State& s = *sState;
    bool any = false;
    worst = 1e9f;
    for (int machine = 0; machine < MachineCount(); ++machine)
        if (machine != g_pNetworkSessionBase->GetLocalMachineId() && s.net.haveReported[machine])
        {
            worst = std::min(worst, s.net.reportedAhead[machine]);
            any = true;
        }
    return any;
}

// The input delay: how far ahead of need the others get this machine's
// inputs (as they report it) tells whether they predict them for long. More
// than two steps and a half: a step more of delay; more than a quarter of a
// step early: a step less, down to the starting delay (a step's change moves
// the measure by a step: it cannot cross back). So the others predict this
// machine's inputs for one or two steps, which a rollback replays within a
// 60 FPS frame. A change at most every three seconds, which the measures need
// to follow it.
void ChooseDelay()
{
    State& s = *sState;
    if (s.net.delayChange || CurrentStep() < s.net.delayChangedAt + kDelayEvery) return;
    float worst;
    if (!WorstReportedAhead(worst)) return;
    if (worst < kLengthenBelow && s.net.delay < kMaxInputDelay)
        s.net.delayChange = 1;
    else if (worst > kShortenAbove && s.net.delay > s.options.inputDelay)
        s.net.delayChange = -1;
}

// The time sync. A machine that runs ahead predicts the others' inputs for
// longer, and rolls back further, than one behind it, which gets their inputs
// early. Each machine measures how far ahead of it each other machine's
// inputs come (messages waiting less steps predicted: that machine's input
// delay less the latency less how far this one runs ahead), and sends what
// it measures with its own delay; half the difference between the two
// measures, the delays taken out, is how far it runs ahead, the latency
// cancelled. The one ahead by more than three quarters of a step slows its
// steps by a tenth until within a quarter.
void SyncTime()
{
    State& s = *sState;
    NetworkInputRouter* router = Router();
    const int local = g_pNetworkSessionBase->GetLocalMachineId();
    s.net.lead = 0;
    for (int machine = 0; machine < MachineCount(); ++machine)
    {
        if (machine == local) continue;
        const unsigned first = s.net.firstPredicted[machine];
        const float ahead = first ? -static_cast<float>(s.net.lastInputFrame - first + 1)
                                  : static_cast<float>(router->m_InputQueue[machine].GetCount());
        s.net.queuedAhead[machine] += (ahead - s.net.queuedAhead[machine]) * kAheadSmoothing;
        if (s.net.haveReported[machine])
            s.net.lead = std::max(s.net.lead, (s.net.reportedAhead[machine] - s.net.queuedAhead[machine]
                                       - static_cast<float>(s.net.delay - s.net.reportedDelay[machine])) / 2);
    }
    bool isCurrentPredictable = PredictablePhases(CurrentPhases());
    // A phase without prediction waits for the inputs instead: on its way in, the
    // input delay takes the steps the others predicted this machine's inputs for
    // (and one for the jitter), since the measure stops seeing them there; on its
    // way out, it gives them back. Up to kMaxLockstepDelay, above the delay a
    // predicting phase may take: a steady delay there beats waiting on every step
    // when the machines are far apart.
    if (s.net.isPrevFramePhasePredictable && !isCurrentPredictable)
    {
        float worst;
        s.net.addedStepsInLockstep = 0;
        if (WorstReportedAhead(worst))
            s.net.addedStepsInLockstep = static_cast<int>(std::ceil(-worst)) + 1;
        s.net.addedStepsInLockstep = std::clamp(s.net.addedStepsInLockstep, 0, kMaxLockstepDelay - s.net.delay);
        s.net.delayChange = s.net.addedStepsInLockstep;
    }
    else if (!s.net.isPrevFramePhasePredictable && isCurrentPredictable)
    {
        s.net.delayChange = -s.net.addedStepsInLockstep;
        s.net.addedStepsInLockstep = 0;
    }
    s.net.isPrevFramePhasePredictable = isCurrentPredictable;
    if (!s.active || !isCurrentPredictable) return;
    ChooseDelay();
    s.net.leadSum += s.net.lead;
    s.net.maxLead = std::max(s.net.maxLead, static_cast<double>(s.net.lead));
    ++s.net.leadSamples;
    if (s.net.lead > kSlowAbove)
        s.net.slowing = true;
    else if (s.net.lead < kSlowUntil)
        s.net.slowing = false;
    if (!s.net.slowing) return;
    auto* fixed = static_cast<FixedUpdateTask*>(s.tasks.list[fixedIndexOf()]);
    const float removed = kSlowdown * std::max(Frame(s.hostFrame).dt[fixedIndexOf()], 0.0f);
    fixed->mAccumulatedDeltaT -= removed;
    s.net.slowedSeconds += removed;
}
} // namespace

// RECHARGED_ROLLBACK, at the start of a frame, before any task sets up its
// cameras and views.
void NetplayFrameStart()
{
    State& s = *sState;
    // Steps the hook between steps did not see (one whose presentation stopped
    // the simulation): undone when they predicted, the match goes on without
    // the rollback otherwise.
    const unsigned now = CurrentStep();
    if (now != s.latest)
    {
        bool predicted = false;
        for (unsigned f = s.latest + 1; f <= now; ++f)
            predicted = predicted || (InputsFor(f).frame == f && AnyPredicted(InputsFor(f)));
        if (!predicted || now < s.latest)
        {
            Stop("a step the rollback did not see");
            return;
        }
        ++s.net.undone;
        RollBack(s.latest, s.latest, s.replay, s.checksum);
    }
    // A step that predicted and changed the match's phase: it runs again when
    // its inputs have all arrived.
    if (s.net.undoFrom)
    {
        const unsigned from = s.net.undoFrom;
        s.net.undoFrom = 0;
        if (from <= s.latest && from > 0)
        {
            ++s.net.undone;
            RollBack(from - 1, from - 1, s.replay, s.checksum);
        }
    }
    Resolve();
    UpdateConfirmed();
    SyncTime();
    // A divergence ends the match as the game's own check did (which the
    // prediction bypasses): the network error, kept by objects no rollback
    // restores.
    if (s.net.matchDesync && !s.net.desyncShown)
    {
        s.net.desyncShown = true;
        if (s.net.seedDesync)
            gNetworkSyncState->OnRandomSeedMismatch(static_cast<int>(s.net.matchDesyncFrame));
        else
            gNetworkSyncState->OnChecksumMismatch(static_cast<int>(s.net.matchDesyncFrame));
    }
}

// The performance overlay (netplay_snapshot.cpp): this machine's input delay,
// in steps, and the most steps predicted at once since the last call. False
// outside a network match with rollback.
bool OverlayNumbers(int& delay, int& predicted)
{
    if (!mscharged_rollback_predicting() || !g_pGame) return false;
    std::lock_guard lock(sMutex);
    State& s = *sState;
    delay = s.net.delay;
    predicted = static_cast<int>(s.net.shownDepth);
    s.net.shownDepth = 0;
    return true;
}

} // namespace mscharged::rollback

using namespace mscharged::rollback;

extern "C" bool mscharged_rollback_predicting(void)
{
    if (!Running() || !sState->options.netplay || !g_pNetworkSessionBase || !gNetworkInputRouter) return false;
    return g_pNetworkSessionBase->GetSessionMode() != NET_MODE_LOCAL && !gNetworkInputRecording->mPlaybackReady;
}

extern "C" int mscharged_rollback_network_reset(void)
{
    std::lock_guard lock(sMutex);
    State& s = *sState;
    if (s.active) Stop("a new match");
    s.net.matchDesync = s.net.desyncShown = false;
    for (StepInputs& inputs : s.net.inputs) inputs.frame = ~0u;
    s.net.lastInputFrame = 0;
    std::fill(std::begin(s.net.firstPredicted), std::end(s.net.firstPredicted), 0u);
    s.net.haveLastReal = false;
    s.net.waitThrough = s.net.undoFrom = 0;
    s.net.captured = s.net.haveConfirmed = false;
    s.net.remoteCount = 0;
    // No step's checksum yet: an empty slot must not pass for step 0's (or for
    // the last match's steps) when another machine's arrives.
    for (SyncPoint& point : s.net.history) point.frame = ~0u;
    std::fill(std::begin(s.net.queuedAhead), std::end(s.net.queuedAhead), 0.0f);
    std::fill(std::begin(s.net.haveReported), std::end(s.net.haveReported), false);
    s.net.delay = s.options.inputDelay;
    s.net.shownDepth = 0;
    s.net.delayChange = 0;
    s.net.delayChangedAt = 0;
    s.net.lead = 0;
    s.net.slowing = false;
    s.net.addedStepsInLockstep = 0;
    s.net.isPrevFramePhasePredictable = true;
    Log("network match: input delay %d steps\n", s.options.inputDelay);
    FlushLog();
    return s.options.inputDelay;
}

extern "C" bool mscharged_rollback_has_input(void)
{
    std::lock_guard lock(sMutex);
    State& s = *sState;
    NetworkInputRouter* router = Router();
    if (router->mQueueCursor < router->mQueueLimit) return true; // the input delay's first steps
    const unsigned frame = CurrentStep() + 1;
    if (!CanPredict(frame))
    {
        // As the original router: every machine's inputs for the step, and
        // none still predicted before it.
        bool ready = !OldestPredicted();
        for (int machine = 0; ready && machine < MachineCount(); ++machine)
        {
            const StepInputs& kept = InputsFor(frame);
            ready = (kept.frame == frame && !kept.predicted[machine]) || router->m_InputQueue[machine].GetCount() > 0;
        }
        if (!ready) ++s.net.lockstepWaits;
        return ready;
    }
    if (const unsigned oldest = OldestPredicted(); oldest && frame - oldest + 1 >= static_cast<unsigned>(kMaxDepth))
    {
        ++s.net.windowWaits;
        return false;
    }
    return true;
}

extern "C" void mscharged_rollback_input_ready(void)
{
    std::lock_guard lock(sMutex);
    State& s = *sState;
    NetworkInputRouter* router = Router();
    const int machines = MachineCount();
    if (!s.net.haveLastReal)
    {
        for (int machine = 0; machine < machines; ++machine) s.net.lastReal[machine] = FromRouter(machine);
        s.net.haveLastReal = true;
    }
    const unsigned frame = CurrentStep() + 1;
    StepInputs& kept = InputsFor(frame);
    // A step undone and run again: the inputs that arrived for it then.
    const bool again = kept.frame == frame;
    StepInputs step{};
    step.frame = frame;
    step.machineCount = machines;
    if (router->mQueueCursor < router->mQueueLimit)
    {
        ++router->mQueueCursor;
        for (int machine = 0; machine < machines; ++machine) step.machines[machine] = Predicted(machine);
    }
    else
        for (int machine = 0; machine < machines; ++machine)
        {
            NetworkInputMessageQueue& queue = router->m_InputQueue[machine];
            if (again && !kept.predicted[machine])
            {
                step.machines[machine] = kept.machines[machine];
                s.net.lastReal[machine] = kept.machines[machine];
                Mirror(machine, kept.machines[machine]);
            }
            else if (!s.net.firstPredicted[machine] && queue.GetCount() > 0)
                step.machines[machine] = Arrived(machine, queue.Pop());
            else
            {
                step.machines[machine] = Predicted(machine);
                step.predicted[machine] = true;
                if (!s.net.firstPredicted[machine]) s.net.firstPredicted[machine] = frame;
            }
        }
    kept = step;
    if (AnyPredicted(step))
    {
        const unsigned depth = frame - OldestPredicted() + 1;
        ++s.net.predictedSteps;
        ++s.net.predictedDepth[std::min<unsigned>(depth, kMaxDepth)];
        s.net.shownDepth = std::max(s.net.shownDepth, depth);
    }
    Compose(step, s.pending);
    ApplyRollbackInputs(&s.pending);
    s.haveInputs = true;
    s.net.lastInputFrame = frame;
}

extern "C" bool mscharged_rollback_can_capture(void)
{
    std::lock_guard lock(sMutex);
    State& s = *sState;
    const unsigned frame = CurrentStep();
    if (s.net.captured && frame <= s.net.capturedFrame) return false;
    s.net.captured = true;
    s.net.capturedFrame = frame;
    if (s.net.delayChange < 0)
    {
        // A step less of input delay (of those asked): this step's capture is left out.
        ++s.net.delayChange;
        --s.net.delay;
        s.net.delayChangedAt = frame;
        ++s.net.delayShorter;
        Log("input delay %d steps from frame %u\n", s.net.delay, frame);
        return false;
    }
    return true;
}

extern "C" int mscharged_rollback_extra_captures(void)
{
    std::lock_guard lock(sMutex);
    State& s = *sState;
    if (s.net.delayChange <= 0) return 0;
    // Steps more: the same inputs once more for each.
    int steps = s.net.delayChange;
    s.net.delayChange = 0;
    s.net.delay += steps;
    s.net.delayChangedAt = CurrentStep();
    ++s.net.delayLonger;
    Log("input delay %d steps from frame %u\n", s.net.delay, s.net.delayChangedAt);
    return steps;
}

extern "C" int mscharged_rollback_sync_data(unsigned* checksum, unsigned* frame, unsigned* seed)
{
    std::lock_guard lock(sMutex);
    const State& s = *sState;
    // Every other step the checksum, the others the time sync's measure (the
    // other machines' average, by 256ths of a step).
    if (CurrentStep() % 2 == 0 && s.net.haveConfirmed)
    {
        const SyncPoint& point = s.net.history[s.net.confirmed % kConfirmedRing];
        if (point.frame != s.net.confirmed) return 0;
        *checksum = point.checksum;
        *frame = point.frame;
        *seed = point.seed;
        return 1;
    }
    float ahead = 0;
    int others = 0;
    for (int machine = 0; machine < MachineCount(); ++machine)
        if (machine != g_pNetworkSessionBase->GetLocalMachineId())
        {
            ahead += s.net.queuedAhead[machine];
            ++others;
        }
    const auto measure = static_cast<std::int32_t>(std::lround(ahead / std::max(others, 1) * 256.0f));
    *frame = (static_cast<unsigned>(s.net.delay) << 24) | (static_cast<unsigned>(measure) & 0xFFFFFFu);
    return 2;
}
