#pragma once

#include "bootstrap/game_tweaks.h"
#include "overlay/overlay_frame.h"

#include <array>
#include <optional>
#include <string>

// The overlay's Game options tab (game_overlay.h): the gameplay options that
// can change while the game runs, grouped like the launcher's Gameplay page,
// with their names and descriptions from kGameTweaks. They count from the
// next match (Fast menus at once); a running match keeps what it started
// with, and online matches play by their own rules. Skip intro, Unlock
// everything, Robot goalies, Super Team, texture packs and the display stay
// in the launcher.
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

// What the tab keeps between frames.
struct GameOptionsState
{
    // The option the description beside the list shows: the one the cursor
    // or the mouse was on last.
    std::optional<GameTweak> shown;
};

// The tab's first screen, and the title of the screen on top of `screens`.
OverlayScreens::Screen GameOptionsFirstScreen();
std::string GameOptionsTitle(const OverlayScreens& screens);
// The content of the screen on top of `screens`, in `layout`.
void DrawGameOptions(const GameOptionsModel& model, GameOptionsState& state, OverlayScreens& screens,
                     std::optional<GameOptionChange>& change, const OverlayLayout& layout);
} // namespace mscharged::overlay
