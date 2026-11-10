#pragma once

#include "bootstrap/game_tweaks.h"
#include "overlay/overlay_rows.h"

#include <array>
#include <optional>
#include <string>
#include <vector>

// The overlay's Game options tab (game_overlay.h): the gameplay options that
// can change while the game runs, in the launcher Gameplay page's categories
// (a menu at the left, as the Settings tab's), each category's switches
// beside it with their names and descriptions from kGameTweaks. They count
// from the next match (Fast menus at once); a running match keeps what it
// started with, and online matches play by their own rules. Skip intro,
// Unlock everything, Robot goalies, Super Team, texture packs and the display
// stay in the launcher.
namespace mscharged::overlay
{
struct GameOptionsModel
{
    // On or off, in kGameTweaks' order (Settings::tweaks).
    std::array<bool, kGameTweaks.size()> tweaks = DefaultGameTweaks();
    bool in_match = false; // a match is played: changes count from the next
};

// An option switched in the tab; the host saves it ([gameplay] in
// recharged.ini) and hands it to the game.
struct GameOptionChange
{
    GameTweak tweak;
    bool on = false;
};

// Whether the tab offers `tweak`, and whether a change counts at once (not
// only from the next match).
bool GameOption(GameTweak tweak);
bool GameOptionAtOnce(GameTweak tweak);

// The tab's categories in their order (the launcher Gameplay page's): each
// one's title, what its options do and when, and its options.
struct GameOptionGroup
{
    const char* title;
    const char* short_title; // "Menus", "Matches", "Competitive"
    const char* about;
    std::vector<GameTweak> tweaks;
};
const std::vector<GameOptionGroup>& GameOptionGroups();

// What the tab keeps between frames: its menu's (overlay_rows.h), whose
// sections are Menus & startup, Matches & unlocks and Competitive gameplay.
struct GameOptionsState : OverlaySectionState
{
};

// The tab's first screen, and the title of the screen on top of `screens`.
OverlayScreens::Screen GameOptionsFirstScreen();
std::string GameOptionsTitle(const OverlayScreens& screens);
// The content of the screen on top of `screens`, in `layout`.
void DrawGameOptions(const GameOptionsModel& model, GameOptionsState& state, OverlayScreens& screens,
                     std::optional<GameOptionChange>& change, const OverlayLayout& layout);
} // namespace mscharged::overlay
