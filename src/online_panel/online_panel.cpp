#include "online_panel/online_panel.h"

#include "imgui_internal.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <iterator>
#include <utility>

namespace mscharged::online_panel
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

ImVec2 Add(ImVec2 a, ImVec2 b) { return {a.x + b.x, a.y + b.y}; }

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

// A rounded rectangle from `first` to `second`, top to bottom, alpha
// included (launcher::FillGradient keeps the rectangle's own alpha).
void FillFade(ImDrawList* draw, ImVec2 min, ImVec2 max, const ImVec4& first, const ImVec4& second, float rounding,
              ImDrawFlags flags = 0)
{
    const int start = draw->VtxBuffer.Size;
    draw->AddRectFilled(min, max, IM_COL32_WHITE, rounding, flags);
    const float height = std::max(1.0f, max.y - min.y);
    for (int i = start; i < draw->VtxBuffer.Size; ++i)
    {
        ImDrawVert& vertex = draw->VtxBuffer[i];
        vertex.col = Col(Mix(first, second, (vertex.pos.y - min.y) / height));
    }
}

// One frame's drawing: the style, what the player did, and whether the next
// focusable item takes the keyboard and gamepad cursor (the panel opened).
struct Frame
{
    const OnlinePanelStyle& style;
    const launcher::Fonts& fonts;
    OnlinePanelState& state;
    OnlinePanelActions& actions;
    bool focus_pending = false;
};

// The item just submitted has the keyboard and gamepad cursor, and it shows.
bool NavFocused() { return ImGui::IsItemFocused() && GImGui->NavCursorVisible; }

// The item just submitted takes the cursor when the panel has just opened.
void ClaimFocus(Frame& frame)
{
    if (!frame.focus_pending) return;
    frame.focus_pending = false;
    ImGui::SetFocusID(ImGui::GetItemID(), ImGui::GetCurrentWindow());
    ImGui::SetNavCursorVisible(true);
}

void FocusRing(ImDrawList* draw, ImVec2 min, ImVec2 max, float rounding)
{
    const float grow = Dp(2.5f);
    draw->AddRect({min.x - grow, min.y - grow}, {max.x + grow, max.y + grow}, Col(color::accent, 0.95f),
                  rounding + grow, 0, Dp(2));
}

// A player's picture: their texture when the host has one, else their
// initials on their colour; a Mii a face on a rounded square instead.
void DrawPicture(const Frame& frame, ImDrawList* draw, ImVec2 center, float radius, const std::string& image,
                 const std::string& initials, std::int64_t seed, bool mii, float alpha)
{
    const ImVec2 min{center.x - radius, center.y - radius}, max{center.x + radius, center.y + radius};
    const float rounding = mii ? radius * 0.48f : radius;
    ImTextureID texture = 0;
    if (!image.empty() && frame.style.image) texture = frame.style.image(image);
    if (texture)
    {
        draw->AddImageRounded(texture, min, max, {0, 0}, {1, 1}, Col(ImVec4{1, 1, 1, 1}, alpha), rounding);
        draw->AddRect(min, max, Col(ImVec4{1, 1, 1, 1}, 0.10f * alpha), rounding, 0, Dp(1));
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
        draw->AddRectFilled(min, max, Col(ImVec4{rgb[0] / 255.0f, rgb[1] / 255.0f, rgb[2] / 255.0f, 1}, alpha), rounding);
        draw->AddRect(min, max, Col(ImVec4{1, 1, 1, 1}, 0.12f * alpha), rounding, 0, Dp(1));
        const ImVec2 head{center.x, center.y + radius * 0.12f};
        const float r = radius * 0.60f;
        draw->AddCircleFilled(head, r, Col(ImVec4{1.0f, 0.84f, 0.67f, 1}, alpha), 24);
        const ImU32 hair = Col(ImVec4{0.24f, 0.16f, 0.11f, 1}, alpha);
        draw->PathArcTo(head, r * 1.06f, IM_PI, 2.0f * IM_PI, 16);
        draw->PathFillConvex(hair);
        draw->AddRectFilled({head.x - r * 1.06f, head.y - r * 0.12f}, {head.x + r * 1.06f, head.y + Dp(0.5f)}, hair);
        const ImU32 ink = Col(ImVec4{0.12f, 0.09f, 0.08f, 1}, alpha);
        draw->AddCircleFilled({head.x - r * 0.36f, head.y + r * 0.30f}, r * 0.11f, ink);
        draw->AddCircleFilled({head.x + r * 0.36f, head.y + r * 0.30f}, r * 0.11f, ink);
        draw->PathArcTo({head.x, head.y + r * 0.42f}, r * 0.30f, 0.35f * IM_PI, 0.65f * IM_PI, 8);
        draw->PathStroke(ink, 0, std::max(1.0f, r * 0.09f));
        return;
    }
    draw->AddCircleFilled(center, radius, Col(SeedTint(seed), alpha));
    ImFont* font = radius >= Dp(19) ? frame.fonts.heading : frame.fonts.label;
    const ImVec2 size = TextSize(font, initials.c_str());
    DrawLabel(draw, font, {center.x - size.x * 0.5f, center.y - size.y * 0.5f},
              Col(color::text, 0.65f + 0.35f * alpha), initials.c_str());
}

// The dot of a picture's presence, ringed in the surface it sits on.
void PresenceDot(ImDrawList* draw, ImVec2 center, float radius, Presence presence, const ImVec4& surface)
{
    const ImVec2 dot{center.x + radius * 0.72f, center.y + radius * 0.72f};
    draw->AddCircleFilled(dot, Dp(6), Col(surface));
    draw->AddCircleFilled(dot, Dp(4), Col(PresenceColor(presence)));
}

