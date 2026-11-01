#include "overlay/online_panel.h"

#include "overlay/overlay_rows.h"
#include "platform/competitive_rules.h"

#include "imgui_internal.h"

#include <algorithm>
#include <cfloat>
#include <cmath>

namespace mscharged::overlay
{
namespace
{
namespace color = launcher::color;
using launcher::Col;
using launcher::Dp;
using launcher::DrawIcon;
using launcher::DrawLabel;
using launcher::FontSize;
using launcher::Icon;
using launcher::Mix;
using launcher::TextSize;

ImVec4 ToneColor(Tone tone)
{
    switch (tone)
    {
    case Tone::Good: return color::success;
    case Tone::Busy: return color::warning;
    case Tone::Away: return color::dim;
    case Tone::Bad: return color::danger;
    default: return color::muted;
    }
}

ImVec4 PresenceColor(Presence presence)
{
    switch (presence)
    {
    case Presence::Free: return color::success;
    case Presence::Busy: return color::warning;
    default: return color::dim;
    }
}

// The colour behind a player's initials: their own, as the launcher's lists
// pick it (a golden-ratio walk around the hue circle).
ImVec4 SeedTint(std::int64_t seed)
{
    ImVec4 tint{0, 0, 0, 1};
    const double hue = std::fmod(double(seed < 0 ? -seed : seed) * 0.618034, 1.0);
    ImGui::ColorConvertHSVtoRGB(float(hue), 0.55f, 0.62f, tint.x, tint.y, tint.z);
    return tint;
}

// One frame of the panel.
struct Frame
{
    const OnlinePanelModel& model;
    const OverlayImages& images;
    OverlayScreens& screens;
    OnlinePanelState& state;
    OnlinePanelActions& actions;
    const launcher::Fonts& fonts;
    bool first_row = true; // the first row is the main screen's preferred item
};

// A player's picture: their texture when the host has one, else their
// initials on their colour; a Mii a face on a rounded square instead.
void DrawPicture(const OverlayImages& images, const launcher::Fonts& fonts, ImDrawList* draw, ImVec2 center,
                 float radius, const std::string& image, const std::string& initials, std::int64_t seed, bool mii)
{
    const ImVec2 min{center.x - radius, center.y - radius}, max{center.x + radius, center.y + radius};
    const float rounding = mii ? radius * 0.48f : radius;
    ImTextureID texture = 0;
    if (!image.empty() && images) texture = images(image);
    if (texture)
    {
        draw->AddImageRounded(texture, min, max, {0, 0}, {1, 1}, IM_COL32_WHITE, rounding);
        draw->AddRect(min, max, Col(ImVec4{1, 1, 1, 1}, 0.10f), rounding, 0, Dp(1));
        return;
    }
    if (mii)
    {
        // The twelve favourite colours of the Wii's Miis, a head with a bowl
        // cut, eyes and a smile (the Mii's own face is not known here).
        static constexpr unsigned char kFavorite[12][3] = {
            {184, 64, 48},  {240, 120, 40}, {248, 216, 32}, {128, 200, 40}, {0, 116, 40},    {32, 72, 152},
            {64, 160, 216}, {232, 96, 120}, {112, 44, 168}, {72, 56, 24},   {224, 224, 224}, {24, 24, 20}};
        const auto& rgb = kFavorite[std::size_t(seed < 0 ? -seed : seed) % 12];
        draw->AddRectFilled(min, max, Col(ImVec4{rgb[0] / 255.0f, rgb[1] / 255.0f, rgb[2] / 255.0f, 1}), rounding);
        draw->AddRect(min, max, Col(ImVec4{1, 1, 1, 1}, 0.12f), rounding, 0, Dp(1));
        const ImVec2 head{center.x, center.y + radius * 0.12f};
        const float r = radius * 0.60f;
        draw->AddCircleFilled(head, r, Col(ImVec4{1.0f, 0.84f, 0.67f, 1}), 24);
        const ImU32 hair = Col(ImVec4{0.24f, 0.16f, 0.11f, 1});
        draw->PathArcTo(head, r * 1.06f, IM_PI, 2.0f * IM_PI, 16);
        draw->PathFillConvex(hair);
        draw->AddRectFilled({head.x - r * 1.06f, head.y - r * 0.12f}, {head.x + r * 1.06f, head.y + Dp(0.5f)}, hair);
        const ImU32 ink = Col(ImVec4{0.12f, 0.09f, 0.08f, 1});
        draw->AddCircleFilled({head.x - r * 0.36f, head.y + r * 0.30f}, r * 0.11f, ink);
        draw->AddCircleFilled({head.x + r * 0.36f, head.y + r * 0.30f}, r * 0.11f, ink);
        draw->PathArcTo({head.x, head.y + r * 0.42f}, r * 0.30f, 0.35f * IM_PI, 0.65f * IM_PI, 8);
        draw->PathStroke(ink, 0, std::max(1.0f, r * 0.09f));
        return;
    }
    draw->AddCircleFilled(center, radius, Col(SeedTint(seed)));
    ImFont* font = radius >= Dp(30) ? fonts.button : radius >= Dp(19) ? fonts.heading : fonts.label;
    const float size = radius >= Dp(30) ? FontSize(font) * radius / Dp(24) : FontSize(font);
    const ImVec2 text = font->CalcTextSizeA(size, FLT_MAX, 0.0f, initials.c_str());
    draw->AddText(font, size, {center.x - text.x * 0.5f, center.y - text.y * 0.5f}, Col(color::text), initials.c_str());
}

// The dot of a picture's presence, ringed in the surface it sits on.
void PresenceDot(ImDrawList* draw, ImVec2 center, float radius, Presence presence, const ImVec4& surface)
{
    const float size = std::max(Dp(4), radius * 0.22f);
    const ImVec2 dot{center.x + radius * 0.72f, center.y + radius * 0.72f};
    draw->AddCircleFilled(dot, size + Dp(2), Col(surface));
    draw->AddCircleFilled(dot, size, Col(PresenceColor(presence)));
}

// A small tag after a name ("Mii", "Friend").
float DrawTag(const Frame& frame, ImDrawList* draw, ImVec2 at, const char* text, const ImVec4& tint)
{
    const ImVec2 size = TextSize(frame.fonts.caption, text);
    const float height = Dp(19);
    draw->AddRectFilled({at.x, at.y - height * 0.5f}, {at.x + size.x + Dp(12), at.y + height * 0.5f}, Col(tint, 0.16f),
                        Dp(6));
    DrawLabel(draw, frame.fonts.caption, {at.x + Dp(6), at.y - size.y * 0.5f}, Col(Mix(tint, color::text, 0.25f)),
              text);
    return size.x + Dp(12);
}

// Wrapped caption text at the cursor, `width` wide.
void Caption(const Frame& frame, const std::string& text, const ImVec4& tint, float width)
{
    if (text.empty()) return;
    const ImVec2 at = ImGui::GetCursorScreenPos();
    const float height =
        frame.fonts.caption->CalcTextSizeA(FontSize(frame.fonts.caption), FLT_MAX, width, text.c_str()).y;
    ImGui::GetWindowDrawList()->AddText(frame.fonts.caption, FontSize(frame.fonts.caption), at, Col(tint),
                                        text.c_str(), nullptr, width);
    ImGui::Dummy({width, height});
}

// ------------------------------------------------------------------ main screen

// One of the player card's lower parts, from `min` to `max` (the card's
// whole width): a line over it and, as a `button`, lit as the mouse or the
// cursor comes. Whether it was pressed; `focused` set.
bool CardPart(ImDrawList* draw, const char* id, ImVec2 min, ImVec2 max, bool last, bool button, bool& focused,
              float& hover)
{
    const float pad = Dp(16);
    draw->AddRectFilled({min.x + pad, min.y - Dp(1)}, {max.x - pad, min.y}, Col(color::border, 0.8f));
    focused = false;
    hover = 0.0f;
    if (!button) return false;
    ImGui::SetCursorScreenPos(min);
    const bool pressed = ImGui::InvisibleButton(id, {max.x - min.x, max.y - min.y}, ImGuiButtonFlags_EnableNav);
    const bool hovered = ImGui::IsItemHovered();
    focused = OverlayNavFocused();
    OverlayFocusable();
    hover = launcher::Animate(ImGui::GetID((std::string(id) + "_hover").c_str()), hovered || focused, 12.0f);
    const float rounding = last ? Dp(16) : 0.0f;
    if (hover > 0.01f)
        draw->AddRectFilled(min, max, Col(color::text, 0.04f * hover), rounding,
                            last ? ImDrawFlags_RoundCornersBottom : ImDrawFlags_None);
    return pressed;
}

// The key of the setup screen of a search for a random opponent.
constexpr const char* kSearchSetup = "search";

// Opens the setup for a search for a random opponent: the preset chosen last.
void OpenSearchSetup(Frame& frame, ImGuiID back)
{
    const auto& presets = frame.model.presets;
    auto& state = frame.state;
    state.setup_for = kSearchSetup;
    state.preset = presets.empty() ? 0 : std::clamp(frame.model.chosen_preset, 0, int(presets.size()) - 1);
    state.rules = presets.empty() ? platform::MatchRules{} : presets[std::size_t(state.preset)].rules;
    frame.screens.Push({int(OnlinePanelScreen::Setup), kSearchSetup, "Find an opponent"}, back);
}

// The player: their picture, name and Discord username; under them the
// switch Open to challenges and the search for a random opponent (each a part
// of the card; the whole part is the control).
void DrawYou(Frame& frame, float width)
{
    const auto& model = frame.model;
    const auto& me = model.me;
    const auto& fonts = frame.fonts;
    auto* draw = ImGui::GetWindowDrawList();
    const ImVec2 at = ImGui::GetCursorScreenPos();
    const float pad = Dp(16), radius = Dp(26);
    const auto caption_height = [&](const char* text, float wrap) {
        return fonts.caption->CalcTextSizeA(FontSize(fonts.caption), FLT_MAX, wrap, text).y;
    };

    // The switch for random games.
    const char* label = "Open to challenges";
    const char* line = me.available ? "Players who aren't your friends can see you and challenge you."
                                    : "Only your friends see you.";
    const float track_w = Dp(40), track_h = Dp(23);
    const float wrap = width - 2 * pad - track_w - Dp(14);
    const float available_h =
        me.can_set_available ? Dp(12) + FontSize(fonts.label) + Dp(3) + caption_height(line, wrap) + Dp(14) : 0.0f;

    // Searching for a random opponent: how long, by which rules, Stop; else
    // the way to start one.
    const auto& search = model.search;
    const bool searching = search.active, can_search = !searching && model.can_search;
    std::string search_line;
    if (searching)
    {
        const int seconds = search.seconds;
        search_line = std::to_string(seconds / 60) + (seconds % 60 < 10 ? ":0" : ":") + std::to_string(seconds % 60)
            + "  ·  " + search.rules + "  ·  "
            + (search.asked == 0 ? std::string("nobody asked yet")
                                 : std::to_string(search.asked) + (search.asked == 1 ? " player asked" : " players asked"));
    }
    else if (can_search)
        search_line = "Your rules go to players open to challenges, until one accepts.";
    const float stop_w = Dp(76);
    const float search_wrap = width - 2 * pad - (searching ? stop_w : Dp(20)) - Dp(14);
    const float search_h = searching || can_search
        ? Dp(12) + FontSize(fonts.label) + Dp(3) + caption_height(search_line.c_str(), search_wrap) + Dp(14)
        : 0.0f;

    const float top = pad + radius * 2 + pad;
    const ImVec2 end{at.x + width, at.y + top + available_h + search_h};
    ImVec4 fill = color::field;
    fill.w = 0.62f;
    OverlayCard(draw, at, end, Dp(16), fill, color::border);

    const ImVec2 center{at.x + pad + radius, at.y + pad + radius};
    DrawPicture(frame.images, frame.fonts, draw, center, radius, me.image, me.initials, me.tint_seed, false);
    PresenceDot(draw, center, radius, Presence::Free, Mix(color::field, OverlaySurface(), 0.15f));
    const float left = center.x + radius + Dp(14);
    const float block = FontSize(fonts.heading) + (me.detail.empty() ? 0.0f : Dp(2) + FontSize(fonts.caption));
    draw->PushClipRect({left, at.y}, {end.x - pad, at.y + top}, true);
    DrawLabel(draw, fonts.heading, {left, center.y - block * 0.5f}, Col(color::text), me.name.c_str());
    if (!me.detail.empty())
        DrawLabel(draw, fonts.caption, {left, center.y - block * 0.5f + FontSize(fonts.heading) + Dp(2)},
                  Col(color::muted), me.detail.c_str());
    draw->PopClipRect();

    float y = at.y + top;
    if (me.can_set_available)
    {
        const ImVec2 min{at.x, y}, max{end.x, y + available_h};
        bool focused = false;
        float hover = 0.0f;
        const bool pressed = CardPart(draw, "##available", min, max, search_h == 0.0f, true, focused, hover);
        const float on = launcher::Animate(ImGui::GetID("##available_on"), me.available, 14.0f);
        DrawLabel(draw, fonts.label, {at.x + pad, y + Dp(12)}, Col(color::text), label);
        draw->AddText(fonts.caption, FontSize(fonts.caption), {at.x + pad, y + Dp(12) + FontSize(fonts.label) + Dp(3)},
                      Col(color::muted), line, nullptr, wrap);
        const ImVec2 track{end.x - pad - track_w, y + (available_h - track_h) * 0.5f};
        draw->AddRectFilled(track, {track.x + track_w, track.y + track_h},
                            Col(Mix(Mix(color::border, color::muted, 0.25f * hover), color::accent, on)),
                            track_h * 0.5f);
        draw->AddCircleFilled({track.x + track_h * 0.5f + (track_w - track_h) * on, track.y + track_h * 0.5f},
                              track_h * 0.5f - Dp(3), Col(Mix(color::text, color::accent_ink, on)));
        if (focused) OverlayFocusRing(draw, min, max, search_h == 0.0f ? Dp(16) : Dp(6));
        if (pressed) frame.actions.set_available = !me.available;
        y = max.y;
    }
    if (search_h > 0.0f)
    {
        const ImVec2 min{at.x, y}, max{end.x, y + search_h};
        bool focused = false;
        float hover = 0.0f;
        const bool pressed = CardPart(draw, "##search", min, max, true, true, focused, hover);
        const ImGuiID item = ImGui::GetItemID();
        DrawLabel(draw, fonts.label, {at.x + pad, y + Dp(12)}, Col(searching ? color::accent : color::text),
                  searching ? "Searching for an opponent" : "Find an opponent");
        draw->AddText(fonts.caption, FontSize(fonts.caption), {at.x + pad, y + Dp(12) + FontSize(fonts.label) + Dp(3)},
                      Col(color::muted), search_line.c_str(), nullptr, search_wrap);
        const float middle = y + search_h * 0.5f;
        if (searching)
        {
            // Stop, as a small field at the right; the whole part stops.
            const ImVec2 field_min{end.x - pad - stop_w, middle - Dp(15)}, field_max{end.x - pad, middle + Dp(15)};
            ImVec4 field = Mix(color::field, color::card_hover, 0.35f + 0.4f * hover);
            field.w = 0.9f;
            draw->AddRectFilled(field_min, field_max, Col(field), Dp(15));
            draw->AddRect(field_min, field_max, Col(Mix(color::border, color::danger, 0.6f * hover), 0.9f), Dp(15), 0,
                          Dp(1));
            const ImVec2 size = TextSize(fonts.body, "Stop");
            DrawLabel(draw, fonts.body, {field_min.x + (stop_w - size.x) * 0.5f, middle - size.y * 0.5f},
                      Col(color::text), "Stop");
        }
        else OverlayChevron(draw, {end.x - pad - Dp(6), middle}, 1, Col(Mix(color::muted, color::text, hover)));
        if (focused) OverlayFocusRing(draw, min, max, Dp(16));
        if (pressed)
        {
            if (searching) frame.actions.stop_search = true;
            else OpenSearchSetup(frame, item);
        }
    }
    ImGui::SetCursorScreenPos(at);
    ImGui::Dummy({width, end.y - at.y});
}

// Games won (left, the accent) and lost (right) of a best-of series, as pips.
void DrawPips(ImDrawList* draw, ImVec2 right_top, int need, int won, int lost)
{
    const float r = Dp(3.5f), step = Dp(11), gap = Dp(12);
    const float group = step * float(need - 1) + 2 * r;
    for (int side = 0; side < 2; ++side)
    {
        float x = right_top.x - group * float(2 - side) - gap * float(1 - side) + r;
        const int filled = side == 0 ? won : lost;
        const ImVec4 tint = side == 0 ? color::accent : Mix(color::text, color::muted, 0.3f);
        for (int i = 0; i < need; ++i, x += step)
        {
            const ImVec2 c{x, right_top.y + r};
            if (i < filled) draw->AddCircleFilled(c, r, Col(tint));
            else draw->AddCircle(c, r - Dp(0.5f), Col(color::muted, 0.55f), 0, Dp(1.2f));
        }
    }
}

// The set: who against, where it is, the score, the rules and its steps.
void DrawSet(Frame& frame, float width)
{
    using Stage = OnlinePanelSet::Stage;
    const OnlinePanelSet& set = frame.model.set;
    const auto& fonts = frame.fonts;
    auto* draw = ImGui::GetWindowDrawList();
    const ImVec2 at = ImGui::GetCursorScreenPos();
    const float pad = Dp(16);
    const bool score = set.games > 0;
    const char* kicker = set.stage == Stage::Challenged ? "CHALLENGE"
        : set.stage == Stage::Waiting                   ? "YOUR CHALLENGE"
        : set.stage == Stage::Ended                     ? "LAST SET"
                                                        : "YOUR SET";
    const ImVec4 tint = set.stage == Stage::Challenged ? color::info
        : set.stage == Stage::Ended                    ? ToneColor(set.tone)
                                                       : color::accent;
    // Its height first: the box is drawn under its contents.
    const float radius = Dp(19);
    const float inner = width - 2 * pad;
    const float score_width = score ? Dp(92) : 0.0f;
    const float rules = set.rules.empty()
        ? 0.0f
        : fonts.caption->CalcTextSizeA(FontSize(fonts.caption), FLT_MAX, inner, set.rules.c_str()).y;
    const bool primary = set.can_accept || set.can_ready || set.can_rematch;
    const bool secondary = set.can_decline || set.can_cancel;
    const float buttons = primary || secondary ? Dp(40) : 0.0f;
    const float kicker_h = FontSize(fonts.caption) + Dp(10);
    const float height = pad + kicker_h + radius * 2 + (rules > 0 ? Dp(10) + rules : 0.0f)
        + (buttons > 0 ? Dp(14) + buttons : 0.0f) + pad;
    const ImVec2 end{at.x + width, at.y + height};
    launcher::FillGradient(draw, at, end, Col(Mix(color::field, tint, 0.14f)), Col(Mix(color::field, tint, 0.04f)),
                           Dp(16), true);
    {
        // A sheen over its upper part, and its contour lit from above.
        ImVec4 sheen{1, 1, 1, 0.05f}, clear{1, 1, 1, 0.0f};
        OverlayFade(draw, {at.x + Dp(1), at.y + Dp(1)}, {end.x - Dp(1), at.y + Dp(60)}, sheen, clear, Dp(15),
                    ImDrawFlags_RoundCornersTop);
        ImVec4 edge = tint;
        edge.w = 0.55f;
        OverlayCard(draw, at, end, Dp(16), ImVec4{0, 0, 0, 0}, edge);
    }

    float y = at.y + pad;
    launcher::DrawTracked(draw, fonts.caption, {at.x + pad, y}, Col(tint), kicker, Dp(1.6f));
    y += kicker_h;
    const ImVec2 center{at.x + pad + radius, y + radius};
    DrawPicture(frame.images, frame.fonts, draw, center, radius, set.opponent.image, set.opponent.initials,
                set.opponent.tint_seed, false);
    PresenceDot(draw, center, radius, set.opponent.presence, Mix(color::field, tint, 0.10f));
    const float left = center.x + radius + Dp(12), right = end.x - pad - score_width;
    const float block = FontSize(fonts.label) + Dp(3) + FontSize(fonts.caption);
    draw->PushClipRect({left, y}, {right - Dp(6), y + radius * 2}, true);
    DrawLabel(draw, fonts.label, {left, center.y - block * 0.5f}, Col(color::text), set.title.c_str());
    DrawLabel(draw, fonts.caption, {left, center.y - block * 0.5f + FontSize(fonts.label) + Dp(3)},
              Col(ToneColor(set.tone)), set.status.c_str());
    draw->PopClipRect();
    if (score)
    {
        // "1 - 0" over the pips of the wins each side needs.
        const std::string text = std::to_string(set.won) + "  \xE2\x80\x93  " + std::to_string(set.lost);
        const ImVec2 size = TextSize(fonts.heading, text.c_str());
        const int need = set.games / 2 + 1;
        const float pips = Dp(11) * float(need - 1) * 2 + Dp(7) * 2 + Dp(12);
        const float column = std::max(size.x, pips);
        const float x = end.x - pad - column;
        DrawLabel(draw, fonts.heading, {x + (column - size.x) * 0.5f, center.y - size.y * 0.5f - Dp(6)},
                  Col(color::text), text.c_str());
        DrawPips(draw, {x + (column + pips) * 0.5f, center.y + size.y * 0.5f - Dp(2)}, need, set.won, set.lost);
    }
    y += radius * 2;
    if (rules > 0)
    {
        y += Dp(10);
        draw->AddText(fonts.caption, FontSize(fonts.caption), {at.x + pad, y}, Col(color::muted), set.rules.c_str(),
                      nullptr, inner);
        y += rules;
    }
    if (buttons > 0)
    {
        y += Dp(14);
        const float gap = Dp(10);
        float x = at.x + pad;
        auto& actions = frame.actions;
        if (primary)
        {
            const float w = secondary ? (inner - gap) * 0.58f : inner;
            ImGui::SetCursorScreenPos({x, y});
            const ImVec2 size{w, buttons};
            if (set.can_accept
                && OverlayButton("accept", "Accept", Icon::Check, size, OverlayLook::Primary, true, true))
                actions.accept = true;
            if (set.can_ready
                && OverlayButton("ready", "Ready to play!", Icon::Play, size, OverlayLook::Primary, true, true))
                actions.ready = true;
            if (set.can_rematch
                && OverlayButton("rematch", "Rematch", Icon::Refresh, size, OverlayLook::Primary, true, true))
                actions.rematch = true;
            x += w + gap;
        }
        if (secondary)
        {
            const float w = primary ? end.x - pad - x : std::min(inner, Dp(150));
            ImGui::SetCursorScreenPos({primary ? x : end.x - pad - w, y});
            const ImVec2 size{w, buttons};
            if (set.can_decline)
            {
                if (OverlayButton("decline", "Decline", Icon::Close, size, OverlayLook::Secondary))
                    actions.decline = true;
            }
            else if (OverlayButton("cancel", set.cancel_label.empty() ? "Cancel set" : set.cancel_label.c_str(),
                                   Icon::Close, size, OverlayLook::Ghost))
                actions.cancel = true;
        }
    }
    ImGui::SetCursorScreenPos(at);
    ImGui::Dummy({width, height});
}

// A player as one row: their picture with the dot of their presence, the
// name (Discord's logo, a Mii tag) over the status, and at the right a quick
// Challenge for the mouse and the chevron to their screen. The row is what
// navigation moves through: A or Enter (or a click) opens their screen.
// Opens a challenge's setup for `row`'s player: the preset chosen last, with
// its rules.
void OpenSetup(Frame& frame, const OnlinePanelRow& row, ImGuiID back)
{
    const auto& presets = frame.model.presets;
    auto& state = frame.state;
    state.setup_for = row.key;
    state.preset = presets.empty() ? 0 : std::clamp(frame.model.chosen_preset, 0, int(presets.size()) - 1);
    state.rules = presets.empty() ? platform::MatchRules{} : presets[std::size_t(state.preset)].rules;
    frame.screens.Push({int(OnlinePanelScreen::Setup), row.key, "Challenge " + row.name}, back);
}

void DrawRow(Frame& frame, const OnlinePanelRow& row, float width, bool line)
{
    const auto& fonts = frame.fonts;
    const bool mii = row.kind == OnlinePanelRow::Kind::Mii;
    ImGui::PushID(row.key.c_str());
    auto* draw = ImGui::GetWindowDrawList();
    const ImVec2 start = ImGui::GetCursorScreenPos();
    const float height = Dp(60);
    const ImVec2 end{start.x + width, start.y + height};

    ImGui::SetNextItemAllowOverlap();
    const bool open = ImGui::InvisibleButton("##row", {width, height}, ImGuiButtonFlags_EnableNav);
    const ImGuiID row_id = ImGui::GetItemID();
    const bool hovered = ImGui::IsItemHovered(), focused = OverlayNavFocused();
    OverlayFocusable(frame.first_row);
    frame.first_row = false;
    const float glow = launcher::Animate(ImGui::GetID("##glow"), hovered || focused, 12.0f);
    if (glow > 0.01f) draw->AddRectFilled(start, end, Col(color::card_hover, glow), Dp(12));
    const ImVec4 surface = Mix(OverlaySurface(), color::card_hover, glow);

    const float radius = Dp(20);
    const ImVec2 center{start.x + Dp(12) + radius, start.y + height * 0.5f};
    DrawPicture(frame.images, frame.fonts, draw, center, radius, row.image, row.initials, row.tint_seed, mii);
    PresenceDot(draw, center, radius, row.presence, surface);

    // From the right: the chevron, then Challenge when it fits.
    float edge = end.x - Dp(14);
    launcher::DrawChevron(draw, {edge - Dp(5), center.y}, Dp(11), Col(Mix(color::dim, color::text, glow)));
    edge -= Dp(10) + Dp(14);
    bool challenge = false;
    if (row.can_challenge && width >= Dp(380))
    {
        const char* label = "Challenge";
        const float icon = Dp(14);
        const ImVec2 size{TextSize(fonts.body, label).x + icon + Dp(8) + Dp(26), Dp(32)};
        edge -= size.x;
        const ImVec2 at{edge, center.y - size.y * 0.5f};
        ImGui::SetCursorScreenPos(at);
        challenge = ImGui::InvisibleButton("##challenge", size); // for the mouse; A opens the player
        const bool over = ImGui::IsItemHovered(), held = ImGui::IsItemActive();
        const float lit = launcher::Animate(ImGui::GetID("##challenge_lit"), over, 12.0f);
        const ImVec2 max{at.x + size.x, at.y + size.y};
        draw->AddRectFilled(at, max,
                            Col(held ? Mix(color::accent_dark, color::accent, 0.3f)
                                     : Mix(Mix(color::field, color::accent_dark, 0.6f), color::accent, 0.85f * lit)),
                            size.y * 0.5f);
        draw->AddRect(at, max, Col(color::accent, 0.45f + 0.4f * lit), size.y * 0.5f, 0, Dp(1));
        const ImU32 ink = Col(Mix(color::accent, color::accent_ink, lit));
        const float left = at.x + Dp(13);
        DrawIcon(draw, Icon::Trophy, {left + icon * 0.5f, center.y}, icon, ink);
        DrawLabel(draw, fonts.body, {left + icon + Dp(8), center.y - FontSize(fonts.body) * 0.5f}, ink, label);
        if (over) ImGui::SetTooltip("Challenge %s to a set", row.name.c_str());
        edge -= Dp(10);
    }
    if (challenge) OpenSetup(frame, row, row_id);
    else if (open)
        frame.screens.Push({int(OnlinePanelScreen::Player), row.key, row.name}, row_id);

    // The name with its marks, over the status.
    const float left = center.x + radius + Dp(14);
    const float block = FontSize(fonts.label) + Dp(3) + FontSize(fonts.caption);
    const float top = center.y - block * 0.5f;
    draw->PushClipRect({left, start.y}, {std::max(left, edge - Dp(4)), end.y}, true);
    const ImVec2 name = TextSize(fonts.label, row.name.c_str());
    DrawLabel(draw, fonts.label, {left, top}, Col(color::text), row.name.c_str());
    float after = left + name.x + Dp(8);
    const float mark_y = top + FontSize(fonts.label) * 0.5f;
    if (row.discord)
    {
        const float mark = Dp(15);
        launcher::DrawDiscordMark(draw, {after + mark * 0.5f, mark_y}, mark, Col(color::discord), Col(surface));
        after += mark + Dp(8);
    }
    if (mii) DrawTag(frame, draw, {after, mark_y}, "Mii", color::muted);
    DrawLabel(draw, fonts.caption, {left, top + FontSize(fonts.label) + Dp(3)}, Col(ToneColor(row.tone)),
              row.status.c_str());
    draw->PopClipRect();
    if (line && glow < 0.99f)
        draw->AddRectFilled({left, end.y - Dp(0.5f)}, {end.x - Dp(10), end.y + Dp(0.5f)},
                            Col(color::border, 0.6f * (1.0f - glow)));
    if (focused) OverlayFocusRing(draw, start, end, Dp(12));

    ImGui::SetCursorScreenPos(start);
    ImGui::Dummy({width, height});
    ImGui::PopID();
}

void DrawSection(Frame& frame, const OnlinePanelSection& section, float width)
{
    auto* draw = ImGui::GetWindowDrawList();
    {
        // The heading: its name in tracked capitals and how many.
        const ImVec2 at = ImGui::GetCursorScreenPos();
        const float height = Dp(32), tracking = Dp(1.5f);
        const float y = at.y + height - FontSize(frame.fonts.caption) - Dp(9);
        launcher::DrawTracked(draw, frame.fonts.caption, {at.x + Dp(4), y}, Col(color::dim), section.title.c_str(),
                              tracking);
        if (!section.rows.empty())
        {
            const std::string count = std::to_string(section.rows.size());
            DrawLabel(draw, frame.fonts.caption,
                      {at.x + Dp(4) + launcher::TrackedWidth(frame.fonts.caption, section.title.c_str(), tracking)
                           + Dp(8),
                       y},
                      Col(color::success, 0.85f), count.c_str());
        }
        if (!section.aside.empty())
        {
            // At the right, quiet: how many are offline.
            const ImVec2 text = TextSize(frame.fonts.caption, section.aside.c_str());
            DrawLabel(draw, frame.fonts.caption, {at.x + width - Dp(4) - text.x, y}, Col(color::dim),
                      section.aside.c_str());
        }
        ImGui::Dummy({width, height});
    }
    if (section.rows.empty())
    {
        // A quiet box saying so.
        const ImVec2 at = ImGui::GetCursorScreenPos();
        const float pad = Dp(16), wrap = width - 2 * pad;
        const auto& fonts = frame.fonts;
        const float hint = section.hint.empty()
            ? 0.0f
            : Dp(4) + fonts.caption->CalcTextSizeA(FontSize(fonts.caption), FLT_MAX, wrap, section.hint.c_str()).y;
        const float height = pad + FontSize(fonts.body) + hint + pad;
        draw->AddRect(at, {at.x + width, at.y + height}, Col(color::border, 0.9f), Dp(12), 0, Dp(1));
        DrawLabel(draw, fonts.body, {at.x + pad, at.y + pad}, Col(color::muted), section.empty.c_str());
        if (!section.hint.empty())
            draw->AddText(fonts.caption, FontSize(fonts.caption),
                          {at.x + pad, at.y + pad + FontSize(fonts.body) + Dp(4)}, Col(color::dim),
                          section.hint.c_str(), nullptr, wrap);
        ImGui::Dummy({width, height});
        return;
    }
    for (std::size_t n = 0; n < section.rows.size(); ++n)
        DrawRow(frame, section.rows[n], width, n + 1 < section.rows.size());
}

void DrawNote(const Frame& frame, float width)
{
    if (frame.model.note.empty()) return;
    const ImVec2 at = ImGui::GetCursorScreenPos();
    DrawIcon(ImGui::GetWindowDrawList(), Icon::Info, {at.x + Dp(8), at.y + FontSize(frame.fonts.caption) * 0.5f},
             Dp(14), Col(color::info));
    ImGui::SetCursorScreenPos({at.x + Dp(24), at.y});
    Caption(frame, frame.model.note, color::muted, width - Dp(24));
}

void DrawLists(Frame& frame, float width)
{
    for (std::size_t s = 0; s < frame.model.sections.size(); ++s)
    {
        if (s) ImGui::Dummy({width, Dp(14)});
        DrawSection(frame, frame.model.sections[s], width);
    }
    ImGui::Dummy({width, Dp(12)});
}

// The main screen: the player, their switch and their set at the left, the
// lists at the right; one column on a narrow window.
void DrawMain(Frame& frame, const OverlayLayout& layout)
{
    const float width = layout.max.x - layout.min.x;
    const bool set = frame.model.set.stage != OnlinePanelSet::Stage::None;
    const bool wide = width >= Dp(700);
    OverlayLayoutIs(wide ? 2 : 1);
    if (wide)
    {
        const float left = std::clamp(width * 0.38f, Dp(300), Dp(380)), gap = Dp(32);
        BeginOverlayScroll("##main_you", layout.min, {layout.min.x + left, layout.max.y});
        ImGui::Dummy({left, Dp(20)});
        DrawYou(frame, left);
        if (set)
        {
            ImGui::Dummy({left, Dp(14)});
            DrawSet(frame, left);
        }
        if (!frame.model.note.empty())
        {
            ImGui::Dummy({left, Dp(14)});
            DrawNote(frame, left);
        }
        ImGui::Dummy({left, Dp(16)});
        EndOverlayScroll();
        const float divider = layout.min.x + left + gap * 0.5f;
        ImGui::GetWindowDrawList()->AddRectFilled({divider, layout.min.y + Dp(20)},
                                                  {divider + Dp(1), layout.max.y - Dp(20)}, Col(color::border, 0.7f));
        const ImVec2 lists{layout.min.x + left + gap, layout.min.y};
        BeginOverlayScroll("##main_lists", lists, layout.max);
        ImGui::Dummy({layout.max.x - lists.x, Dp(10)});
        DrawLists(frame, layout.max.x - lists.x);
        EndOverlayScroll();
        return;
    }
    BeginOverlayScroll("##main_column", layout.min, layout.max);
    ImGui::Dummy({width, Dp(18)});
    DrawYou(frame, width);
    if (set)
    {
        ImGui::Dummy({width, Dp(12)});
        DrawSet(frame, width);
    }
    if (!frame.model.note.empty())
    {
        ImGui::Dummy({width, Dp(12)});
        DrawNote(frame, width);
    }
    ImGui::Dummy({width, Dp(10)});
    DrawLists(frame, width);
    EndOverlayScroll();
}

// ------------------------------------------------------------------ player screen

// A player's screen: their picture, status and tags, then Challenge and the
// friend button. Back to the lists after a challenge.
void DrawPlayer(Frame& frame, const OverlayLayout& layout, const OnlinePanelRow* row)
{
    const auto& fonts = frame.fonts;
    const float width = std::min(layout.max.x - layout.min.x, Dp(640));
    const float x = layout.min.x + ((layout.max.x - layout.min.x) - width) * 0.5f;
    BeginOverlayScroll(("##player_" + frame.screens.Top().key).c_str(), {x, layout.min.y}, {x + width, layout.max.y});
    ImGui::Dummy({width, Dp(28)});
    if (!row)
    {
        Caption(frame, "Not online any more.", color::muted, width);
        EndOverlayScroll();
        return;
    }
    auto* draw = ImGui::GetWindowDrawList();
    const bool mii = row->kind == OnlinePanelRow::Kind::Mii;
    {
        // The picture, and beside it the status, the handle and the tags.
        const ImVec2 at = ImGui::GetCursorScreenPos();
        const float radius = Dp(48);
        const ImVec2 center{at.x + radius, at.y + radius};
        DrawPicture(frame.images, frame.fonts, draw, center, radius, row->image, row->initials, row->tint_seed, mii);
        PresenceDot(draw, center, radius, row->presence, OverlaySurface());
        const float left = center.x + radius + Dp(22);
        const float block = FontSize(fonts.heading) + Dp(6) + FontSize(fonts.body) + Dp(12) + Dp(19);
        float y = center.y - block * 0.5f;
        draw->PushClipRect({left, at.y}, {at.x + width, at.y + radius * 2}, true);
        DrawLabel(draw, fonts.heading, {left, y}, Col(ToneColor(row->tone)), row->status.c_str());
        y += FontSize(fonts.heading) + Dp(6);
        DrawLabel(draw, fonts.body, {left, y}, Col(color::muted), row->detail.empty() ? " " : row->detail.c_str());
        y += FontSize(fonts.body) + Dp(12) + Dp(19) * 0.5f;
        float tag = left;
        if (row->discord) tag += DrawTag(frame, draw, {tag, y}, "Discord friend", color::discord) + Dp(8);
        else if (row->is_friend)
            tag += DrawTag(frame, draw, {tag, y}, mii ? "Mii friend" : "Friend", color::accent) + Dp(8);
        if (mii && !row->is_friend) DrawTag(frame, draw, {tag, y}, "Mii", color::muted);
        draw->PopClipRect();
        ImGui::Dummy({width, radius * 2});
    }
    ImGui::Dummy({width, Dp(28)});

    // The steps: Challenge (a Recharged player), and the friend button.
    const float gap = Dp(12), button_h = Dp(46);
    const float button_w = std::min(Dp(260), (width - gap) * 0.5f);
    const ImVec2 at = ImGui::GetCursorScreenPos();
    float bx = at.x;
    auto& actions = frame.actions;
    if (!mii)
    {
        ImGui::SetCursorScreenPos({bx, at.y});
        if (OverlayButton("challenge", "Challenge to a set", Icon::Trophy, {button_w, button_h}, OverlayLook::Primary,
                          row->can_challenge, true))
            OpenSetup(frame, *row, ImGui::GetItemID());
        bx += button_w + gap;
    }
    if (row->can_befriend)
    {
        const char* label = mii ? (row->is_friend ? "Remove Mii friend" : "Add Mii friend")
                                : (row->is_friend ? "Remove from friends" : "Add to friends");
        ImGui::SetCursorScreenPos({bx, at.y});
        if (OverlayButton("friend", label, Icon::Heart, {button_w, button_h}, OverlayLook::Secondary, true, mii))
        {
            if (mii)
            {
                if (row->is_friend) actions.remove_mii_friend = row->mii_profile;
                else actions.add_mii_friend = row->mii_profile;
            }
            else if (row->is_friend) actions.remove_friend = row->player_id;
            else actions.add_friend = row->player_id;
        }
    }
    ImGui::SetCursorScreenPos(at);
    ImGui::Dummy({width, button_h});
    ImGui::Dummy({width, Dp(14)});
    // Why not, or what a challenge plays by.
    if (mii)
        Caption(frame, "Sets are for Recharged players: play this Mii in the game's online menu.", color::muted,
                width);
    else if (!row->can_challenge && !row->blocked.empty())
        Caption(frame, row->blocked + ": no challenge now.", color::warning, width);
    else if (row->can_challenge)
        Caption(frame, "Choose the rules next: a preset, changed as you like for this challenge.", color::muted,
                width);
    ImGui::Dummy({width, Dp(16)});
    EndOverlayScroll();
}

// "3 minutes", "2:30 minutes", "1 minute".
std::string GameLength(int seconds)
{
    if (seconds % 60 == 0) return std::to_string(seconds / 60) + (seconds == 60 ? " minute" : " minutes");
    return std::to_string(seconds / 60) + ":" + (seconds % 60 < 10 ? "0" : "") + std::to_string(seconds % 60)
        + " minutes";
}

// The next of `values` after `current`, `step` places on, round at the ends
// (the first when `current` isn't one of them).
template <typename T> T Next(const std::vector<T>& values, const T& current, int step)
{
    const auto found = std::find(values.begin(), values.end(), current);
    const int n = int(values.size());
    if (found == values.end() || n == 0) return n ? values.front() : current;
    return values[std::size_t((int(found - values.begin()) + step + n) % n)];
}

// A cheat's row: its choices by key, shown by their names.
void CheatRow(const char* id, const char* label, const std::vector<platform::RuleChoice>& choices,
              std::string& key, float width)
{
    std::vector<std::string> keys;
    std::string shown = key;
    for (const auto& choice : choices)
    {
        keys.push_back(choice.key);
        if (key == choice.key) shown = choice.label;
    }
    if (const int step = OverlayChoiceRow(id, label, shown, width)) key = Next(keys, key, step);
}

// A challenge's setup: the preset it starts from, all its rules to change for
// this challenge only, and Send challenge.
void DrawSetup(Frame& frame, const OverlayLayout& layout, const OnlinePanelRow* row, bool search)
{
    const auto& model = frame.model;
    auto& state = frame.state;
    const float width = std::min(layout.max.x - layout.min.x, Dp(640));
    const float x = layout.min.x + ((layout.max.x - layout.min.x) - width) * 0.5f;
    BeginOverlayScroll(("##setup_" + frame.screens.Top().key).c_str(), {x, layout.min.y}, {x + width, layout.max.y});
    ImGui::Dummy({width, Dp(12)});
    if ((!search && !row) || model.presets.empty() || state.setup_for != (search ? kSearchSetup : row->key))
    {
        Caption(frame, !search && !row ? "Not online any more." : "This server has no rules to play sets by yet.",
                color::muted, width);
        EndOverlayScroll();
        return;
    }
    const int count = int(model.presets.size());
    state.preset = std::clamp(state.preset, 0, count - 1);
    const OnlinePanelPreset& preset = model.presets[std::size_t(state.preset)];
    auto& rules = state.rules;

    OverlayRowGroup("PRESET", width, [&] {
        if (const int step = OverlayChoiceRow("preset", "Start from", preset.name, width))
        {
            state.preset = (state.preset + step + count) % count;
            rules = model.presets[std::size_t(state.preset)].rules;
        }
    });
    OverlayRowGroup("THE MATCH", width, [&] {
        if (const int step = OverlayChoiceRow("games", "Series",
                                       rules.games == 1 ? std::string("One game") : "Best of " + std::to_string(rules.games),
                                       width))
            rules.games = Next(std::vector<int>{1, 3, 5, 7}, rules.games, step);
        if (OverlayChoiceRow("limit", "A game ends", rules.limit == "goals" ? "On goals" : "On time", width))
            rules.limit = rules.limit == "goals" ? "time" : "goals";
        if (rules.limit == "goals")
        {
            if (const int step = OverlayChoiceRow("goals", "Goals to win", std::to_string(rules.goals), width))
                rules.goals = Next(std::vector<int>{1, 2, 3, 4, 5, 6, 7, 8, 9, 10}, rules.goals, step);
        }
        else
        {
            std::vector<int> lengths{60, 120, 180, 240, 300};
            if (std::find(lengths.begin(), lengths.end(), rules.time) == lengths.end())
            {
                lengths.push_back(rules.time);
                std::sort(lengths.begin(), lengths.end());
            }
            if (const int step = OverlayChoiceRow("time", "Game length", GameLength(rules.time), width))
                rules.time = Next(lengths, rules.time, step);
        }
        std::vector<std::string> stadiums;
        std::string stadium = rules.stadium;
        for (const auto& choice : platform::StadiumChoices())
        {
            stadiums.push_back(choice.key);
            if (rules.stadium == choice.key) stadium = choice.label;
        }
        if (const int step = OverlayChoiceRow("stadium", "Stadium", stadium, width))
            rules.stadium = Next(stadiums, rules.stadium, step);
    });
    OverlayRowGroup("CHEATS", width, [&] {
        CheatRow("stadium_cheat", "Stadium", platform::StadiumCheatChoices(), rules.stadium_cheat, width);
        CheatRow("player_cheat", "Players", platform::PlayerCheatChoices(), rules.player_cheat, width);
        CheatRow("powerup_cheat", "Power-ups", platform::PowerupCheatChoices(), rules.powerup_cheat, width);
    });
    OverlayRowGroup("GAMEPLAY", width, [&] {
        for (const auto& tweak : platform::RuleTweakChoices())
        {
            const auto found = std::find(rules.tweaks.begin(), rules.tweaks.end(), tweak.key);
            const bool on = found != rules.tweaks.end();
            if (OverlaySwitchRow(tweak.key, tweak.label, on, width))
            {
                if (on) rules.tweaks.erase(found);
                else
                {
                    rules.tweaks.push_back(tweak.key);
                    std::sort(rules.tweaks.begin(), rules.tweaks.end());
                }
            }
        }
    });

    // Send, by the server's rule set itself while nothing changed (servers
    // before 1.2.3 take only theirs).
    ImGui::Dummy({width, Dp(18)});
    const bool changed = !(rules == preset.rules);
    const float button_w = std::min(Dp(260), width);
    ImGui::SetCursorScreenPos({ImGui::GetCursorScreenPos().x + width - button_w, ImGui::GetCursorScreenPos().y});
    const bool can = search ? model.can_search : row->can_challenge;
    if (OverlayButton("send", search ? "Find an opponent" : "Send challenge", search ? Icon::Target : Icon::Trophy,
                      {button_w, Dp(46)}, OverlayLook::Primary, can))
    {
        OnlinePanelChallenge challenge;
        challenge.player = search ? 0 : row->player_id;
        challenge.name = preset.name;
        if (changed && preset.name.size() + 10 <= 48) challenge.name += " (changed)";
        challenge.rules = rules;
        challenge.rule_set = changed ? 0 : preset.rule_set;
        if (search) frame.actions.search = challenge;
        else frame.actions.challenge = challenge;
        frame.actions.set_challenge_rules = preset.name;
        while (!frame.screens.Empty() && frame.screens.Top().id != int(OnlinePanelScreen::Main)
               && frame.screens.Pop()) {}
    }
    ImGui::Dummy({width, Dp(10)});
    const std::string kept = changed ? "Changed for this one only: the preset stays as it is. " : "";
    if (search)
        Caption(frame,
                kept + "Players open to challenges get these rules, one after the other; the first who accepts "
                       "plays you. Your card shows the search, with Stop.",
                color::muted, width);
    else if (!row->can_challenge && !row->blocked.empty())
        Caption(frame, row->blocked + ": no challenge now.", color::warning, width);
    else Caption(frame, kept + row->name + " sees the rules before accepting.", color::muted, width);
    ImGui::Dummy({width, Dp(16)});
    EndOverlayScroll();
}
} // namespace

OverlayScreens::Screen OnlinePanelFirstScreen() { return {int(OnlinePanelScreen::Main), {}, "Online"}; }

void DrawPlayerPicture(ImDrawList* draw, ImVec2 center, float radius, const OnlinePanelRow& row,
                       const OverlayImages& images, const ImVec4& surface)
{
    DrawPicture(images, launcher::CurrentFonts(), draw, center, radius, row.image, row.initials, row.tint_seed,
                row.kind == OnlinePanelRow::Kind::Mii);
    PresenceDot(draw, center, radius, row.presence, surface);
}

std::string OnlinePanelTitle(const OnlinePanelModel& model, const OverlayScreens& screens)
{
    if (screens.Empty() || screens.Top().id == int(OnlinePanelScreen::Main)) return "Online";
    const OnlinePanelRow* row = FindRow(model, screens.Top().key);
    if (screens.Top().id == int(OnlinePanelScreen::Setup))
    {
        if (screens.Top().key == kSearchSetup) return "Find an opponent";
        return row ? "Challenge " + row->name : screens.Top().title;
    }
    return row ? row->name : screens.Top().title;
}

void DrawOnlinePanel(const OnlinePanelModel& model, OverlayScreens& screens, OnlinePanelState& state,
                     OnlinePanelActions& actions, const OverlayImages& images, const OverlayLayout& layout)
{
    if (screens.Empty()) return;
    Frame frame{model, images, screens, state, actions, launcher::CurrentFonts()};
    if (screens.Top().id == int(OnlinePanelScreen::Player))
        DrawPlayer(frame, layout, FindRow(model, screens.Top().key));
    else if (screens.Top().id == int(OnlinePanelScreen::Setup))
        DrawSetup(frame, layout, FindRow(model, screens.Top().key), screens.Top().key == kSearchSetup);
    else DrawMain(frame, layout);
}
} // namespace mscharged::overlay
