#pragma once

// What the game is doing, for the Discord status (issue #71). The game module
// fills it (charged_presence_snapshot); the host reads it after a game frame,
// on the thread that runs the game's tasks. Numbers are the game's own enums.
struct ChargedPresenceSnapshot
{
    // The menu screens on the game's scene stack, top first (SceneList); the
    // rest are -2. In a match the stack holds the pause menu while it is open.
    int scenes[8];
    int scene_count;
    // GameInfoManager::eGameMode: 0 Domination, 1 online tournament, 2 the
    // title screen's demo match, 3 Road to the Striker Cup, 4 Striker
    // Challenges; -1 before the game set one up.
    int mode;
    int online;      // GameInfoManager::mIsOnlineMode
    int ranked;      // an online ranked match
    int strikers101; // a Strikers 101 lesson
    // A match exists (from match loading until it is unloaded).
    int in_match;
    int game_state;      // cGame eGameState, -1 outside matches
    int teams[2];        // eTeamID of the home and away captain, -1 unknown
    int stadium;         // eStadiumID, -1 unknown
    int scores[2];       // goals of home and away
    float game_time;     // seconds on the match clock
    float game_duration; // the match length in seconds
    // Bit 0: a player on this machine plays the home team, bit 1: the away
    // team (none: the computer plays both).
    int human_sides;
};

#ifdef __cplusplus
extern "C" {
#endif
// Game module entry: fills `snapshot` when `size` matches this build's
// struct; false otherwise.
typedef bool (*ChargedPresenceSnapshotFunction)(ChargedPresenceSnapshot* snapshot, unsigned size);
#ifdef __cplusplus
}
#endif