enum class Look
{
    Primary,   // the accent, filled: the step to take
    Secondary, // a field with a border
    Ghost,     // text until hovered
};

// A button that keyboard and gamepad navigation reach (the launcher's are
// mouse buttons), with the accent ring while it has their cursor.
bool PanelButton(Frame& frame, const char* id, const char* label, Icon icon, ImVec2 size, Look look, bool enabled,
                 bool claim = false)
{
    ImGui::PushID(id);
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    ImGui::BeginDisabled(!enabled);
    const bool pressed = ImGui::InvisibleButton("##button", size, ImGuiButtonFlags_EnableNav);
    const bool hovered = ImGui::IsItemHovered(), held = ImGui::IsItemActive(), focused = NavFocused();
    ImGui::EndDisabled();
    if (claim && enabled) ClaimFocus(frame);
    const float hover = launcher::Animate(ImGui::GetID("##hover"), (hovered || focused) && enabled, 12.0f);
    auto* draw = ImGui::GetWindowDrawList();
    const ImVec2 max = Add(pos, size);
    const float rounding = std::min(size.y * 0.5f, Dp(10));
    ImU32 ink = 0;
    switch (look)
    {
    case Look::Primary:
        if (enabled)
        {
            const float light = held ? -0.10f : 0.12f * hover;
            const ImVec4 a = Mix(color::accent_hi, ImVec4{1, 1, 1, 1}, std::max(0.0f, light));
            const ImVec4 b = Mix(color::accent, light < 0 ? color::accent_dark : ImVec4{1, 1, 1, 1}, std::fabs(light));
            launcher::FillGradient(draw, pos, max, Col(a), Col(b), rounding, true);
            ink = Col(color::accent_ink);
        }
        else
        {
            draw->AddRectFilled(pos, max, Col(color::field), rounding);
            draw->AddRect(pos, max, Col(color::border), rounding, 0, Dp(1));
            ink = Col(color::dim);
        }
        break;
    case Look::Secondary:
        draw->AddRectFilled(pos, max, Col(held ? Mix(color::field, color::accent, 0.18f)
                                               : Mix(color::field, color::border, 0.9f * hover)),
                            rounding);
        draw->AddRect(pos, max, Col(Mix(color::border, color::muted, 0.4f * hover)), rounding, 0, Dp(1));
        ink = Col(enabled ? Mix(color::muted, color::text, 0.6f + 0.4f * hover) : color::dim);
        break;
    case Look::Ghost:
        if (hover > 0.0f || held)
            draw->AddRectFilled(pos, max, Col(color::text, held ? 0.10f : 0.07f * hover), rounding);
        ink = Col(enabled ? Mix(color::muted, color::text, 0.35f + 0.65f * hover) : color::dim);
        break;
    }
    ImFont* font = look == Look::Primary ? frame.fonts.label : frame.fonts.body;
    const ImVec2 text = TextSize(font, label);
    const float icon_size = icon == Icon::None ? 0.0f : FontSize(font) * 0.9f;
    const float gap = icon == Icon::None || !*label ? 0.0f : Dp(8);
    const float left = pos.x + (size.x - (text.x + icon_size + gap)) * 0.5f;
    if (icon != Icon::None) DrawIcon(draw, icon, {left + icon_size * 0.5f, pos.y + size.y * 0.5f}, icon_size, ink);
    DrawLabel(draw, font, {left + icon_size + gap, pos.y + (size.y - text.y) * 0.5f}, ink, label);
    if (focused) FocusRing(draw, pos, max, rounding);
    ImGui::PopID();
    return pressed && enabled;
}

// A round icon button (close, heart), reached by navigation too.
bool IconButton(const char* id, Icon icon, float size, const ImVec4& tint, bool lit, const char* tip)
{
    ImGui::PushID(id);
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const bool pressed = ImGui::InvisibleButton("##icon", {size, size}, ImGuiButtonFlags_EnableNav);
    const bool hovered = ImGui::IsItemHovered(), focused = NavFocused();
    const float hover = launcher::Animate(ImGui::GetID("##hover"), hovered || focused, 12.0f);
    auto* draw = ImGui::GetWindowDrawList();
    const ImVec2 middle{pos.x + size * 0.5f, pos.y + size * 0.5f};
    if (lit || hover > 0.0f) draw->AddCircleFilled(middle, size * 0.5f, Col(tint, (lit ? 0.10f : 0.0f) + 0.10f * hover));
    DrawIcon(draw, icon, middle, size * 0.47f, Col(Mix(tint, color::text, 0.25f * hover)));
    if (focused) draw->AddCircle(middle, size * 0.5f + Dp(2.5f), Col(color::accent, 0.95f), 0, Dp(2));
    if (hovered && tip) ImGui::SetTooltip("%s", tip);
    ImGui::PopID();
    return pressed;
}

// A key's cap, for the hint at the bottom.
float KeyCapWidth(const Frame& frame, const char* key)
{
    return std::max(Dp(26), TextSize(frame.fonts.caption, key).x + Dp(14));
}

void DrawKeyCap(const Frame& frame, ImDrawList* draw, ImVec2 pos, const char* key)
{
    const ImVec2 text = TextSize(frame.fonts.caption, key);
    const ImVec2 max{pos.x + KeyCapWidth(frame, key), pos.y + Dp(22)};
    draw->AddRectFilled(pos, max, Col(color::field), Dp(6));
    draw->AddRect(pos, max, Col(color::border), Dp(6), 0, Dp(1));
    draw->AddRectFilled({pos.x + Dp(1), max.y - Dp(3)}, {max.x - Dp(1), max.y - Dp(1)}, Col(color::border, 0.6f), Dp(4));
    DrawLabel(draw, frame.fonts.caption, {pos.x + (max.x - pos.x - text.x) * 0.5f, pos.y + (Dp(20) - text.y) * 0.5f},
              Col(color::text), key);
}

