#pragma once

#include "overlay/overlay_frame.h"

#include "imgui.h"
#include "imgui_internal.h"

#include <string>

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
