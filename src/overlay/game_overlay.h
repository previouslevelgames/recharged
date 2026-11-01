#pragma once

#include "overlay/game_options.h"
#include "overlay/online_panel.h"
#include "overlay/overlay_frame.h"

#include <optional>

// The game's overlay over its blurred picture: tabs across the top (Online,
// Game options), each with its own screens, in one frame (overlay_frame.h).
// The host opens it on a tab (F1 or HOME on Online, F2 on Game options),
// feeds it the data and acts on what the player did. Inside, the shoulder
// buttons (L1/R1, also L2/R2), Q/E, Ctrl+Tab / Ctrl+Shift+Tab and the mouse
// switch tabs; B or Backspace go back a screen; Escape, Close and a click
// beside the window close it.
namespace mscharged::overlay
{
enum class OverlayTab
{
    Online = 0,
    GameOptions = 1,
};
inline constexpr int kOverlayTabs = 2;

struct GameOverlayModel
{
    OnlinePanelModel online;
    GameOptionsModel options;
};

// What the player did in this frame's overlay. Fresh for every frame.
struct GameOverlayActions
{
    bool close = false; // Escape, Close, a click beside the window, B on a tab's first screen
    OnlinePanelActions online;
    std::optional<GameOptionChange> set_option;

    bool Any() const { return close || online.Any() || set_option.has_value(); }
};

// What the overlay keeps between frames; the host holds one.
struct GameOverlayState
{
    // The host opens the overlay on `tab`: every tab on its first screen, the
    // keyboard and gamepad cursor on the tab's first item.
    void Open(OverlayTab tab = OverlayTab::Online);
    // While it is open: another tab (each keeps its screens), for the host's
    // own keys (F1/F2, a Wii Remote's - and +); the overlay's keys do it themselves.
    void Switch(OverlayTab tab);
    void NextTab();
    void PreviousTab();
    OverlayTab Tab() const { return tab; }
    OverlayScreens& Screens(OverlayTab which) { return which == OverlayTab::Online ? online : options; }

    OverlayTab tab = OverlayTab::Online;
    OverlayScreens online, options;
    GameOptionsState options_state;
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