// The player, and their switch for random games.
float DrawHeader(Frame& frame, const OnlinePanelModel& model, ImVec2 at, float width, ImGuiID& close_id)
{
    const auto& fonts = frame.fonts;
    auto* draw = ImGui::GetWindowDrawList();
    // ONLINE, and the close button at the right.
    launcher::DrawTracked(draw, fonts.caption, {at.x, at.y + Dp(9)}, Col(color::accent), "ONLINE", Dp(2.2f));
    const float close = Dp(32);
    ImGui::SetCursorScreenPos({at.x + width - close + Dp(6), at.y});
    if (IconButton("close", Icon::Close, close, color::muted, false, nullptr)) frame.actions.close = true;
    close_id = ImGui::GetItemID();
    float y = at.y + close + Dp(8);

    // The player: their picture, name and Discord username.
    const float radius = Dp(22);
    const ImVec2 center{at.x + radius, y + radius};
    DrawPicture(frame, draw, center, radius, model.me.image, model.me.initials, model.me.tint_seed, false, 1.0f);
    PresenceDot(draw, center, radius, Presence::Free, Mix(color::window, color::card, 0.4f));
    const float left = center.x + radius + Dp(14);
    const float name_height = FontSize(fonts.heading);
    const float block = name_height + (model.me.detail.empty() ? 0.0f : Dp(2) + FontSize(fonts.caption));
    draw->PushClipRect({left, y}, {at.x + width, y + radius * 2}, true);
    DrawLabel(draw, fonts.heading, {left, center.y - block * 0.5f}, Col(color::text), model.me.name.c_str());
    if (!model.me.detail.empty())
        DrawLabel(draw, fonts.caption, {left, center.y - block * 0.5f + name_height + Dp(2)}, Col(color::muted),
                  model.me.detail.c_str());
    draw->PopClipRect();
    y += radius * 2 + Dp(14);

    if (model.me.can_set_available)
    {
        // Available for random games: the whole box is the switch.
        const char* label = "Available for random games";
        const char* line = model.me.available ? "Others looking for a game can challenge you."
                                              : "Find others looking for a game.";
        const float pad = Dp(12), track_w = Dp(38), track_h = Dp(22);
        const float wrap = width - 2 * pad - track_w - Dp(12);
        const float text = fonts.caption->CalcTextSizeA(FontSize(fonts.caption), FLT_MAX, wrap, line).y;
        const float height = pad + FontSize(fonts.label) + Dp(2) + text + pad;
        ImGui::SetCursorScreenPos({at.x, y});
        const bool pressed = ImGui::InvisibleButton("##available", {width, height}, ImGuiButtonFlags_EnableNav);
        const bool hovered = ImGui::IsItemHovered(), focused = NavFocused();
        const float hover = launcher::Animate(ImGui::GetID("##available_hover"), hovered || focused, 12.0f);
        const float on = launcher::Animate(ImGui::GetID("##available_on"), model.me.available, 14.0f);
        const ImVec2 end{at.x + width, y + height};
        draw->AddRectFilled({at.x, y}, end, Col(Mix(color::field, color::border, 0.35f * hover)), Dp(12));
        draw->AddRect({at.x, y}, end, Col(Mix(color::border, color::accent, 0.35f * on)), Dp(12), 0, Dp(1));
        DrawLabel(draw, fonts.label, {at.x + pad, y + pad}, Col(color::text), label);
        draw->AddText(fonts.caption, FontSize(fonts.caption), {at.x + pad, y + pad + FontSize(fonts.label) + Dp(2)},
                      Col(color::muted), line, nullptr, wrap);
        // The switch.
        const ImVec2 track{end.x - pad - track_w, y + (height - track_h) * 0.5f};
        draw->AddRectFilled(track, {track.x + track_w, track.y + track_h},
                            Col(Mix(Mix(color::border, color::muted, 0.25f * hover), color::accent, on)), track_h * 0.5f);
        const float knob = track_h * 0.5f - Dp(3);
        draw->AddCircleFilled({track.x + track_h * 0.5f + (track_w - track_h) * on, track.y + track_h * 0.5f}, knob,
                              Col(Mix(color::text, color::accent_ink, on)));
        if (focused) FocusRing(draw, {at.x, y}, end, Dp(12));
        if (pressed) frame.actions.set_available = !model.me.available;
        y += height + Dp(12);
    }
    return y - at.y;
}

// Games won (left, the accent) and lost (right) of a best-of series, as pips.
void DrawPips(ImDrawList* draw, ImVec2 right_top, int need, int won, int lost)
{
    const float r = Dp(3.5f), step = Dp(11), gap = Dp(12);
    const float group = step * float(need - 1) + 2 * r;
    float x = right_top.x - group * 2 - gap + r;
    for (int i = 0; i < need; ++i, x += step)
    {
        const ImVec2 c{x, right_top.y + r};
        if (i < won) draw->AddCircleFilled(c, r, Col(color::accent));
        else draw->AddCircle(c, r - Dp(0.5f), Col(color::muted, 0.55f), 0, Dp(1.2f));
    }
    x = right_top.x - group + r;
    for (int i = 0; i < need; ++i, x += step)
    {
        const ImVec2 c{x, right_top.y + r};
        if (i < lost) draw->AddCircleFilled(c, r, Col(Mix(color::text, color::muted, 0.3f)));
        else draw->AddCircle(c, r - Dp(0.5f), Col(color::muted, 0.55f), 0, Dp(1.2f));
    }
}

