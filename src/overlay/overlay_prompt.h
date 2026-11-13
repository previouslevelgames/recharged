#pragma once

#include "overlay/online_panel.h"
#include "overlay/overlay_frame.h"

#include <string>
#include <vector>

// A prompt over the game in the overlay's look (frosted glass, contour,
// shadow, the accent), compact and without tabs: when a set ends (to play it
// again), when it stopped, when someone challenges the player or asks for a
// rematch. A kicker and a title, the opponent (with the series' score for a
// played set), a line of detail, and two buttons: the primary action (it has
// the keyboard and gamepad cursor as the prompt opens) and the secondary one.
// A or Enter (or a click) on a button presses it; B, Escape or a click beside
// the prompt is the secondary answer ("Not now", "Decline").
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
    // The keys shown in the buttons: a controller's and the keyboard's.
    std::vector<std::string> primary_keys{"A", "Enter"}, secondary_keys{"B", "Esc"};
    // The opponent's picture (OnlinePanelRow::image), or their initials.
    OverlayImages image;
};

struct OverlayPromptActions
{
    bool primary = false, secondary = false;
};

// What the prompt keeps between frames; the host holds one per prompt.
struct OverlayPromptState
{
    // The prompt shows (again): its entrance starts now, and the primary
    // button gets the cursor.
    void Open();

    bool focus = true;   // the primary button takes the cursor next frame
    float shown = 0.0f;  // seconds since it opened, for its entrance
};

// Draws the prompt as its own ImGui window over the style's area (its
// `appear` also fades it in; the prompt has its own entrance too).
void DrawOverlayPrompt(const OverlayPrompt& prompt, OverlayPromptState& state, OverlayPromptActions& actions,
                       const OverlayStyle& style);
} // namespace mscharged::overlay
