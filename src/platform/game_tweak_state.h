#pragma once

// The game module's tweak state that a match replay keeps and sets again
// (game_tweaks.cpp, read and set by match_replay.cpp): the enabled tweaks and
// Training's settings, which the match plays with.
namespace mscharged::platform
{
struct TrainingTweakState
{
    int item = -1;        // ePowerUpType, -1 none, 9 the captain's own
    int item_count = 1;   // 1, 3 or 5
    bool item_giant = false;
    int away_fielders = 0;
    bool away_goalie = true;
    int away_skill = -1;  // 0 training to 5, -1 the game's own setting
    bool play_away = false;
    bool skip_goal = false;
};

unsigned EnabledTweaks();
void SetEnabledTweaks(unsigned mask);
TrainingTweakState GetTrainingTweakState();
void SetTrainingTweakState(const TrainingTweakState& state);
} // namespace mscharged::platform
