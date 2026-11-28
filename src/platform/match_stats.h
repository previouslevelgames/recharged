#pragma once

// Match stats for replays, inside the original game module (tweak patch
// 0085). The game's StatsTracker counts every player's shots, goals, passes,
// hits, items... for the match; the recorder keeps its totals in the replay,
// with the moments the stats screens of the launcher show (goals, shots,
// items, hits, steals, fouls: when, who and where) and who played. Nothing
// here changes the game: the hooks only read.
//
// Online, the steps a player's machine predicts run again when the other
// machine's inputs differ (the rollback): the moments of the steps it undoes
// go (StatsRolledBack), and their steps add theirs again as they run.

#include "platform/match_replay_stream.h"

#include <cstdint>

namespace mscharged::replay
{
// A match's recording begins: no moments yet, the names and the field as the
// game has them now.
void StatsBegin();
// After each recorded step: the totals as the game counts them now.
void StatsStep();
// The rollback went back to the save after step `frame` (the fixed update's
// frame count): the moments after it are undone.
void StatsRolledBack(std::uint32_t frame);
// The recording ends: its stats (none when no recording was running).
MatchStats StatsFinish();
} // namespace mscharged::replay
