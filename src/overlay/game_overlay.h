#pragma once

#include "overlay/game_options.h"
#include "overlay/game_settings.h"
#include "overlay/online_panel.h"
#include "overlay/overlay_frame.h"
#include "overlay/ranked_tab.h"

#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

// The game's overlay over its blurred picture: tabs across the top (Online,
// Game options), each with its own screens, in one frame (overlay_frame.h).
// The host opens it on a tab (F1 or HOME on Online, F2 on Game options),
// feeds it the data and acts on what the player did. Inside, the shoulder
// buttons (L1/R1, also L2/R2), Q/E, Ctrl+Tab / Ctrl+Shift+Tab and the mouse
// switch tabs; B or Backspace go back a screen; Escape, Close and a click
// beside the window close it.
namespace mscharged::overlay
{
// The overlay's tabs by id. An id is not a place: the hosts' keys open a tab
// by its id wherever it stands, and tabs to come (ranked sets, tournaments)
// take new numbers.
enum class OverlayTab : int
{
    Online = 0,
    GameOptions = 1,
    Events = 2, // tournaments and organised match-ups: a page to come
    Settings = 3, // the display and sound settings that change at once
    Ranked = 4, // the game's online RANKED: play it, and its leaderboards
};

// A tab besides the game's own, with its page: the launcher's preview shows
// tabs to come with them; the game has none.
struct GameOverlayMoreTab
{
    OverlayTabs::Tab tab; // its id other than the game's tabs'
    std::string title;    // its page's title line (empty: the tab's label)
    std::function<void(const OverlayLayout&)> draw; // its page's content; none: empty
};

struct GameOverlayModel
{
    OnlinePanelModel online;
    RankedModel ranked;
    GameOptionsModel options;
    GameSettingsModel settings;
    std::vector<GameOverlayMoreTab> more_tabs; // after Online and Ranked, before Events, Game options and Settings (last)
};

// The tabs in their order with their badges (Online's: what waits for the
// player there), as the strip shows them.
std::vector<OverlayTabs::Tab> GameOverlayTabs(const GameOverlayModel& model);

// What the player did in this frame's overlay. Fresh for every frame.
struct GameOverlayActions
{
    bool close = false; // Escape, Close, a click beside the window, B on a tab's first screen
    OnlinePanelActions online;
    RankedActions ranked;
    std::optional<GameOptionChange> set_option;
    std::optional<GameSettingChange> set_setting;

    bool Any() const
    {
        return close || online.Any() || ranked.Any() || set_option.has_value() || set_setting.has_value();
    }
};

// What the overlay keeps between frames; the host holds one.
struct GameOverlayState
{
    // The host opens the overlay on `tab`: every tab on its first screen, the
    // keyboard and gamepad cursor on the tab's first item.
    void Open(OverlayTab tab = OverlayTab::Online);
    // While it is open: another tab (each keeps its screens), for the host's
    // own keys (F1/F2, a Wii Remote's - and +); the overlay's keys do it
    // themselves. False, and no change, for a tab that isn't there or doesn't
    // open now.
    bool Switch(OverlayTab tab);
    // The next or previous tab that opens, round from the last to the first.
    void NextTab();
    void PreviousTab();
    OverlayTab Tab() const { return tab; }
    OverlayScreens& Screens(OverlayTab which);

    OverlayTab tab = OverlayTab::Online;
    OverlayScreens online, ranked, options, settings;
    OnlinePanelState online_state; // the Online tab's challenge setup
    RankedState ranked_state;
    GameSettingsState settings_state;
    std::map<int, OverlayScreens> more; // the more tabs' (GameOverlayMoreTab), by id
    GameOptionsState options_state;
    // The tabs as the overlay last showed them (GameOverlayTabs); before
    // that, the game's own.
    std::vector<OverlayTabs::Tab> tabs;
};

struct GameOverlayStyle : OverlayStyle
{
    OverlayImages image; // the Online tab's pictures (OverlayImages)
};

// Draws the overlay as its own ImGui window over the style's area, in the
// current ImGui frame. It turns off ImGui's window switcher (Ctrl+Tab),
// whose keys switch its tabs.
void DrawGameOverlay(const GameOverlayModel& model, GameOverlayState& state, GameOverlayActions& actions,
                     const GameOverlayStyle& style);
} // namespace mscharged::overlay
