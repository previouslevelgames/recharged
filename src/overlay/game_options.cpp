#include "overlay/game_options.h"

#include "bootstrap/texts.h"

#include <algorithm>

namespace mscharged::overlay
{
std::vector<GameOptionGroup> GameOptionGroups()
{
    // The options the game offers. The Super Team (it reads a GameCube image)
    // and the development switch stay in the launcher's Extras.
    return {
        {"Menus & startup", texts::T("menus.gameplay.menus.name"), texts::T("menus.gameplay.menus.about"),
         {GameTweak::FastMenus, GameTweak::SkipIntro, GameTweak::UnlockAll}},
        {"Matches & unlocks", texts::T("menus.gameplay.matches.name"), texts::T("menus.gameplay.matches.about"),
         {GameTweak::CpuVsCpu, GameTweak::MirrorMatch, GameTweak::BluePeach, GameTweak::RobotGoalies}},
        {"Competitive gameplay", texts::T("menus.gameplay.competitive.name"),
         texts::T("menus.gameplay.competitive.about"),
         {GameTweak::FastTerrain, GameTweak::SecureStadiums, GameTweak::ThunderIslandWalls,
          GameTweak::FairBallContests, GameTweak::NoMegaStrikes, GameTweak::MatchPoint}},
    };
}

bool GameOption(GameTweak tweak)
{
    for (const auto& group : GameOptionGroups())
        if (std::find(group.tweaks.begin(), group.tweaks.end(), tweak) != group.tweaks.end()) return true;
    return false;
}

bool GameOptionAtOnce(GameTweak tweak) { return tweak == GameTweak::FastMenus || tweak == GameTweak::UnlockAll; }

bool GameOptionNextStart(GameTweak tweak) { return tweak == GameTweak::SkipIntro; }
} // namespace mscharged::overlay
