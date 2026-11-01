#pragma once

#include "overlay/online_panel_model.h"
#include "overlay/overlay_frame.h"

#include "imgui.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>

// The overlay's Online tab (game_overlay.h), drawn with ImGui in the
// overlay's frame (overlay_frame.h). Three screens: the main one (the player,
// the switch for random games and their set beside the friends online and
// the players available; one column when the window is narrow), a player's
// screen, opened by a row, to challenge them or keep them as a friend, and a
// challenge's setup: a preset (the server's rule sets, the player's own), all
// its rules to change for this challenge, and Send challenge. Mouse, keyboard
// and gamepad (ImGui's navigation) work alike: A or Enter on a row opens the
// player, B goes back; on a rule, A or Enter takes the next value, left and
// right go through them.
namespace mscharged::overlay
{
// What the player did in the Online tab this frame; the caller acts on it
// (asks the profile service). Fresh for every frame.
// A challenge as its setup sends it: by one of the server's rule sets as it
// is (`rule_set`), else by `rules`, under the preset's name.
struct OnlinePanelChallenge
{
    std::int64_t player = 0;
    std::string name;
    platform::MatchRules rules;
    std::int64_t rule_set = 0;
};

struct OnlinePanelActions
{
    std::optional<OnlinePanelChallenge> challenge;
    // A search for a random opponent by these rules (`player` 0), or its stop.
    std::optional<OnlinePanelChallenge> search;
    bool stop_search = false;
    std::optional<std::int64_t> add_friend;    // a player's id
    std::optional<std::int64_t> remove_friend; // a player's id
    std::optional<std::int32_t> add_mii_friend;    // a Mii profile
    std::optional<std::int32_t> remove_mii_friend; // a Mii profile
    std::optional<bool> set_available; // open to challenges, or not
    // The preset a challenge's setup opens with from now on (its name, one
    // of OnlinePanelModel::presets): the one a challenge was sent with.
    std::optional<std::string> set_challenge_rules;
    // The set's (OnlinePanelSet::match): accept or decline its challenge,
    // Ready to play!, withdraw or cancel it, play it again.
    bool accept = false, decline = false, ready = false, cancel = false, rematch = false;

    bool Any() const
    {
        return challenge || search || stop_search || add_friend || remove_friend || add_mii_friend
            || remove_mii_friend || set_available || set_challenge_rules || accept || decline || ready || cancel
            || rematch;
    }
};

// The tab's screens (OverlayScreens::Screen::id).
enum class OnlinePanelScreen
{
    Main = 0,
    Player = 1, // Screen::key: the player's OnlinePanelRow::key
    Setup = 2,  // a challenge to the player of Screen::key; "search": a search for a random opponent
};

// What the tab keeps between frames: the challenge being set up, its player's
// key, the preset it started from (in OnlinePanelModel::presets) and its
// rules as the player changed them.
struct OnlinePanelState
{
    std::string setup_for;
    int preset = 0;
    platform::MatchRules rules;
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
void DrawOnlinePanel(const OnlinePanelModel& model, OverlayScreens& screens, OnlinePanelState& state,
                     OnlinePanelActions& actions, const OverlayImages& images, const OverlayLayout& layout);
} // namespace mscharged::overlay
