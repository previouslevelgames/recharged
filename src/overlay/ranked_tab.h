#pragma once

#include "overlay/overlay_frame.h"
#include "platform/profile_service.h"

#include <optional>
#include <string>

struct ImGuiWindow;

// The Recharged Game Menu's Ranked tab: the game's online RANKED, as its
// online menu has it, from anywhere in its menus. Play RANKED sends the game
// by itself into a RANKED search against anybody (the host's
// charged_start_ranked); beside it the player's own place and the season's
// and today's leaders, as the game's LEADERBOARD menu shows them.
namespace mscharged::overlay
{
struct RankedModel
{
    // Play RANKED starts now; else `blocked` says why (under the button).
    bool can_play = false;
    std::string blocked;
    // The game is on its way to RANKED (Stop stops it), and waits for the
    // player to go back to the main menu or the online menu.
    bool going = false, waiting = false;
    // The player's own Mii plays; else they choose one in the game.
    bool own_mii = false;
    // The leaderboards: none without a player profile, still to come, or
    // none on this server (before 1.2.1).
    bool signed_out = false, loading = false, missing = false;
    std::optional<platform::Leaderboard> boards;
};

struct RankedState
{
    int board = 0; // the season's leaders, or today's
    // Right from Play RANKED to the boards (left back): the cursor jumps, as
    // the two aren't level.
    enum class Jump
    {
        None,
        ToBoards,
    } jump = Jump::None;
    // Play RANKED's button as last drawn, for the way back.
    unsigned play_id = 0;
    ImGuiWindow* play_window = nullptr;
};

struct RankedActions
{
    bool play = false; // Play RANKED
    bool stop = false; // the way to RANKED stops

    bool Any() const { return play || stop; }
};

// The tab's only screen (OverlayScreens::Screen::id).
OverlayScreens::Screen RankedFirstScreen();

void DrawRanked(const RankedModel& model, RankedState& state, RankedActions& actions, const OverlayLayout& layout);
} // namespace mscharged::overlay
