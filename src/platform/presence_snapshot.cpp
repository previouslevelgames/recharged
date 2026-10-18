// The Discord status (issue #71): reads what the game is doing from its own
// objects for the host, after a game frame. Read only: the game is unchanged.
#if !defined(MSCHARGED_NATIVE) || !defined(MSCHARGED_GAME_MODULE)
#error The presence snapshot belongs to the isolated original game module
#endif
#include "platform/presence_snapshot.h"

#include "Game/Game.h"
#include "Game/GameInfo.h"
#include "Game/GameSceneManager.h"
#include "Game/Sys/clock.h"
#include "Game/Team.h"

extern "C" __attribute__((visibility("default"))) bool charged_presence_snapshot(ChargedPresenceSnapshot* snapshot,
                                                                                 unsigned size)
{
    if (!snapshot || size != sizeof(ChargedPresenceSnapshot)) return false;
    ChargedPresenceSnapshot s{};
    for (int& scene : s.scenes) scene = SCENE_INVALID;
    s.mode = -1;
    s.game_state = GS_NONE;
    s.teams[0] = s.teams[1] = TEAM_INVALID;
    s.stadium = STAD_INVALID;

    if (const GameSceneManager* scenes = nlSingleton<GameSceneManager>::Instance())
    {
        const unsigned long depth = scenes->mCurrentStackDepth;
        if (depth <= BaseGameSceneManager::MAX_SCENE_DEPTH)
            for (unsigned long n = 0; n < depth && s.scene_count < 8; ++n)
                s.scenes[s.scene_count++] = scenes->m_sceneStack[depth - 1 - n];
    }

    if (const GameInfoManager* info = nlSingleton<GameInfoManager>::Instance())
    {
        if (info->mCurrentMode >= 0 && info->mCurrentMode < GameInfoManager::GM_NUM_MODES)
        {
            s.mode = info->mCurrentMode;
            if (const BasicGameInfo* game = info->mGameInfo[info->mCurrentMode])
            {
                s.teams[0] = game->mTeamIndex[0];
                s.teams[1] = game->mTeamIndex[1];
                s.stadium = game->mStadiumIndex;
                for (const short side : game->mPadSides)
                    if (side == 0 || side == 1) s.human_sides |= 1 << side;
            }
        }
        s.online = info->mIsOnlineMode;
        s.ranked = info->mIsOnlineMode && info->mOnlineRankedMatch;
        s.strikers101 = info->mIsInStrikers101Mode != 0;
    }

    if (cGame* game = g_pGame)
    {
        s.in_match = 1;
        s.game_state = game->m_eGameState;
        s.game_duration = game->m_fGameDuration;
        if (game->m_pGameClock) s.game_time = game->m_pGameClock->m_fTimer;
        for (int side = 0; side < 2; ++side)
            if (g_pTeams[side]) s.scores[side] = g_pTeams[side]->GetScore();
    }
    *snapshot = s;
    return true;
}
