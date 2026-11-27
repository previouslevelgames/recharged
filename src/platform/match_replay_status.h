#pragma once

// Match replays (src/platform/match_replay.cpp): the module's exports the
// host calls, and what a replay that plays reports.
#ifdef __cplusplus
extern "C" {
#endif

struct ChargedReplayStatus
{
    int playing;            // a replay was handed over and is valid
    int started;            // its match began loading
    int finished;           // all its steps were played, or the match left
    unsigned steps;         // steps played
    unsigned total;         // steps in the replay
    int speed;              // -1 paused, 0 normal, 1 twice, 2 four times as fast
    unsigned checked;       // recorded checksums compared
    unsigned differences;   // of those, how many differed from the game's
    unsigned first_difference; // the step of the first difference
    char error[160];        // why the replay stopped early, or empty
    int live;               // a live stream (charged_configure_live_replay)
    unsigned behind;        // live: the steps that have arrived and not played yet
    int turbo;              // live: catching up as fast as this computer can, its sound off
};

// [replays] record: whether matches are recorded (before original main).
typedef void (*ChargedConfigureReplaysFunction)(int record);
// A replay to play (its stream, copied): nonzero when this module reads it.
typedef int (*ChargedConfigureReplayFunction)(const void* stream, unsigned size);
// The oldest finished recording: its size; copied and removed when `out` has
// room for it. 0 when none waits.
typedef unsigned (*ChargedTakeReplayFunction)(void* out, unsigned capacity);
// Ends a recording still running (the game closes during a match).
typedef void (*ChargedFinishReplayFunction)(void);
// A replay's progress; nonzero when filled.
typedef int (*ChargedReplayStatusFunction)(ChargedReplayStatus* status, unsigned size);
// A replay's speed: -1 paused, 0 normal, 1 twice, 2 four times as fast (any thread).
typedef void (*ChargedSetReplaySpeedFunction)(int speed);
// Online matches stream live as they are recorded (before original main).
typedef void (*ChargedConfigureLiveFunction)(int enabled);
// The sender's thread: the live stream's messages that fit (live_ring.h), the
// stream that stopped (0 none) and the steps the match runs ahead of its inputs.
typedef unsigned (*ChargedTakeLiveFunction)(void* out, unsigned capacity, unsigned* stopped, int* unconfirmed);
// The reports' number of the module's online series (ChargedOnlineMatchSnapshot::series).
typedef void (*ChargedNameOnlineSeriesFunction)(unsigned module_series, unsigned long long number);
// A live replay (before original main): the stream's first bytes (at least its
// setup) and the most it may grow to; nonzero when this module plays it.
typedef int (*ChargedConfigureLiveReplayFunction)(const void* start, unsigned size, unsigned capacity);
// More of a live replay's stream, from one thread; `complete` once it has all
// of it. Zero when it takes no more (full, or not a live replay).
typedef int (*ChargedFeedLiveReplayFunction)(const void* data, unsigned size, int complete);
// From the same thread: a player skipped part `part` of a cutscene of the game,
// `after` hundredths of a second into it (the server's word, beside the stream).
typedef void (*ChargedFeedLiveSkipFunction)(unsigned part, unsigned after);

#ifdef __cplusplus
}
#endif