// The set: who against, where it is, the score, the rules and its steps.
float DrawSet(Frame& frame, const OnlinePanelSet& set, ImVec2 at, float width)
{
    using Stage = OnlinePanelSet::Stage;
    const auto& fonts = frame.fonts;
    auto* draw = ImGui::GetWindowDrawList();
    const float pad = Dp(14);
    const bool score = set.games > 0;
    const char* kicker = set.stage == Stage::Challenged ? "CHALLENGE"
        : set.stage == Stage::Waiting                   ? "YOUR CHALLENGE"
        : set.stage == Stage::Ended                     ? "LAST SET"
                                                        : "YOUR SET";
    const ImVec4 tint = set.stage == Stage::Challenged ? color::info
        : set.stage == Stage::Ended                    ? ToneColor(set.tone)
                                                       : color::accent;
    // Its height first: the box is drawn under its contents.
    const float radius = Dp(18);
    const float inner = width - 2 * pad;
    const float score_width = score ? Dp(96) : 0.0f;
    const float rules = set.rules.empty()
        ? 0.0f
        : fonts.caption->CalcTextSizeA(FontSize(fonts.caption), FLT_MAX, inner, set.rules.c_str()).y;
    const bool primary = set.can_accept || set.can_ready || set.can_rematch;
    const bool secondary = set.can_decline || set.can_cancel;
    const float buttons = primary || secondary ? Dp(38) : 0.0f;
    const float kicker_h = FontSize(fonts.caption) + Dp(10);
    const float height = pad + kicker_h + radius * 2 + (rules > 0 ? Dp(10) + rules : 0.0f)
        + (buttons > 0 ? Dp(12) + buttons : 0.0f) + pad;
    const ImVec2 end{at.x + width, at.y + height};
    launcher::FillGradient(draw, at, end, Col(Mix(color::field, tint, 0.13f)), Col(Mix(color::field, tint, 0.04f)),
                           Dp(14), true);
    draw->AddRect(at, end, Col(tint, 0.45f), Dp(14), 0, Dp(1));

    float y = at.y + pad;
    launcher::DrawTracked(draw, fonts.caption, {at.x + pad, y}, Col(tint), kicker, Dp(1.6f));
    y += kicker_h;
    const ImVec2 center{at.x + pad + radius, y + radius};
    DrawPicture(frame, draw, center, radius, set.opponent.image, set.opponent.initials, set.opponent.tint_seed, false,
                1.0f);
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
        y += Dp(12);
        const float gap = Dp(10);
        float x = at.x + pad;
        if (primary)
        {
            const float w = secondary ? (inner - gap) * 0.58f : inner;
            ImGui::SetCursorScreenPos({x, y});
            if (set.can_accept && PanelButton(frame, "accept", "Accept", Icon::Check, {w, buttons}, Look::Primary, true, true))
                frame.actions.accept = true;
            if (set.can_ready
                && PanelButton(frame, "ready", "Ready to play!", Icon::Play, {w, buttons}, Look::Primary, true, true))
                frame.actions.ready = true;
            if (set.can_rematch
                && PanelButton(frame, "rematch", "Rematch", Icon::Refresh, {w, buttons}, Look::Primary, true, true))
                frame.actions.rematch = true;
            x += w + gap;
        }
        if (secondary)
        {
            const float w = primary ? end.x - pad - x : std::min(inner, Dp(150));
            ImGui::SetCursorScreenPos({primary ? x : end.x - pad - w, y});
            if (set.can_decline && PanelButton(frame, "decline", "Decline", Icon::Close, {w, buttons}, Look::Secondary, true))
                frame.actions.decline = true;
            else if (set.can_cancel && !set.can_decline
                     && PanelButton(frame, "cancel", set.cancel_label.empty() ? "Cancel set" : set.cancel_label.c_str(),
                                    Icon::Close, {w, buttons}, Look::Ghost, true))
                frame.actions.cancel = true;
        }
    }
    ImGui::SetCursorScreenPos(at);
    ImGui::Dummy({width, height});
    return height;
}

// A small tag after a name ("Mii").
float DrawTag(const Frame& frame, ImDrawList* draw, ImVec2 at, const char* text, float alpha)
{
    const ImVec2 size = TextSize(frame.fonts.caption, text);
    const float height = Dp(17);
    draw->AddRectFilled({at.x, at.y - height * 0.5f}, {at.x + size.x + Dp(10), at.y + height * 0.5f},
                        Col(color::border, 0.9f * alpha), Dp(5));
    DrawLabel(draw, frame.fonts.caption, {at.x + Dp(5), at.y - size.y * 0.5f}, Col(color::muted, alpha), text);
    return size.x + Dp(10);
}

