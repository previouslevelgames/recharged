#pragma once

#include "bootstrap/game_tweaks.h"

#include <array>
#include <optional>
#include <string>
#include <vector>

// The menus' GAMEPLAY page (src/ui/rui_pages.cpp): the gameplay options, by
// category, with their names and descriptions from kGameTweaks. They count
// from the next match (Fast menus and Unlock everything at once, Skip intro
// from the next start); a running match keeps what it started with, and
// online matches play by their own rules. The Super Team, the development
// switch and texture packs stay in the launcher's Extras.
namespace mscharged::overlay
{
struct GameOptionsModel
{
    // On or off, in kGameTweaks' order (Settings::tweaks).
    std::array<bool, kGameTweaks.size()> tweaks = DefaultGameTweaks();
    bool in_match = false; // a match is played: changes count from the next
};

// An option switched on the page; the host saves it ([gameplay] in
// recharged.ini) and hands it to the game.
struct GameOptionChange
{
    GameTweak tweak;
    bool on = false;
};

// Whether the page offers `tweak`, and whether a change counts at once (not
// only from the next match).
bool GameOption(GameTweak tweak);
bool GameOptionAtOnce(GameTweak tweak);
// Whether it counts only from the game's next start.
bool GameOptionNextStart(GameTweak tweak);

// The page's categories in their order (the launcher Gameplay page's): each
// one's title, what its options do and when (in the player's language), and
// its options.
struct GameOptionGroup
{
    const char* title;       // its name in logs (English)
    const char* short_title; // "Menus", "Matches", "Competitive"
    const char* about;
    std::vector<GameTweak> tweaks;
};
std::vector<GameOptionGroup> GameOptionGroups();

} // namespace mscharged::overlay
