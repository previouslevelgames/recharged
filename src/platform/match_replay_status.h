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

#ifdef __cplusplus
}
#endif
