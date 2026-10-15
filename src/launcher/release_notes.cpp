#include "launcher/release_notes.h"

#include <algorithm>
#include <cctype>

namespace mscharged::launcher
{
namespace
{
std::string_view TrimLeft(std::string_view text)
{
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) text.remove_prefix(1);
    return text;
}

std::string_view Trim(std::string_view text)
{
    text = TrimLeft(text);
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) text.remove_suffix(1);
    return text;
}

std::size_t Indent(std::string_view line)
{
    std::size_t width = 0;
    for (const char c : line)
    {
        if (c == ' ') ++width;
        else if (c == '\t') width += 4;
        else break;
    }
    return width;
}

bool IsRule(std::string_view line)
{
    line = Trim(line);
    if (line.size() < 3) return false;
    const char mark = line[0];
    if (mark != '-' && mark != '*' && mark != '_') return false;
    int count = 0;
    for (const char c : line)
    {
        if (c == mark) ++count;
        else if (c != ' ') return false;
    }
    return count >= 3;
}

// An HTML tag at the start of `text` ("<br>", "</details>"), or 0.
std::size_t TagLength(std::string_view text)
{
    if (text.size() < 3 || text[0] != '<') return 0;
    std::size_t at = text[1] == '/' ? 2 : 1;
    if (at >= text.size() || !std::isalpha(static_cast<unsigned char>(text[at]))) return 0;
    const auto end = text.find('>', at);
    return end == std::string_view::npos ? 0 : end + 1;
}

void Append(std::vector<NoteSpan>& spans, NoteSpan::Style style, std::string_view text)
{
    if (text.empty()) return;
    if (!spans.empty() && spans.back().style == style) spans.back().text += text;
    else spans.push_back({style, std::string(text)});
}

void AppendSpans(std::vector<NoteSpan>& spans, std::vector<NoteSpan> more)
{
    for (auto& span : more) Append(spans, span.style, span.text);
}

std::string WithoutComments(std::string_view text)
{
    std::string out;
    for (;;)
    {
        const auto start = text.find("<!--");
        if (start == std::string_view::npos) break;
        out += text.substr(0, start);
        const auto end = text.find("-->", start + 4);
        if (end == std::string_view::npos) return out;
        text.remove_prefix(end + 3);
    }
    out += text;
    return out;
}
} // namespace

std::vector<NoteSpan> ParseNoteSpans(std::string_view text)
{
    std::vector<NoteSpan> spans;
    std::string plain;
    auto flush = [&] {
        Append(spans, NoteSpan::Style::Plain, plain);
        plain.clear();
    };
    for (std::size_t at = 0; at < text.size();)
    {
        const char c = text[at];
        const std::string_view rest = text.substr(at);
        if (c == '\\' && at + 1 < text.size() && std::ispunct(static_cast<unsigned char>(text[at + 1])))
        {
            plain += text[at + 1];
            at += 2;
            continue;
        }
        if (c == '`')
        {
            const auto end = text.find('`', at + 1);
            if (end != std::string_view::npos)
            {
                flush();
                Append(spans, NoteSpan::Style::Code, text.substr(at + 1, end - at - 1));
                at = end + 1;
                continue;
            }
        }
        if ((rest.substr(0, 2) == "**" || rest.substr(0, 2) == "__") && rest.size() > 4)
        {
            const auto end = text.find(rest.substr(0, 2), at + 2);
            if (end != std::string_view::npos && end > at + 2)
            {
                flush();
                for (auto span : ParseNoteSpans(text.substr(at + 2, end - at - 2)))
                    Append(spans, span.style == NoteSpan::Style::Plain ? NoteSpan::Style::Strong : span.style, span.text);
                at = end + 2;
                continue;
            }
        }
        if (c == '!' && rest.substr(0, 2) == "![")
        {
            const auto close = text.find("](", at);
            const auto end = close == std::string_view::npos ? close : text.find(')', close);
            if (end != std::string_view::npos)
            {
                at = end + 1; // images are left out
                continue;
            }
        }
        if (c == '[')
        {
            const auto close = text.find("](", at);
            const auto end = close == std::string_view::npos ? close : text.find(')', close);
            if (end != std::string_view::npos && text.find('\n', at) > end)
            {
                flush();
                for (auto span : ParseNoteSpans(text.substr(at + 1, close - at - 1)))
                    Append(spans, span.style == NoteSpan::Style::Plain ? NoteSpan::Style::Link : span.style, span.text);
                at = end + 1;
                continue;
            }
        }
        if (c == '<')
        {
            // <https://...> autolinks, other HTML tags are dropped.
            if (rest.substr(0, 9) == "<https://" || rest.substr(0, 8) == "<http://")
            {
                const auto end = text.find('>', at);
                if (end != std::string_view::npos)
                {
                    flush();
                    Append(spans, NoteSpan::Style::Link, text.substr(at + 1, end - at - 1));
                    at = end + 1;
                    continue;
                }
            }
            if (const auto length = TagLength(rest))
            {
                at += length;
                continue;
            }
        }
        // *emphasis* is shown as plain text.
        if (c == '*' && at + 1 < text.size() && text[at + 1] != ' ')
        {
            const auto end = text.find('*', at + 1);
            if (end != std::string_view::npos && text[end - 1] != ' ')
            {
                flush();
                AppendSpans(spans, ParseNoteSpans(text.substr(at + 1, end - at - 1)));
                at = end + 1;
                continue;
            }
        }
        plain += c;
        ++at;
    }
    flush();
    return spans;
}

