#pragma once

#include <string>
#include <string_view>
#include <vector>

// Release notes and pull request descriptions (GitHub Markdown), reduced to
// what the launcher shows: headings, paragraphs, list items, code lines and
// rules, with strong, code and link text. Images, HTML and link targets are
// dropped; the full notes stay one click away on GitHub.
namespace mscharged::launcher
{
struct NoteSpan
{
    enum class Style { Plain, Strong, Code, Link };
    Style style = Style::Plain;
    std::string text;
};

struct NoteBlock
{
    enum class Kind { Heading, Paragraph, Item, Code, Rule };
    Kind kind = Kind::Paragraph;
    int level = 0;      // heading level 1-6, or list depth from 0
    std::string marker; // list items: empty for a bullet, else "1." etc.
    std::vector<NoteSpan> spans;
};

std::vector<NoteBlock> ParseReleaseNotes(std::string_view markdown);
// The inline formatting of one line of text.
std::vector<NoteSpan> ParseNoteSpans(std::string_view text);

// Draws the notes at the cursor, wrapped to `width` (launcher UI only).
void DrawReleaseNotes(const std::vector<NoteBlock>& blocks, float width);
} // namespace mscharged::launcher