// A player as one row: their picture with the dot of their presence, the
// name (Discord's logo, a Mii tag, unread messages) over the status, and at
// the right Challenge and the heart. The row itself is what navigation
// moves through: A (or Enter) on it challenges, right goes to its heart.
void DrawRow(Frame& frame, const OnlinePanelRow& row, float width, bool line)
{
    const auto& fonts = frame.fonts;
    const bool online = row.presence != Presence::Offline;
    const bool mii = row.kind == OnlinePanelRow::Kind::Mii;
    ImGui::PushID(row.key.c_str());
    auto* draw = ImGui::GetWindowDrawList();
    const ImVec2 start = ImGui::GetCursorScreenPos();
    const float height = online ? Dp(54) : Dp(48);
    const ImVec2 end{start.x + width, start.y + height};

    ImGui::SetNextItemAllowOverlap();
    const bool row_pressed = ImGui::InvisibleButton("##row", {width, height}, ImGuiButtonFlags_EnableNav);
    const bool row_hovered = ImGui::IsItemHovered(), row_focused = NavFocused();
    ClaimFocus(frame);
    const float glow = launcher::Animate(ImGui::GetID("##glow"), row_hovered || row_focused, 12.0f);
    if (glow > 0.01f) draw->AddRectFilled(start, end, Col(color::card_hover, glow), Dp(12));
    const ImVec4 surface = Mix(Mix(color::window, color::card, 0.4f), color::card_hover, glow);

    const float radius = online ? Dp(18) : Dp(16);
    const ImVec2 center{start.x + Dp(10) + radius, start.y + height * 0.5f};
    const float alpha = online ? 1.0f : 0.5f;
    DrawPicture(frame, draw, center, radius, row.image, row.initials, row.tint_seed, mii, alpha);
    if (online || !mii) PresenceDot(draw, center, radius, row.presence, surface);

    // From the right: the heart, then Challenge.
    float edge = end.x - Dp(8);
    if (row.can_befriend)
    {
        const float size = Dp(32);
        edge -= size;
        ImGui::SetCursorScreenPos({edge, center.y - size * 0.5f});
        const std::string tip = (row.is_friend ? "Remove " + row.name + " from your friends"
                                               : "Add " + row.name + " to your friends");
        const ImVec4 heart = row.is_friend ? color::accent : color::dim;
        if (IconButton("heart", Icon::Heart, size, heart, row.is_friend, tip.c_str()))
        {
            if (mii)
            {
                if (row.is_friend) frame.actions.remove_mii_friend = row.mii_profile;
                else frame.actions.add_mii_friend = row.mii_profile;
            }
            else if (row.is_friend) frame.actions.remove_friend = row.player_id;
            else frame.actions.add_friend = row.player_id;
        }
        edge -= Dp(6);
    }
    if (row.can_challenge)
    {
        // Lit while the row has the cursor: A challenges then. For the mouse
        // only; navigation stops on the row.
        const char* label = "Challenge";
        const float icon = Dp(14);
        const ImVec2 size{TextSize(fonts.body, label).x + icon + Dp(8) + Dp(24), Dp(30)};
        edge -= size.x;
        const ImVec2 at{edge, center.y - size.y * 0.5f};
        ImGui::SetCursorScreenPos(at);
        const bool pressed = ImGui::InvisibleButton("##challenge", size);
        const bool hovered = ImGui::IsItemHovered(), held = ImGui::IsItemActive();
        const float lit = launcher::Animate(ImGui::GetID("##challenge_lit"), hovered || row_focused, 12.0f);
        const ImVec2 max = Add(at, size);
        draw->AddRectFilled(at, max,
                            Col(held ? Mix(color::accent_dark, color::accent, 0.3f)
                                     : Mix(Mix(color::field, color::accent_dark, 0.6f), color::accent, 0.85f * lit)),
                            size.y * 0.5f);
        draw->AddRect(at, max, Col(color::accent, 0.45f + 0.4f * lit), size.y * 0.5f, 0, Dp(1));
        const ImU32 ink = Col(Mix(color::accent, color::accent_ink, lit));
        const float left = at.x + Dp(12);
        DrawIcon(draw, Icon::Trophy, {left + icon * 0.5f, center.y}, icon, ink);
        DrawLabel(draw, fonts.body, {left + icon + Dp(8), center.y - FontSize(fonts.body) * 0.5f}, ink, label);
        if (hovered) ImGui::SetTooltip("Challenge %s to a set", row.name.c_str());
        if (pressed) frame.actions.challenge = row.player_id;
        edge -= Dp(8);
    }
    // A (or Enter) on the row: the challenge. A click on it does nothing.
    if (row_pressed && row.can_challenge && !ImGui::IsMouseReleased(ImGuiMouseButton_Left))
        frame.actions.challenge = row.player_id;
    if (row_hovered && !row.can_challenge && !row.blocked.empty() && online)
        ImGui::SetTooltip("%s: no challenge now.", row.blocked.c_str());

    // The name with its marks, over the status.
    const float left = center.x + radius + Dp(13);
    const float block = FontSize(fonts.label) + Dp(2) + FontSize(fonts.caption);
    const float top = center.y - block * 0.5f;
    draw->PushClipRect({left, start.y}, {std::max(left, edge - Dp(4)), end.y}, true);
    const ImVec2 name = TextSize(fonts.label, row.name.c_str());
    DrawLabel(draw, fonts.label, {left, top}, Col(online ? color::text : Mix(color::text, color::muted, 0.45f)),
              row.name.c_str());
    float after = left + name.x + Dp(8);
    const float mark_y = top + FontSize(fonts.label) * 0.5f;
    if (row.discord)
    {
        const float mark = Dp(14);
        launcher::DrawDiscordMark(draw, {after + mark * 0.5f, mark_y}, mark, Col(color::discord, online ? 1.0f : 0.55f),
                                  Col(surface));
        after += mark + Dp(8);
    }
    if (mii) after += DrawTag(frame, draw, {after, mark_y}, "Mii", online ? 1.0f : 0.7f) + Dp(8);
    if (row.unread > 0)
    {
        const std::string count = row.unread > 9 ? "9+" : std::to_string(row.unread);
        const ImVec2 text = TextSize(fonts.caption, count.c_str());
        const float w = std::max(Dp(17), text.x + Dp(9)), h = Dp(17);
        draw->AddRectFilled({after, mark_y - h * 0.5f}, {after + w, mark_y + h * 0.5f}, Col(color::accent), h * 0.5f);
        DrawLabel(draw, fonts.caption, {after + (w - text.x) * 0.5f, mark_y - text.y * 0.5f}, Col(color::accent_ink),
                  count.c_str());
    }
    const float status_y = top + FontSize(fonts.label) + Dp(2);
    DrawLabel(draw, fonts.caption, {left, status_y}, Col(ToneColor(row.tone)), row.status.c_str());
    if (!row.detail.empty())
    {
        const float x = left + TextSize(fonts.caption, row.status.c_str()).x;
        const std::string detail = "  \xC2\xB7  " + row.detail;
        DrawLabel(draw, fonts.caption, {x, status_y}, Col(color::dim), detail.c_str());
    }
    draw->PopClipRect();
    if (line && glow < 0.99f)
        draw->AddRectFilled({left, end.y - Dp(0.5f)}, {end.x - Dp(8), end.y + Dp(0.5f)},
                            Col(color::border, 0.6f * (1.0f - glow)));
    if (row_focused) FocusRing(draw, start, end, Dp(12));

    ImGui::SetCursorScreenPos(start);
    ImGui::Dummy({width, height});
    ImGui::PopID();
}

