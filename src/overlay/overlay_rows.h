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
bool OverlaySwitchRow(const char* id, const char* label, bool on, float width, bool interactive = true);
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
                        float width, bool interactive);
// A chevron pointing left (-1) or right (+1), centred on `center`.
void OverlayChevron(ImDrawList* draw, ImVec2 center, int direction, ImU32 ink);

// A group's name in small tracked capitals, then a card that holds its rows
// (`rows` draws them, one under the other).
void OverlayRowGroupBegin(const char* title, float width);
void OverlayRowGroupEnd(float width);
template <typename Rows> void OverlayRowGroup(const char* title, float width, Rows&& rows)
{
    OverlayRowGroupBegin(title, width);
    rows();
    OverlayRowGroupEnd(width);
}
} // namespace mscharged::overlay