std::vector<NoteBlock> ParseReleaseNotes(std::string_view markdown)
{
    std::string text = WithoutComments(markdown);
    text.erase(std::remove(text.begin(), text.end(), '\r'), text.end());
    std::vector<NoteBlock> blocks;
    std::string pending; // text of the open paragraph or list item
    bool open = false, fenced = false;
    auto close = [&] {
        if (open && !blocks.empty()) blocks.back().spans = ParseNoteSpans(pending);
        if (open && !blocks.empty() && blocks.back().spans.empty()) blocks.pop_back();
        pending.clear();
        open = false;
    };
    std::string_view rest = text;
    while (!rest.empty() || open)
    {
        const auto end = rest.find('\n');
        std::string_view line = rest.substr(0, end);
        const bool last = end == std::string_view::npos;
        rest = last ? std::string_view() : rest.substr(end + 1);
        const std::string_view content = TrimLeft(line);
        if (content.substr(0, 3) == "```" || content.substr(0, 3) == "~~~")
        {
            close();
            fenced = !fenced;
        }
        else if (fenced)
        {
            NoteBlock block;
            block.kind = NoteBlock::Kind::Code;
            block.spans.push_back({NoteSpan::Style::Code, std::string(line)});
            blocks.push_back(std::move(block));
        }
        else if (content.empty())
            close();
        else if (content[0] == '#')
        {
            close();
            std::size_t level = 0;
            while (level < content.size() && content[level] == '#') ++level;
            if (level <= 6 && level < content.size() && content[level] == ' ')
            {
                std::string_view title = Trim(content.substr(level));
                while (!title.empty() && title.back() == '#') title.remove_suffix(1);
                NoteBlock block;
                block.kind = NoteBlock::Kind::Heading;
                block.level = int(level);
                block.spans = ParseNoteSpans(Trim(title));
                if (!block.spans.empty()) blocks.push_back(std::move(block));
            }
            else
            {
                blocks.push_back({});
                pending = std::string(content);
                open = true;
            }
        }
        else if (IsRule(content))
        {
            close();
            blocks.push_back({NoteBlock::Kind::Rule, 0, {}, {}});
        }
        else
        {
            // List items: "- ", "* ", "+ " or "1. " / "1) ".
            std::size_t marker_length = 0;
            std::string marker;
            if ((content[0] == '-' || content[0] == '*' || content[0] == '+') && content.size() > 1 && content[1] == ' ')
                marker_length = 2;
            else
            {
                std::size_t digits = 0;
                while (digits < content.size() && digits < 4 && std::isdigit(static_cast<unsigned char>(content[digits])))
                    ++digits;
                if (digits && digits + 1 < content.size() && (content[digits] == '.' || content[digits] == ')')
                    && content[digits + 1] == ' ')
                {
                    marker = std::string(content.substr(0, digits)) + ".";
                    marker_length = digits + 2;
                }
            }
            std::string_view body = content;
            if (body.substr(0, 2) == "> ") body.remove_prefix(2);
            else if (body == ">") body = {};
            if (marker_length)
            {
                close();
                NoteBlock block;
                block.kind = NoteBlock::Kind::Item;
                block.level = int(std::min<std::size_t>(Indent(line) / 2, 3));
                block.marker = marker;
                blocks.push_back(std::move(block));
                pending = std::string(Trim(content.substr(marker_length)));
                open = true;
            }
            else if (open)
                pending += " " + std::string(Trim(body));
            else
            {
                blocks.push_back({});
                pending = std::string(Trim(body));
                open = true;
            }
        }
        if (last && rest.empty()) close();
    }
    return blocks;
}
} // namespace mscharged::launcher
