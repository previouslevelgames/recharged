#pragma once

#include "overlay/online_panel_model.h"

#include <string>

// A question over the game (the menus' prompt page, src/ui/rui_pages.cpp):
// when a set ends (to play it again), when it stopped, when someone
// challenges the player or asks for a rematch. A kicker and a title, the
// opponent (with the series' score for a played set), a line of detail, and
// two buttons: the primary answer and the secondary one ("Not now",
// "Decline"), which B and BACK give too.
namespace mscharged::overlay
{
struct OverlayPrompt
{
    enum class Kind
    {
        SetOver,    // the set was played: the score, and a rematch
        SetStopped, // the set ended without a result: play it again
        Challenge,  // someone challenges the player
        Rematch,    // the last opponent asks for a rematch
    };
    Kind kind = Kind::SetOver;
    std::string kicker, title, detail; // "SET OVER", "You won the set", the rules or why it stopped
    OnlinePanelRow opponent; // name, image key, initials, tint as in the rows; status optional
    // The series as this game counted it (best of `games`).
    bool has_score = false;
    int won = 0, lost = 0, games = 0;
    std::string primary, secondary; // the buttons' labels; no secondary: one button
    std::string note;               // a small line under the buttons (optional)
};

} // namespace mscharged::overlay
