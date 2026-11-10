#include "overlay/game_options.h"

#include <algorithm>

namespace mscharged::overlay
{
const std::vector<GameOptionGroup>& GameOptionGroups()
{
    // The options the game offers, in the categories and order of the
    // launcher's Gameplay page (whose other options stay there).
    static const std::vector<GameOptionGroup> groups{
        {"Menus & startup", "Menus",
         "How the menus run; a change takes effect at once. Skip intro is in the launcher: it needs the game "
         "started again.",
         {GameTweak::FastMenus}},
        {"Matches & unlocks", "Matches",
         "Who plays and how the teams look; changes take effect from the next match. Robot goalies, Super Team and "
         "Unlock everything are in the launcher: they need the game started again.",
         {GameTweak::CpuVsCpu, GameTweak::MirrorMatch, GameTweak::BluePeach}},
        {"Competitive gameplay", "Competitive", "Rules for fairer matches; changes take effect from the next match.",
         {GameTweak::FastTerrain, GameTweak::SecureStadiums, GameTweak::ThunderIslandWalls,
          GameTweak::FairBallContests, GameTweak::NoMegaStrikes, GameTweak::MatchPoint}},
    };
    return groups;
}

bool GameOption(GameTweak tweak)
{
    for (const auto& group : GameOptionGroups())
        if (std::find(group.tweaks.begin(), group.tweaks.end(), tweak) != group.tweaks.end()) return true;
    return false;
}

bool GameOptionAtOnce(GameTweak tweak) { return tweak == GameTweak::FastMenus; }
} // namespace mscharged::overlay
