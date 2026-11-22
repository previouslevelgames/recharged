#pragma once

// Drawing release notes and the server's message (Markdown, release_notes.h)
// in other colours than the launcher's own.

#include "launcher/release_notes.h"

#include "imgui.h"

namespace mscharged::launcher
{
// The colours notes are drawn in.
struct NotePalette
{
    ImVec4 heading; // headings and **bold**
    ImVec4 body;
    ImVec4 code;
    ImVec4 link;
    ImVec4 marker; // list bullets and numbers
    ImVec4 rule;
};
// The launcher's own, on its dark cards.
NotePalette DarkNotePalette();
// For notes on a light card (the server's message).
NotePalette LightNotePalette();

void DrawReleaseNotes(const std::vector<NoteBlock>& blocks, float width, const NotePalette& palette);
} // namespace mscharged::launcher