// A list's heading: its name in tracked capitals, and how many are online.
void DrawHeading(const Frame& frame, const OnlinePanelSection& section, float width)
{
    auto* draw = ImGui::GetWindowDrawList();
    const ImVec2 at = ImGui::GetCursorScreenPos();
    const float height = Dp(30), tracking = Dp(1.4f);
    const float y = at.y + height - FontSize(frame.fonts.caption) - Dp(8);
    launcher::DrawTracked(draw, frame.fonts.caption, {at.x + Dp(4), y}, Col(color::dim), section.title.c_str(), tracking);
    const bool offline = section.online < section.rows.size();
    const std::string count = offline ? std::to_string(section.online) + " online" : std::to_string(section.rows.size());
    DrawLabel(draw, frame.fonts.caption,
              {at.x + Dp(4) + launcher::TrackedWidth(frame.fonts.caption, section.title.c_str(), tracking) + Dp(8), y},
              Col(section.online > 0 ? color::success : color::muted, section.online > 0 ? 0.85f : 1.0f),
              count.c_str());
    ImGui::Dummy({width, height});
}

// The fold of the friends offline: a row with a chevron.
void DrawFold(Frame& frame, std::size_t count, float width)
{
    auto* draw = ImGui::GetWindowDrawList();
    const ImVec2 at = ImGui::GetCursorScreenPos();
    const float height = Dp(36);
    const bool pressed = ImGui::InvisibleButton("##offline_fold", {width, height}, ImGuiButtonFlags_EnableNav);
    const bool hovered = ImGui::IsItemHovered(), focused = NavFocused();
    ClaimFocus(frame);
    const float glow = launcher::Animate(ImGui::GetID("##fold_glow"), hovered || focused, 12.0f);
    if (glow > 0.01f) draw->AddRectFilled(at, {at.x + width, at.y + height}, Col(color::card_hover, glow), Dp(10));
    const float chevron = Dp(10);
    const ImVec2 mark{at.x + Dp(14) + chevron * 0.5f, at.y + height * 0.5f};
    const ImU32 ink = Col(Mix(color::muted, color::text, glow));
    if (frame.state.offline_open) launcher::DrawChevronDown(draw, mark, chevron, ink);
    else launcher::DrawChevron(draw, mark, chevron, ink);
    const std::string label = (frame.state.offline_open ? "Hide offline friends (" : "Show offline friends (")
        + std::to_string(count) + ")";
    DrawLabel(draw, frame.fonts.body, {at.x + Dp(34), at.y + (height - FontSize(frame.fonts.body)) * 0.5f}, ink,
              label.c_str());
    if (focused) FocusRing(draw, at, {at.x + width, at.y + height}, Dp(10));
    if (pressed) frame.state.offline_open = !frame.state.offline_open;
}

void DrawSection(Frame& frame, const OnlinePanelSection& section, float width)
{
    DrawHeading(frame, section, width);
    if (section.rows.empty())
    {
        auto* draw = ImGui::GetWindowDrawList();
        const ImVec2 at = ImGui::GetCursorScreenPos();
        const float wrap = width - Dp(8);
        const float h = frame.fonts.caption->CalcTextSizeA(FontSize(frame.fonts.caption), FLT_MAX, wrap,
                                                           section.empty.c_str()).y;
        draw->AddText(frame.fonts.caption, FontSize(frame.fonts.caption), {at.x + Dp(4), at.y}, Col(color::muted),
                      section.empty.c_str(), nullptr, wrap);
        ImGui::Dummy({width, h + Dp(4)});
        return;
    }
    // The friends' offline rows fold away under the online ones.
    const bool fold = section.id == OnlinePanelSection::Id::Friends && section.online < section.rows.size();
    const std::size_t shown = fold ? section.online : section.rows.size();
    for (std::size_t n = 0; n < shown; ++n) DrawRow(frame, section.rows[n], width, n + 1 < shown);
    if (fold)
    {
        if (section.online > 0) ImGui::Dummy({width, Dp(4)});
        DrawFold(frame, section.rows.size() - section.online, width);
        if (frame.state.offline_open)
            for (std::size_t n = section.online; n < section.rows.size(); ++n)
                DrawRow(frame, section.rows[n], width, n + 1 < section.rows.size());
    }
}

