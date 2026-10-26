// <file>.summary: the rollback's counts and costs, rewritten every two
// seconds of steps (a crash leaves recent numbers) and when tracking stops.
#include "platform/rollback_state.h"

#include <algorithm>

namespace mscharged::rollback
{
void WriteSummary()
{
    const State& s = *sState;
    if (!s.summaryPath) return; // no log
    WriteAudit(s.summaryPath);
    FILE* file = std::fopen(s.summaryPath, "w");
    if (!file) return;
    const auto& h = mscharged::platform::RollbackSnapshotStatistics();
    if (s.options.netplay)
    {
        std::fprintf(file, "rollback: network matches predict, input delay %d steps\n", s.options.inputDelay);
        std::fprintf(file, "DESYNCS: %llu", static_cast<unsigned long long>(s.net.desyncs));
        if (s.net.desyncs) std::fprintf(file, " (first at frame %u)", s.net.firstDesyncFrame);
        std::fprintf(file, "; checksums compared with other machines %llu; ROLLBACKS IMPOSSIBLE %llu\n",
                     static_cast<unsigned long long>(s.net.remoteChecks), static_cast<unsigned long long>(s.net.impossible));
        std::fprintf(file, "steps with predicted inputs %llu, by how many steps predicted:",
                     static_cast<unsigned long long>(s.net.predictedSteps));
        for (int d = 1; d <= kMaxDepth; ++d)
            if (s.net.predictedDepth[d])
                std::fprintf(file, " %d: %llu", d, static_cast<unsigned long long>(s.net.predictedDepth[d]));
        std::fprintf(file, "\nmispredicted steps by machine:");
        for (int m = 0; m < kMachines; ++m)
            std::fprintf(file, " %llu", static_cast<unsigned long long>(s.net.mispredictions[m]));
        std::fprintf(file, "; rollbacks to correct them %llu\n",
                     static_cast<unsigned long long>(s.net.correctionRollbacks));
        static const char* const kPartNames[kInputParts] = {"buttons", "triggers", "remote accel",
            "nunchuk accel", "pointer targets", "pointer", "sticks", "player state", "events", "other"};
        std::fprintf(file, "mispredicted steps that got wrong:");
        for (int part = 0; part < kInputParts; ++part)
            std::fprintf(file, " %s %llu", kPartNames[part], static_cast<unsigned long long>(s.net.mispredictedParts[part]));
        std::fprintf(file, "\n");
        std::fprintf(file, "steps undone (a phase change on predicted inputs) %llu, replays cut at a phase change "
                           "%llu; waits for inputs: as the original router %llu, prediction window full %llu\n",
                     static_cast<unsigned long long>(s.net.undone), static_cast<unsigned long long>(s.net.truncated),
                     static_cast<unsigned long long>(s.net.lockstepWaits), static_cast<unsigned long long>(s.net.windowWaits));
        std::fprintf(file, "input delay now %d steps (from %d), lengthened %llu times, shortened %llu\n", s.net.delay,
                     s.options.inputDelay, static_cast<unsigned long long>(s.net.delayLonger),
                     static_cast<unsigned long long>(s.net.delayShorter));
        std::fprintf(file, "replayed steps not saved (no rollback could go back to them) %llu\n",
                     static_cast<unsigned long long>(s.stats.savesSkipped));
        std::fprintf(file, "time sync: ahead of the others by %.2f steps now, %.2f on average, %.2f at most; "
                           "slowed by %.2f s in all\n",
                     s.net.lead, s.net.leadSamples ? s.net.leadSum / static_cast<double>(s.net.leadSamples) : 0.0, s.net.maxLead,
                     s.net.slowedSeconds);
    }
    else
        std::fprintf(file, "rollback test: up to %d steps back every %d steps\n", s.options.maxDepth, s.options.interval);
    std::fprintf(file, "%llu steps, tracking started %d times; %d regions, %zu pages tracked, %zu hot\n",
                 static_cast<unsigned long long>(s.stats.stepCount), s.stats.starts, s.memory.regionCount, h.trackedPages, h.hotPages);
    std::fprintf(file, "rollbacks: %llu, steps replayed %llu, frames %llu replayed as %llu; by steps undone:",
                 static_cast<unsigned long long>(s.stats.rollbacks), static_cast<unsigned long long>(s.stats.replayed),
                 static_cast<unsigned long long>(s.stats.framesReplayed + s.stats.framesMerged),
                 static_cast<unsigned long long>(s.stats.framesReplayed));
    for (int d = 1; d <= kMaxDepth; ++d)
        if (s.stats.byDepth[d]) std::fprintf(file, " %d: %llu", d, static_cast<unsigned long long>(s.stats.byDepth[d]));
    std::fprintf(file, "\nREPLAYED STEPS WITH ANOTHER CHECKSUM: %llu", static_cast<unsigned long long>(s.stats.mismatches));
    if (s.stats.mismatches) std::fprintf(file, " (first at frame %u)", s.stats.firstMismatchFrame);
    std::fprintf(file, "\nfailed restores %llu; replays reaching a NIS %llu; frees the registry could not undo %llu; "
                       "pages written behind the tracking %llu\n",
                 static_cast<unsigned long long>(s.stats.failedRestores), static_cast<unsigned long long>(s.stats.nisInReplay),
                 static_cast<unsigned long long>(s.stats.lossyFrees), static_cast<unsigned long long>(s.stats.untracked));
    std::fprintf(file, "skipped: disc read in flight %llu, disc read started %llu, game or task state change %llu, "
                       "too many events %llu, too few steps saved %llu\n",
                 static_cast<unsigned long long>(s.stats.skippedReads), static_cast<unsigned long long>(s.stats.skippedStarts),
                 static_cast<unsigned long long>(s.stats.skippedPhases),
                 static_cast<unsigned long long>(s.stats.skippedEvents), static_cast<unsigned long long>(s.stats.skippedShort));
    std::fprintf(file, "mega strike meter presses received %llu, given again in replays %llu\n",
                 static_cast<unsigned long long>(s.stats.meterReceived), static_cast<unsigned long long>(s.stats.meterRedelivered));
    if (h.saves)
        std::fprintf(file, "save: %.1f us avg / %.1f us max; %llu pages avg / %llu max; %zu hot now\n",
                     h.saveNs / 1e3 / h.saves, h.maxSaveNs / 1e3,
                     static_cast<unsigned long long>(h.savedPages / h.saves),
                     static_cast<unsigned long long>(h.maxSavedPages), h.hotPages);
    if (s.stats.rollbacks)
    {
        std::fprintf(file, "restore: %.1f us avg / %.1f us max\n", s.stats.restoreNs / 1e3 / s.stats.rollbacks,
                     s.stats.maxRestoreNs / 1e3);
        std::fprintf(file, "rollback (restore, replay, saves): %.2f ms avg / %.2f ms max\n",
                     s.stats.rollbackNs / 1e6 / s.stats.rollbacks, s.stats.maxRollbackNs / 1e6);
    }
    if (s.stats.framesReplayed)
    {
        const double frames = static_cast<double>(s.stats.framesReplayed);
        std::fprintf(file, "replay, us per frame replayed:");
        for (int i = 0; i < s.tasks.count; ++i)
            if (s.stats.taskNs[i])
            {
                const char* name = s.tasks.list[i]->GetName();
                std::fprintf(file, " %s %.0f", name ? name : "?", s.stats.taskNs[i] / 1e3 / frames);
            }
        std::fprintf(file, "; the fixed update's work before its steps %.0f", s.stats.beforeStepsNs / 1e3 / frames);
        if (s.stats.replayed)
            std::fprintf(file, "; %.2f steps per frame replayed, us per step replayed: step %.0f (checksum %.0f), "
                               "save %.0f (registry %.0f); per step replayed: faults %.0f, pages saved %.0f of %.0f hot",
                         s.stats.replayed / frames, s.stats.stepNs / 1e3 / s.stats.replayed,
                         s.stats.checksumNs / 1e3 / s.stats.replayed, s.stats.saveNs / 1e3 / s.stats.replayed,
                         s.stats.markNs / 1e3 / s.stats.replayed,
                         static_cast<double>(s.stats.replayFaults) / s.stats.replayed,
                         static_cast<double>(s.stats.replaySavedPages) / s.stats.replayed,
                         static_cast<double>(s.stats.replayHotPages) / s.stats.replayed);
        std::fprintf(file, "\n");
    }
    const auto registry = mscharged::platform::CurrentGameAllocationCheckpointStats();
    std::fprintf(file, "registry: %llu copies of an allocation's facts, %zu journal entries held; changes it does "
                       "not follow %llu, failed copies %llu\n",
                 static_cast<unsigned long long>(registry.copies), registry.entries,
                 static_cast<unsigned long long>(registry.untracked),
                 static_cast<unsigned long long>(registry.failedCopies));
    std::fprintf(file, "faults %llu, cold pages turned hot %llu, hot turned cold %llu, saves dropped early %llu\n",
                 static_cast<unsigned long long>(h.faults), static_cast<unsigned long long>(h.promoted),
                 static_cast<unsigned long long>(h.demoted), static_cast<unsigned long long>(h.evicted));
    if (s.options.compareEvery)
    {
        std::fprintf(file, "\ncompared every %d rollbacks: %llu, %llu with differences, %llu bytes in all; "
                           "places %d (%llu not told apart)\n",
                     s.options.compareEvery, static_cast<unsigned long long>(s.compare.compares),
                     static_cast<unsigned long long>(s.compare.comparesDiffering),
                     static_cast<unsigned long long>(s.compare.compareDiffBytes), s.compare.placeCount,
                     static_cast<unsigned long long>(s.compare.placesDropped));
        // The places, most often differing first (sorting a copy of the indices).
        int order[kMaxPlaces];
        for (int i = 0; i < s.compare.placeCount; ++i) order[i] = i;
        std::sort(order, order + s.compare.placeCount, [&](int a, int b) {
            return s.compare.places[a].compares != s.compare.places[b].compares ? s.compare.places[a].compares > s.compare.places[b].compares
                                                                : s.compare.places[a].bytes > s.compare.places[b].bytes;
        });
        for (int i = 0; i < s.compare.placeCount && i < kListedPlaces * 2; ++i)
        {
            const Place& place = s.compare.places[order[i]];
            std::fprintf(file, "  %llu times, %llu bytes: ", static_cast<unsigned long long>(place.compares),
                         static_cast<unsigned long long>(place.bytes));
            PrintPlace(file, place);
            std::fprintf(file, "\n");
        }
    }
    std::size_t recordsUsed = 0, recordsPeak = 0, recordsBytes = 0;
    mscharged::platform::GameResourceRecordUsage(&recordsUsed, &recordsPeak, &recordsBytes);
    std::fprintf(file, "pool records' heap: %zu of %zu bytes used, %zu at most\n", recordsUsed, recordsBytes,
                 recordsPeak);
    if (sAudioHeap)
        std::fprintf(file, "audio heap: %u of %zu bytes free, largest %u\n", sAudioAllocator.TotalFreeMemory(),
                     kAudioHeapBytes, sAudioAllocator.LargestFreeBlock());
    std::fprintf(file, "\nnot restored:\n");
    for (int i = 0; i < s.memory.excludedCount; ++i)
        std::fprintf(file, "  %s: %zu bytes\n", s.memory.excluded[i].name,
                     static_cast<std::size_t>(s.memory.excluded[i].end - s.memory.excluded[i].start));
    std::fprintf(file, "restored inside those: %d ranges (the network players' inputs)\n", s.memory.keptCount);
    std::fclose(file);
}
} // namespace mscharged::rollback
