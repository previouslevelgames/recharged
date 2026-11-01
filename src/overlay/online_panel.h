#pragma once

#include "overlay/online_panel_model.h"
#include "overlay/overlay_frame.h"

#include "imgui.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>

// The overlay's Online tab (game_overlay.h), drawn with ImGui in the
// overlay's frame (overlay_frame.h). Two screens: the main one (the player,
// the switch for random games and their set beside the friends online and the
// players available; one column when the window is narrow), and a player's
// screen, opened by a row, to challenge them or keep them as a friend. Mouse,
// keyboard and gamepad (ImGui's navigation) work alike: A or Enter on a row
// opens the player, B goes back.
namespace mscharged::overlay
{
// What the player did in the Online tab this frame; the caller acts on it
// (asks the profile service). Fresh for every frame.
struct OnlinePanelActions
{
    std::optional<std::int64_t> challenge;     // a player's id: a set by the player's rules
    std::optional<std::int64_t> add_friend;    // a player's id
    std::optional<std::int64_t> remove_friend; // a player's id
    std::optional<std::int32_t> add_mii_friend;    // a Mii profile
    std::optional<std::int32_t> remove_mii_friend; // a Mii profile
    std::optional<bool> set_available; // available for random games, or not
    // The set's (OnlinePanelSet::match): accept or decline its challenge,
    // Ready to play!, withdraw or cancel it, play it again.
    bool accept = false, decline = false, ready = false, cancel = false, rematch = false;

    bool Any() const
    {
        return challenge || add_friend || remove_friend || add_mii_friend || remove_mii_friend || set_available
            || accept || decline || ready || cancel || rematch;
    }
};

// The tab's screens (OverlayScreens::Screen::id).
enum class OnlinePanelScreen
{
    Main = 0,
    Player = 1, // Screen::key: the player's OnlinePanelRow::key
};

// The texture for a picture's key (OnlinePanelRow::image: an avatar's URL,
// "mii:<profile>"), or 0 while there is none: its initials show then. Asked
// every frame for each picture on screen; may start loading it.
using OverlayImages = std::function<ImTextureID(const std::string& key)>;

// A player's picture as the lists draw it: their texture, else their
// initials on their colour (a Mii a face), with the dot of their presence
// ringed in `surface`, the colour it sits on.
void DrawPlayerPicture(ImDrawList* draw, ImVec2 center, float radius, const OnlinePanelRow& row,
                       const OverlayImages& images, const ImVec4& surface);

// The tab's first screen.
OverlayScreens::Screen OnlinePanelFirstScreen();
// The title of the screen on top of `screens`.
std::string OnlinePanelTitle(const OnlinePanelModel& model, const OverlayScreens& screens);
// The content of the screen on top of `screens`, in `layout`.
void DrawOnlinePanel(const OnlinePanelModel& model, OverlayScreens& screens, OnlinePanelActions& actions,
                     const OverlayImages& images, const OverlayLayout& layout);
} // namespace mscharged::overlay