// Each font where none was given: the body font, or ImGui's.
launcher::Fonts Complete(launcher::Fonts fonts)
{
    if (!fonts.body) fonts.body = ImGui::GetFont();
    for (ImFont** font : {&fonts.caption, &fonts.label, &fonts.heading, &fonts.title, &fonts.button, &fonts.overline})
        if (!*font) *font = fonts.body;
    return fonts;
}
} // namespace

void DrawOnlinePanel(const OnlinePanelModel& model, OnlinePanelState& state, OnlinePanelActions& actions,
                     const OnlinePanelStyle& style)
{
    // The launcher's widgets read their scale and fonts from ui_kit: the
    // panel's for this call, the caller's back after it.
    const float saved_unit = launcher::UiScale();
    const launcher::Fonts saved_fonts = launcher::CurrentFonts();
    const launcher::Fonts fonts = Complete(style.fonts);
    launcher::SetUiScale(style.unit);
    launcher::SetCurrentFonts(fonts);
    Frame frame{style, fonts, state, actions, state.focus};

    const ImGuiIO& io = ImGui::GetIO();
    ImVec2 area_min = style.area_min, area_max = style.area_max;
    if (area_max.x <= area_min.x || area_max.y <= area_min.y)
    {
        area_min = {0, 0};
        area_max = io.DisplaySize;
    }
    const float area_w = area_max.x - area_min.x, area_h = area_max.y - area_min.y;
    const float appear = std::clamp(style.appear, 0.0f, 1.0f);
    const float eased = 1.0f - (1.0f - appear) * (1.0f - appear) * (1.0f - appear);

    ImGui::SetNextWindowPos(area_min);
    ImGui::SetNextWindowSize({area_w, area_h});
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0, 0});
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {0, 0});
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarSize, Dp(6));
    ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarRounding, Dp(3));
    ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, Dp(8));
    ImGui::PushStyleVar(ImGuiStyleVar_PopupBorderSize, 1.0f);
    constexpr int style_vars = 9;
    const std::pair<ImGuiCol, ImVec4> colors[] = {
        {ImGuiCol_WindowBg, {0, 0, 0, 0}},
        {ImGuiCol_ChildBg, {0, 0, 0, 0}},
        {ImGuiCol_NavCursor, {0, 0, 0, 0}}, // the panel draws its own ring
        {ImGuiCol_ScrollbarBg, {0, 0, 0, 0}},
        {ImGuiCol_ScrollbarGrab, color::border},
        {ImGuiCol_ScrollbarGrabHovered, Mix(color::border, color::muted, 0.4f)},
        {ImGuiCol_ScrollbarGrabActive, color::muted},
        {ImGuiCol_PopupBg, Mix(color::card, color::field, 0.5f)},
        {ImGuiCol_Border, color::border},
        {ImGuiCol_Text, color::text},
    };
    for (const auto& [index, value] : colors) ImGui::PushStyleColor(index, value);
    ImGui::PushFont(fonts.body);
    ImGui::Begin("##recharged_online_panel", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings
                     | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoScrollbar
                     | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoFocusOnAppearing);
    // Opening: over everything else. Its first item takes the focus itself
    // (ClaimFocus): focusing the window would put ImGui's on the first item.
    if (state.focus) ImGui::BringWindowToDisplayFront(ImGui::GetCurrentWindow());
    auto* draw = ImGui::GetWindowDrawList();

    // The sheet at the right, sized for the picture: about a third of a
    // 1280x720 window, narrower on small ones.
    const float margin = Dp(16);
    const float width = std::min(std::clamp(area_w * 0.36f, Dp(360), Dp(440)), area_w - 2 * margin);
    const float slide = (1.0f - eased) * (width + margin + Dp(40));
    const ImVec2 sheet_min{area_max.x - margin - width + slide, area_min.y + margin};
    const ImVec2 sheet_max{area_max.x - margin + slide, area_max.y - margin};
    const float rounding = Dp(20);
    if (style.dim > 0.0f) draw->AddRectFilled(area_min, area_max, Col(ImVec4{0, 0, 0, 1}, style.dim * eased));
    for (int i = 0; i < 8; ++i)
    {
        const float grow = Dp(2.0f + 3.0f * float(i));
        draw->AddRectFilled({sheet_min.x - grow, sheet_min.y - grow * 0.6f}, {sheet_max.x + grow, sheet_max.y + grow},
                            Col(ImVec4{0, 0, 0, 1}, 0.07f * (1.0f - float(i) / 8.0f) * eased), rounding + grow);
    }
    const ImVec4 sheet = Mix(color::window, color::card, 0.4f);
    draw->AddRectFilled(sheet_min, sheet_max, Col(sheet, 0.97f), rounding);
    {
        ImVec4 glow = Mix(sheet, color::accent, 0.10f), none = sheet;
        glow.w = 0.97f;
        none.w = 0.0f;
        FillFade(draw, {sheet_min.x + Dp(1), sheet_min.y + Dp(1)}, {sheet_max.x - Dp(1), sheet_min.y + Dp(170)}, glow,
                 none, rounding - Dp(1), ImDrawFlags_RoundCornersTop);
    }
    draw->AddRect(sheet_min, sheet_max, Col(color::border), rounding, 0, Dp(1));

    const float pad = Dp(20);
    const float content_x = sheet_min.x + pad, content_w = width - 2 * pad;
    float y = sheet_min.y + Dp(16);
    ImGuiID close_id = 0;
    y += DrawHeader(frame, model, {content_x, y}, content_w, close_id);
    if (model.set.stage != OnlinePanelSet::Stage::None)
        y += DrawSet(frame, model.set, {content_x, y}, content_w) + Dp(6);
    if (!model.note.empty())
    {
        const float wrap = content_w - Dp(30);
        const float h = fonts.caption->CalcTextSizeA(FontSize(fonts.caption), FLT_MAX, wrap, model.note.c_str()).y;
        DrawIcon(draw, Icon::Info, {content_x + Dp(9), y + FontSize(fonts.caption) * 0.5f}, Dp(14), Col(color::info));
        draw->AddText(fonts.caption, FontSize(fonts.caption), {content_x + Dp(26), y}, Col(color::muted),
                      model.note.c_str(), nullptr, wrap);
        y += h + Dp(6);
    }

    // The lists, scrolling between the top and the hint at the bottom.
    const float footer = Dp(46);
    const float list_bottom = sheet_max.y - footer;
    // The scroll bar sits in the sheet's padding, right of the rows.
    const float bar = Dp(12);
    ImGui::SetCursorScreenPos({content_x, y});
    ImGui::BeginChild("##online_panel_lists", {content_w + bar, std::max(Dp(40), list_bottom - y)},
                      ImGuiChildFlags_NavFlattened, ImGuiWindowFlags_NoBackground);
    const float row_width = std::min(content_w, ImGui::GetContentRegionAvail().x);
    for (std::size_t s = 0; s < model.sections.size(); ++s)
    {
        if (s) ImGui::Dummy({row_width, Dp(10)});
        DrawSection(frame, model.sections[s], row_width);
    }
    ImGui::Dummy({row_width, Dp(8)});
    // The lists fade out under the hint when they scroll on.
    if (ImGui::GetScrollY() < ImGui::GetScrollMaxY() - 1.0f)
    {
        auto* list = ImGui::GetWindowDrawList();
        const ImVec2 wmin = ImGui::GetWindowPos();
        const float bottom = wmin.y + ImGui::GetWindowHeight();
        ImVec4 clear = sheet, solid = sheet;
        clear.w = 0.0f;
        solid.w = 0.97f;
        FillFade(list, {wmin.x, bottom - Dp(32)}, {wmin.x + content_w, bottom}, clear, solid, 0.0f);
    }
    ImGui::EndChild();

    // The hint: how to close it.
    {
        const float line_y = list_bottom + Dp(4);
        draw->AddRectFilled({content_x, line_y}, {content_x + content_w, line_y + Dp(1)}, Col(color::border, 0.8f));
        const char* close = "Close";
        const char* key = style.close_key.empty() ? "Esc" : style.close_key.c_str();
        const float key_w = KeyCapWidth(frame, key);
        const ImVec2 text = TextSize(fonts.caption, close);
        const float row_y = line_y + (sheet_max.y - line_y - Dp(22)) * 0.5f;
        const float x = content_x + content_w - text.x - Dp(8) - key_w;
        DrawKeyCap(frame, draw, {x, row_y}, key);
        DrawLabel(draw, fonts.caption, {x + key_w + Dp(8), row_y + (Dp(22) - text.y) * 0.5f}, Col(color::muted), close);
        const std::string online = std::to_string(model.sections.empty() ? 0 : model.sections.front().online)
            + (model.sections.empty() || model.sections.front().online != 1 ? " friends online" : " friend online");
        DrawLabel(draw, fonts.caption, {content_x, row_y + (Dp(22) - text.y) * 0.5f}, Col(color::dim), online.c_str());
    }

    // Escape or B closes it, as does a click beside the sheet.
    if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)
        && (ImGui::IsKeyPressed(ImGuiKey_Escape, false) || ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false)))
        actions.close = true;
    if (ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows) && ImGui::IsMouseClicked(ImGuiMouseButton_Left)
        && !ImGui::IsMouseHoveringRect(sheet_min, sheet_max, false))
        actions.close = true;
    // Nothing to put the cursor on: the close button gets it.
    if (frame.focus_pending && close_id)
    {
        ImGui::SetFocusID(close_id, ImGui::GetCurrentWindow());
        ImGui::SetNavCursorVisible(true);
    }
    state.focus = false;

    ImGui::End();
    ImGui::PopFont();
    ImGui::PopStyleColor(int(std::size(colors)));
    ImGui::PopStyleVar(style_vars);
    launcher::SetUiScale(saved_unit);
    launcher::SetCurrentFonts(saved_fonts);
}

