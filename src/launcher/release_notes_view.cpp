#include "launcher/release_notes_view.h"

#include "launcher/ui_kit.h"

#include <algorithm>

namespace mscharged::launcher
{
namespace
{
ImVec4 SpanColor(const NotePalette& palette, NoteSpan::Style style, bool heading)
{
    switch (style)
    {
    case NoteSpan::Style::Strong: return palette.heading;
    case NoteSpan::Style::Code: return palette.code;
    case NoteSpan::Style::Link: return palette.link;
    case NoteSpan::Style::Plain: break;
    }
    return heading ? palette.heading : palette.body;
}
} // namespace

NotePalette DarkNotePalette()
{
    return {color::text, Mix(color::muted, color::text, 0.5f), Mix(color::info, color::text, 0.25f), color::accent,
            color::muted, color::border};
}

NotePalette LightNotePalette()
{
    return {{0.063f, 0.075f, 0.098f, 1.0f},  // #10131A
            {0.204f, 0.227f, 0.271f, 1.0f},  // #343A45
            {0.094f, 0.290f, 0.588f, 1.0f},  // #184A96
            {0.035f, 0.435f, 0.184f, 1.0f},  // #096F2F, the accent darkened to read on white
            {0.380f, 0.412f, 0.467f, 1.0f},  // #616977
            {0.800f, 0.812f, 0.835f, 1.0f}}; // #CCCFD5
}

void DrawReleaseNotes(const std::vector<NoteBlock>& blocks, float width)
{
    DrawReleaseNotes(blocks, width, DarkNotePalette());
}

void DrawReleaseNotes(const std::vector<NoteBlock>& blocks, float width, const NotePalette& palette)
{
    const auto& fonts = CurrentFonts();
    auto* draw = ImGui::GetWindowDrawList();
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    float y = origin.y;
    bool first = true;
    for (const auto& block : blocks)
    {
        const bool heading = block.kind == NoteBlock::Kind::Heading;
        ImFont* font = heading ? (block.level <= 1 ? fonts.heading : fonts.label) : fonts.body;
        const float size = FontSize(font);
        const float line_height = size * 1.35f;
        if (!first)
            y += heading ? Dp(14) : block.kind == NoteBlock::Kind::Item ? Dp(4) : block.kind == NoteBlock::Kind::Code ? 0.0f : Dp(8);
        first = false;
        if (block.kind == NoteBlock::Kind::Rule)
        {
            draw->AddLine({origin.x, y + Dp(6)}, {origin.x + width, y + Dp(6)}, Col(palette.rule), 1.0f);
            y += Dp(12);
            continue;
        }
        const float indent = block.kind == NoteBlock::Kind::Item ? Dp(20) * float(block.level + 1) : 0.0f;
        const float left = origin.x + indent, right = origin.x + width;
        if (block.kind == NoteBlock::Kind::Item)
        {
            if (block.marker.empty())
                draw->AddCircleFilled({left - Dp(10), y + line_height * 0.5f}, Dp(2.6f), Col(palette.link, 0.85f));
            else
            {
                const ImVec2 mark = TextSize(font, block.marker.c_str());
                DrawLabel(draw, font, {left - Dp(6) - mark.x, y + (line_height - size) * 0.5f}, Col(palette.marker),
                          block.marker.c_str());
            }
        }
        // Words flow across the spans; a line breaks before a word that does
        // not fit (a single word wider than the column overflows it).
        float x = left;
        for (const auto& span : block.spans)
        {
            const ImU32 ink = Col(SpanColor(palette, span.style, heading));
            const std::string& text = span.text;
            std::size_t at = 0;
            while (at < text.size())
            {
                std::size_t end = text.find(' ', at);
                end = end == std::string::npos ? text.size() : end + 1;
                const char* begin = text.c_str() + at;
                const char* stop = text.c_str() + end;
                const char* visible = stop;
                while (visible > begin && visible[-1] == ' ') --visible;
                const float word = font->CalcTextSizeA(size, FLT_MAX, 0.0f, begin, visible).x;
                const float advance = font->CalcTextSizeA(size, FLT_MAX, 0.0f, begin, stop).x;
                if (x > left && x + word > right)
                {
                    x = left;
                    y += line_height;
                    if (begin == visible) { at = end; continue; } // spaces at a line start
                }
                if (visible > begin)
                    draw->AddText(font, size, {x, y + (line_height - size) * 0.5f}, ink, begin, visible);
                x += advance;
                at = end;
            }
        }
        y += line_height;
    }
    ImGui::Dummy({width, std::max(0.0f, y - origin.y)});
}
} // namespace mscharged::launcher
