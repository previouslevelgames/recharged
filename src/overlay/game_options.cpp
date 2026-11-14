#include "overlay/game_options.h"

#include "bootstrap/texts.h"

#include <algorithm>

namespace mscharged::overlay
{
std::vector<GameOptionGroup> GameOptionGroups()
{
    // The options the game offers, in the categories and order of the
    // launcher's Gameplay page (whose other options stay there).
    return {
        {"Menus & startup", texts::T("menus.gameplay.menus.name"), texts::T("menus.gameplay.menus.about"),
         {GameTweak::FastMenus}},
        {"Matches & unlocks", texts::T("menus.gameplay.matches.name"), texts::T("menus.gameplay.matches.about"),
         {GameTweak::CpuVsCpu, GameTweak::MirrorMatch, GameTweak::BluePeach}},
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

bool GameOptionAtOnce(GameTweak tweak) { return tweak == GameTweak::FastMenus; }
} // namespace mscharged::overlay
