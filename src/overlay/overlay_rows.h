#pragma once

#include "overlay/overlay_frame.h"

#include "imgui.h"
#include "imgui_internal.h"

#include <optional>
#include <string>
#include <vector>

// Rows of choices and switches in groups, for the overlay's lists of settings
// (a challenge's setup, the Settings tab): a row is one control the whole
// width; mouse, keyboard and gamepad work alike.
namespace mscharged::overlay
{
// A row with its label at the left and its value in a field with chevrons at
// the right: A, Enter or a click take the next value, left and right go
// through them, a click on the left chevron the one before. The step taken:
// -1, 0 or 1. Not `interactive`: drawn only, never a step (a preview).
int OverlayChoiceRow(const char* id, const char* label, const std::string& value, float width,
                     bool interactive = true);
// A row with its label and a switch at the right: whether it was pressed.
// `about`: a short description under the label (the row grows to fit it).
bool OverlaySwitchRow(const char* id, const char* label, bool on, float width, bool interactive = true,
                      const char* about = nullptr);
// A row with its label and a slider for 0 to 100 % at the right: its new
// value while it changes. A press on the track and a drag set it (`done`
// false until the drag lets go, then once more with `done` true); left and
// right step by 5 (`done` true).
std::optional<int> OverlaySliderRow(const char* id, const char* label, int value, float width, bool& done);
// A row with its label and a segmented choice at the right, as the launcher's
// (one of `options`, `selected` lit): the one picked now, or -1. A click picks
// one; left and right step to the next one (not round), A to the next one
// round.
int OverlaySegmentedRow(const char* id, const char* label, const std::vector<std::string>& options, int selected,
                        float width, bool interactive, const char* about = nullptr);
// A tab's menu of sections (Settings, Game options): a column at the left
// with the section shown beside it, or in one column a row above it. The
// cursor on an item shows its section; right, or A on it, goes into its rows
// (OverlayEnterSection); coming back, the cursor lands on the item shown,
// not on whichever is nearest.
struct OverlaySection
{
    const char* name;
    launcher::Icon icon;
    const char* short_name = nullptr; // in a row, when the name is long
    std::string badge;                // small, at the item's right ("2 on"); empty for none
};
struct OverlaySectionState
{
    int section = 0;            // the section shown
    bool menu_focused = false;  // the cursor was on the menu
    bool enter_section = false; // right or A on its item: the cursor goes to its first row
    bool back_to_menu = false;  // left from a row: the cursor goes to the item shown
};
void OverlaySectionMenu(const std::vector<OverlaySection>& sections, OverlaySectionState& state, float width,
                        bool row);
// Before the section's first row: the cursor onto it, when the menu sent it.
void OverlayEnterSection(OverlaySectionState& state);
// After a row of a section whose rows don't take left and right (switches):
// left on it goes back to the menu's item shown, wherever the items are.
void OverlaySectionRowBack(OverlaySectionState& state);

// A chevron pointing left (-1) or right (+1), centred on `center`.
void OverlayChevron(ImDrawList* draw, ImVec2 center, int direction, ImU32 ink);

// A short text in small muted letters, wrapped to `width`.
void OverlayCaption(const char* text, float width);

// A group's name in small tracked capitals (none when empty), then a card
// that holds its rows (`rows` draws them, one under the other).
void OverlayRowGroupBegin(const char* title, float width);
void OverlayRowGroupEnd(float width);
template <typename Rows> void OverlayRowGroup(const char* title, float width, Rows&& rows)
{
    OverlayRowGroupBegin(title, width);
    rows();
    OverlayRowGroupEnd(width);
}
} // namespace mscharged::overlay