launcher::Fonts AddOnlinePanelFonts(ImFontAtlas& atlas, void* ttf, int ttf_size, float unit)
{
    // Latin-1 and the punctuation the panel writes (dashes, dots, quotes).
    static ImVector<ImWchar> ranges;
    if (ranges.empty())
    {
        ImFontGlyphRangesBuilder builder;
        builder.AddRanges(atlas.GetGlyphRangesDefault());
        builder.AddText("\xE2\x80\x93\xE2\x80\x94\xE2\x80\xA2\xE2\x80\xA6\xE2\x80\x98\xE2\x80\x99\xE2\x80\x9C"
                        "\xE2\x80\x9D");
        builder.BuildRanges(&ranges);
    }
    const auto add = [&](float dp) {
        ImFontConfig config;
        config.FontDataOwnedByAtlas = false;
        config.OversampleH = 2;
        config.OversampleV = 1;
        config.PixelSnapH = false;
        return atlas.AddFontFromMemoryTTF(ttf, ttf_size, std::round(dp * unit), &config, ranges.Data);
    };
    launcher::Fonts fonts;
    fonts.caption = add(13.0f);
    fonts.body = add(15.0f);
    fonts.label = add(16.5f);
    fonts.heading = add(20.0f);
    fonts.title = fonts.heading;
    fonts.button = fonts.label;
    fonts.overline = fonts.label;
    return fonts;
}
} // namespace mscharged::online_panel
