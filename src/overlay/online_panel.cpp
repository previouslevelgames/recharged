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

// A radar's ping going out of a round badge at `center`: rings that grow and
// fade, then a pause; two half a period apart while `busy`.
void PingRound(ImDrawList* draw, ImVec2 center, float radius, bool busy)
{
    const double period = busy ? 1.8 : 2.8;
    const float strength = busy ? 0.5f : 0.32f;
    for (int ring = 0; ring < (busy ? 2 : 1); ++ring)
    {
        const float t = float(std::fmod(ImGui::GetTime() / period + 0.5 * ring, 1.0)) / 0.7f;
        if (t >= 1.0f) continue;
        const float out = 1.0f - (1.0f - t) * (1.0f - t) * (1.0f - t);
        draw->AddCircle(center, radius + Dp(1) + Dp(13) * out, Col(color::accent, strength * (1.0f - t) * (1.0f - t)),
                        0, Dp(1.5f));
    }
}

// The badge of Open to challenges, as a status reads: `on` (0..1) green
// with a check, open to challenges; off red with a bar, as Do not disturb.
void AvailabilityBadge(ImDrawList* draw, ImVec2 center, float radius, float on)
{
    const ImVec4 tint = Mix(color::danger, color::success, on);
    draw->AddCircleFilled(center, radius, Col(tint, 0.16f));
    draw->AddCircle(center, radius - Dp(0.5f), Col(tint, 0.55f), 0, Dp(1.2f));
    if (on > 0.01f) launcher::DrawIcon(draw, Icon::Check, center, radius * 1.05f, Col(tint, on));
    if (on < 0.99f)
        draw->AddLine({center.x - radius * 0.45f, center.y}, {center.x + radius * 0.45f, center.y},
                      Col(tint, 1.0f - on), Dp(2.6f));
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
    const char* on_line = "Players who aren't your friends can see you and challenge you.";
    const char* off_line = "Only your friends see you.";
    const char* line = me.available ? on_line : off_line;
    const float track_w = Dp(40), track_h = Dp(23);
    // Each part's badge at the left, its words beside it.
    const float badge = Dp(17), text_x = at.x + pad + badge * 2 + Dp(12);
    const float wrap = width - 2 * pad - badge * 2 - Dp(12) - track_w - Dp(14);
    // As high as the longer of its lines: switching moves nothing below it.
    const float available_h = me.can_set_available
        ? std::max(Dp(12) + FontSize(fonts.label) + Dp(3)
                       + std::max(caption_height(on_line, wrap), caption_height(off_line, wrap)) + Dp(14),
                   badge * 2 + Dp(24))
        : 0.0f;

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
    const char* invite = "Your rules go to players open to challenges, until one accepts.";
    if (!searching && can_search) search_line = invite;
    const float stop_w = Dp(76);
    const float search_wrap = width - 2 * pad - badge * 2 - Dp(12) - (searching ? stop_w : Dp(20)) - Dp(14);
    // As high for the search as before it (two lines at least).
    const float search_h = searching || can_search
        ? std::max(Dp(12) + FontSize(fonts.label) + Dp(3)
                       + std::max({caption_height(search_line.c_str(), search_wrap), caption_height(invite, search_wrap),
                                   FontSize(fonts.caption) * 2})
                       + Dp(14),
                   badge * 2 + Dp(24))
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
        AvailabilityBadge(draw, {at.x + pad + badge, y + available_h * 0.5f}, badge, on);
        DrawLabel(draw, fonts.label, {text_x, y + Dp(12)}, Col(color::text), label);
        draw->AddText(fonts.caption, FontSize(fonts.caption), {text_x, y + Dp(12) + FontSize(fonts.label) + Dp(3)},
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
        // The step to take: lit in the accent, its badge sending a radar's
        // ping out (faster while the search runs).
        draw->AddRectFilled({min.x + Dp(1), min.y}, {max.x - Dp(1), max.y - Dp(1)}, Col(color::accent, 0.07f), Dp(15),
                            ImDrawFlags_RoundCornersBottom);
        const bool pressed = CardPart(draw, "##search", min, max, true, true, focused, hover);
        const ImGuiID item = ImGui::GetItemID();
        const float middle = y + search_h * 0.5f;
        const ImVec2 center{at.x + pad + badge, middle};
        draw->PushClipRect(min, max, true);
        PingRound(draw, center, badge, searching);
        draw->PopClipRect();
        draw->AddCircleFilled(center, badge, Col(Mix(color::accent, color::accent_hi, 0.3f * hover)));
        launcher::DrawIcon(draw, Icon::Target, center, badge * 1.15f, Col(color::accent_ink));
        DrawLabel(draw, fonts.label, {text_x, y + Dp(12)}, Col(color::accent),
                  searching ? "Searching for an opponent" : "Find an opponent");
        draw->AddText(fonts.caption, FontSize(fonts.caption), {text_x, y + Dp(12) + FontSize(fonts.label) + Dp(3)},
                      Col(color::muted), search_line.c_str(), nullptr, search_wrap);
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
        else OverlayChevron(draw, {end.x - pad - Dp(6), middle}, 1, Col(Mix(color::accent, color::text, hover)));
        if (focused) OverlayFocusRing(draw, min, max, Dp(16));
        // Its keys hang from its lower edge at the right, as on the search's
        // screen: Y (or F) presses it from anywhere on this screen.
        float x = max.x - Dp(12) - (OverlayKeyCapWidth("Y") + Dp(4) + OverlayKeyCapWidth("F"));
        for (const char* key : {"Y", "F"})
        {
            OverlayKeyCap(draw, {x, max.y - Dp(11)}, key);
            x += OverlayKeyCapWidth(key) + Dp(4);
        }
        const bool shortcut = !ImGui::GetIO().WantTextInput
            && (ImGui::IsKeyPressed(ImGuiKey_GamepadFaceUp, false) || ImGui::IsKeyPressed(ImGuiKey_F, false));
        if (pressed || shortcut)
        {
            if (searching) frame.actions.stop_search = true;
            else OpenSearchSetup(frame, item);
        }
    }
    ImGui::SetCursorScreenPos(at);
    // The keys hang below the card.
    ImGui::Dummy({width, end.y - at.y + (search_h > 0.0f ? Dp(11) : 0.0f)});
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
        const float left = OverlaySplitLeft(width), gap = OverlaySplitGap();
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
        OverlaySplitDivider(layout, left);
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

// ------------------------------------------------------------------ signed out

// What a player profile brings, as the launcher's welcome says it.
struct Benefit
{
    Icon icon;
    const char* title;
    const char* text;
    const char* tag;
};
constexpr Benefit kBenefits[] = {
    {Icon::Play, "Online in one click",
     "Skip the Wii's friend codes and setup: your profile is ready in a minute, with Discord or a Mii.", nullptr},
    {Icon::Chat, "Challenges over the game",
     "See who is online right here and challenge them, without leaving the game.", nullptr},
    {Icon::Trophy, "Real sets, fair rules",
     "Best-of series by competitive rules that both players see before they accept.", nullptr},
    {Icon::Medal, "Tournaments", "Your next tournament game, opponent and rules, in the Events tab. With Discord.",
     "Soon"},
};

// The invitation: play online the easy way, made on the launcher's Player
// profile page.
void DrawInvitation(const Frame& frame, float width)
{
    const auto& fonts = frame.fonts;
    auto* draw = ImGui::GetWindowDrawList();
    const float pad = Dp(20), badge = Dp(52), text_width = width - pad * 2;
    const char* title = "Play online, the easy way";
    const char* intro = "Connect Discord, or play with a Mii: no friend codes, no setup.";
    const char* how = "Make your player profile on the launcher's Player profile page. Your friends and "
                      "challenges show here from the game's next start.";
    const auto wrapped = [&](ImFont* font, const char* text, float wrap) {
        return font->CalcTextSizeA(FontSize(font), FLT_MAX, wrap, text).y;
    };
    const float how_width = text_width - Dp(26);
    const float height = pad + badge + Dp(16) + wrapped(fonts.heading, title, text_width) + Dp(6)
        + wrapped(fonts.body, intro, text_width) + Dp(16) + Dp(12) + wrapped(fonts.caption, how, how_width) + Dp(12)
        + pad;
    const ImVec2 at = ImGui::GetCursorScreenPos();
    const ImVec2 end{at.x + width, at.y + height};
    // A soft accent glow at the top, as the launcher's welcome has it.
    draw->AddRectFilled(at, end, Col(color::field, 0.85f), Dp(16));
    draw->AddRectFilledMultiColor({at.x + Dp(1), at.y + Dp(1)}, {end.x - Dp(1), at.y + Dp(110)},
                                  Col(color::accent, 0.10f), Col(color::accent, 0.10f), Col(color::accent, 0.0f),
                                  Col(color::accent, 0.0f));
    draw->AddRect(at, end, Col(color::border, 0.8f), Dp(16));
    float y = at.y + pad;
    const ImVec2 icon{at.x + pad + badge * 0.5f, y + badge * 0.5f};
    draw->AddCircleFilled(icon, badge * 0.5f, Col(color::accent, 0.16f));
    DrawIcon(draw, Icon::Person, icon, Dp(24), Col(color::accent));
    y += badge + Dp(16);
    draw->AddText(fonts.heading, FontSize(fonts.heading), {at.x + pad, y}, Col(color::text), title, nullptr,
                  text_width);
    y += wrapped(fonts.heading, title, text_width) + Dp(6);
    draw->AddText(fonts.body, FontSize(fonts.body), {at.x + pad, y}, Col(color::muted), intro, nullptr, text_width);
    y += wrapped(fonts.body, intro, text_width) + Dp(16);
    // Where to make it: a note in the info colour.
    const float note_h = Dp(12) + wrapped(fonts.caption, how, how_width) + Dp(12);
    draw->AddRectFilled({at.x + pad, y}, {end.x - pad, y + note_h}, Col(color::info, 0.10f), Dp(10));
    DrawIcon(draw, Icon::Info, {at.x + pad + Dp(14), y + Dp(12) + FontSize(fonts.caption) * 0.5f}, Dp(14),
             Col(color::info));
    draw->AddText(fonts.caption, FontSize(fonts.caption), {at.x + pad + Dp(26), y + Dp(12)}, Col(color::text), how,
                  nullptr, how_width - Dp(12));
    ImGui::Dummy({width, height});
}

// What a profile brings, one row each.
void DrawBenefits(const Frame& frame, float width)
{
    const auto& fonts = frame.fonts;
    auto* draw = ImGui::GetWindowDrawList();
    const float pad = Dp(16), badge = Dp(38), text_width = width - pad * 2 - badge - Dp(14);
    DrawLabel(draw, fonts.caption, ImGui::GetCursorScreenPos(), Col(color::dim), "WHAT A PLAYER PROFILE BRINGS");
    ImGui::Dummy({width, FontSize(fonts.caption) + Dp(8)});
    for (const Benefit& benefit : kBenefits)
    {
        const float text = fonts.body->CalcTextSizeA(FontSize(fonts.body), FLT_MAX, text_width, benefit.text).y;
        const float height = pad + FontSize(fonts.label) + Dp(6) + text + pad;
        const ImVec2 at = ImGui::GetCursorScreenPos();
        const ImVec2 end{at.x + width, at.y + height};
        const ImVec4 tint = benefit.tag ? color::info : color::accent;
        draw->AddRectFilled(at, end, Col(color::field, 0.7f), Dp(14));
        draw->AddRect(at, end, Col(color::border, 0.8f), Dp(14));
        const ImVec2 icon{at.x + pad + badge * 0.5f, at.y + pad + badge * 0.5f};
        draw->AddRectFilled({icon.x - badge * 0.5f, icon.y - badge * 0.5f}, {icon.x + badge * 0.5f, icon.y + badge * 0.5f},
                            Col(tint, 0.14f), Dp(10));
        DrawIcon(draw, benefit.icon, icon, Dp(19), Col(tint));
        const float left = at.x + pad + badge + Dp(14);
        DrawLabel(draw, fonts.label, {left, at.y + pad}, Col(color::text), benefit.title);
        if (benefit.tag)
        {
            const float x = left + TextSize(fonts.label, benefit.title).x + Dp(10);
            const ImVec2 tag = TextSize(fonts.caption, benefit.tag);
            const ImVec2 tag_at{x, at.y + pad + (FontSize(fonts.label) - Dp(20)) * 0.5f};
            draw->AddRectFilled(tag_at, {tag_at.x + tag.x + Dp(16), tag_at.y + Dp(20)}, Col(color::info, 0.16f), Dp(10));
            DrawLabel(draw, fonts.caption, {tag_at.x + Dp(8), tag_at.y + (Dp(20) - tag.y) * 0.5f}, Col(color::info),
                      benefit.tag);
        }
        draw->AddText(fonts.body, FontSize(fonts.body), {left, at.y + pad + FontSize(fonts.label) + Dp(6)},
                      Col(color::muted), benefit.text, nullptr, text_width);
        ImGui::Dummy({width, height});
        ImGui::Dummy({width, Dp(8)});
    }
}

// Without a player profile: the invitation at the left, what a profile
// brings at the right, split as the main screen; nothing to press (the
// launcher makes the profile).
void DrawSignedOut(const Frame& frame, const OverlayLayout& layout)
{
    const float width = layout.max.x - layout.min.x;
    const bool wide = width >= Dp(700);
    OverlayLayoutIs(wide ? 2 : 1);
    if (wide)
    {
        const float left = OverlaySplitLeft(width);
        BeginOverlayScroll("##signed_out_you", layout.min, {layout.min.x + left, layout.max.y});
        ImGui::Dummy({left, Dp(20)});
        DrawInvitation(frame, left);
        EndOverlayScroll();
        OverlaySplitDivider(layout, left);
        const ImVec2 pane{layout.min.x + left + OverlaySplitGap(), layout.min.y};
        BeginOverlayScroll("##signed_out_benefits", pane, layout.max);
        ImGui::Dummy({layout.max.x - pane.x, Dp(20)});
        DrawBenefits(frame, layout.max.x - pane.x);
        EndOverlayScroll();
        return;
    }
    BeginOverlayScroll("##signed_out_column", layout.min, layout.max);
    ImGui::Dummy({width, Dp(18)});
    DrawInvitation(frame, width);
    ImGui::Dummy({width, Dp(16)});
    DrawBenefits(frame, width);
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
              std::string& key, float width, bool interactive)
{
    std::vector<std::string> keys;
    std::string shown = key;
    for (const auto& choice : choices)
    {
        keys.push_back(choice.key);
        if (key == choice.key) shown = choice.label;
    }
    if (const int step = OverlayChoiceRow(id, label, shown, width, interactive)) key = Next(keys, key, step);
}

// A radar's ping going out of the box from `min` to `max`: a ring that grows
// and fades, then a pause; two rings half a period apart while `busy` (the
// search runs). It draws the eye to the button without moving it.
void Ping(ImDrawList* draw, ImVec2 min, ImVec2 max, float rounding, bool busy)
{
    const double period = busy ? 1.8 : 2.8;
    const float strength = busy ? 0.45f : 0.28f;
    for (int ring = 0; ring < (busy ? 2 : 1); ++ring)
    {
        // Out over the first 70 % of the period.
        const float t = float(std::fmod(ImGui::GetTime() / period + 0.5 * ring, 1.0)) / 0.7f;
        if (t >= 1.0f) continue;
        const float out = 1.0f - (1.0f - t) * (1.0f - t) * (1.0f - t);
        const float grow = Dp(2) + Dp(16) * out;
        draw->AddRect({min.x - grow, min.y - grow}, {max.x + grow, max.y + grow},
                      Col(color::accent, strength * (1.0f - t) * (1.0f - t)), rounding + grow, 0, Dp(1.5f));
    }
}

// What the screen draws from the vertex `from` of `draw` on, faded: the
// rules while a search runs on them.
void Fade(ImDrawList* draw, int from, float alpha)
{
    for (int n = from; n < draw->VtxBuffer.Size; ++n)
    {
        ImU32& col = draw->VtxBuffer[n].col;
        const ImU32 a = ImU32(float((col >> IM_COL32_A_SHIFT) & 0xFF) * alpha);
        col = (col & ~IM_COL32_A_MASK) | (a << IM_COL32_A_SHIFT);
    }
}

// The rules of a challenge or search, all of them to change for this one
// only: the preset first, the match and the cheats side by side, the
// gameplay two to a row (one column where the room is too narrow).
void DrawRules(Frame& frame, platform::MatchRules& rules, float width, bool interactive)
{
    const auto& model = frame.model;
    auto& state = frame.state;
    const int count = int(model.presets.size());
    const OnlinePanelPreset& preset = model.presets[std::size_t(state.preset)];
    const bool two = width >= Dp(720);
    const float gap = Dp(20), half = two ? (width - gap) * 0.5f : width;

    OverlayRowGroup("PRESET", width, [&] {
        if (const int step = OverlayChoiceRow("preset", "Start from", preset.name, width, interactive))
        {
            state.preset = (state.preset + step + count) % count;
            rules = model.presets[std::size_t(state.preset)].rules;
        }
    });
    const auto match = [&] {
        OverlayRowGroup("THE MATCH", half, [&] {
            if (const int step = OverlayChoiceRow(
                    "games", "Series",
                    rules.games == 1 ? std::string("One game") : "Best of " + std::to_string(rules.games), half,
                    interactive))
                rules.games = Next(std::vector<int>{1, 3, 5, 7}, rules.games, step);
            if (OverlayChoiceRow("limit", "A game ends", rules.limit == "goals" ? "On goals" : "On time", half,
                                 interactive))
                rules.limit = rules.limit == "goals" ? "time" : "goals";
            if (rules.limit == "goals")
            {
                if (const int step =
                        OverlayChoiceRow("goals", "Goals to win", std::to_string(rules.goals), half, interactive))
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
                if (const int step =
                        OverlayChoiceRow("time", "Game length", GameLength(rules.time), half, interactive))
                    rules.time = Next(lengths, rules.time, step);
            }
            std::vector<std::string> stadiums;
            std::string stadium = rules.stadium;
            for (const auto& choice : platform::StadiumChoices())
            {
                stadiums.push_back(choice.key);
                if (rules.stadium == choice.key) stadium = choice.label;
            }
            if (const int step = OverlayChoiceRow("stadium", "Stadium", stadium, half, interactive))
                rules.stadium = Next(stadiums, rules.stadium, step);
        });
    };
    const auto cheats = [&] {
        OverlayRowGroup("CHEATS", half, [&] {
            CheatRow("stadium_cheat", "Stadium", platform::StadiumCheatChoices(), rules.stadium_cheat, half,
                     interactive);
            CheatRow("player_cheat", "Players", platform::PlayerCheatChoices(), rules.player_cheat, half,
                     interactive);
            CheatRow("powerup_cheat", "Power-ups", platform::PowerupCheatChoices(), rules.powerup_cheat, half,
                     interactive);
        });
    };
    if (two)
    {
        const ImVec2 at = ImGui::GetCursorScreenPos();
        ImGui::BeginGroup();
        match();
        ImGui::EndGroup();
        const float bottom = ImGui::GetItemRectMax().y;
        ImGui::SetCursorScreenPos({at.x + half + gap, at.y});
        ImGui::BeginGroup();
        cheats();
        ImGui::EndGroup();
        ImGui::SetCursorScreenPos({at.x, std::max(bottom, ImGui::GetItemRectMax().y)});
    }
    else
    {
        match();
        cheats();
    }
    OverlayRowGroup("GAMEPLAY", width, [&] {
        const auto& tweaks = platform::RuleTweakChoices();
        const ImVec2 start = ImGui::GetCursorScreenPos();
        float line_h = 0.0f, bottom = start.y;
        for (std::size_t n = 0; n < tweaks.size(); ++n)
        {
            const auto& tweak = tweaks[n];
            const std::size_t column = two ? n % 2 : 0, line = two ? n / 2 : n;
            ImGui::SetCursorScreenPos({start.x + float(column) * (half + gap), start.y + float(line) * line_h});
            const auto found = std::find(rules.tweaks.begin(), rules.tweaks.end(), tweak.key);
            const bool on = found != rules.tweaks.end();
            if (OverlaySwitchRow(tweak.key, tweak.label, on, half, interactive))
            {
                if (on) rules.tweaks.erase(found);
                else
                {
                    rules.tweaks.push_back(tweak.key);
                    std::sort(rules.tweaks.begin(), rules.tweaks.end());
                }
            }
            line_h = ImGui::GetItemRectSize().y;
            bottom = std::max(bottom, ImGui::GetItemRectMax().y);
        }
        ImGui::SetCursorScreenPos({start.x, bottom});
    });
}

// A challenge's or a search's setup: its action at the left, which never
// scrolls away (Send challenge; Find an opponent with a radar's ping, then
// the search running and Stop), and the rules beside it, to change for this
// one only. While the search runs its rules stay as they went: greyed, not
// to be changed.
void DrawSetup(Frame& frame, const OverlayLayout& layout, const OnlinePanelRow* row, bool search)
{
    const auto& model = frame.model;
    auto& state = frame.state;
    const auto& fonts = frame.fonts;
    const float total = layout.max.x - layout.min.x;
    if ((!search && !row) || model.presets.empty() || state.setup_for != (search ? kSearchSetup : row->key))
    {
        const float width = std::min(total, Dp(640));
        const float x = layout.min.x + (total - width) * 0.5f;
        BeginOverlayScroll(("##setup_" + frame.screens.Top().key).c_str(), {x, layout.min.y},
                           {x + width, layout.max.y});
        ImGui::Dummy({width, Dp(12)});
        Caption(frame, !search && !row ? "Not online any more." : "This server has no rules to play sets by yet.",
                color::muted, width);
        EndOverlayScroll();
        return;
    }
    const int count = int(model.presets.size());
    state.preset = std::clamp(state.preset, 0, count - 1);
    const OnlinePanelPreset& preset = model.presets[std::size_t(state.preset)];
    auto& rules = state.rules;
    const bool changed = !(rules == preset.rules);

    // A search runs from its request until the server has it (and the panel
    // shows it) or tells why not.
    const double now = ImGui::GetTime();
    if (model.search.active || now - state.search_sent > 10.0) state.search_sent = -100.0;
    const bool searching = search && (model.search.active || state.search_sent > 0.0);

    // The action's column at the left; the rules beside it, as wide as fits.
    const float side = std::clamp(total * 0.26f, Dp(210), Dp(280)), gap = Dp(28);
    const float width = std::min(total - side - gap, Dp(880));
    const float left = layout.min.x + (total - side - gap - width) * 0.5f;
    const ImVec2 column{left, layout.min.y + Dp(12) + Dp(38) + Dp(4)};
    ImGui::SetCursorScreenPos(column);
    ImGui::BeginGroup();
    const ImVec2 button{side, Dp(52)};
    const ImVec2 button_min = ImGui::GetCursorScreenPos();
    if (search) Ping(ImGui::GetWindowDrawList(), button_min, {button_min.x + button.x, button_min.y + button.y},
                     std::min(button.y * 0.5f, Dp(10)), searching);
    const bool can = search ? model.can_search || searching : row->can_challenge;
    const char* label = !search ? "Send challenge" : searching ? "Stop searching" : "Find an opponent NOW!";
    // The cursor starts on it; Y (or F) presses it from anywhere on the
    // screen, as left and right change the rules' values.
    const bool shortcut = can && !ImGui::GetIO().WantTextInput
        && (ImGui::IsKeyPressed(ImGuiKey_GamepadFaceUp, false) || ImGui::IsKeyPressed(ImGuiKey_F, false));
    if (OverlayButton("send", label, !search ? Icon::Trophy : searching ? Icon::Close : Icon::Target, button,
                      searching ? OverlayLook::Secondary : OverlayLook::Primary, can, true)
        || shortcut)
    {
        if (searching)
        {
            frame.actions.stop_search = true;
            state.search_sent = -100.0;
        }
        else
        {
            // Send, by the server's rule set itself while nothing changed
            // (servers before 1.2.3 take only theirs).
            OnlinePanelChallenge challenge;
            challenge.player = search ? 0 : row->player_id;
            challenge.name = preset.name;
            if (changed && preset.name.size() + 10 <= 48) challenge.name += " (changed)";
            challenge.rules = rules;
            challenge.rule_set = changed ? 0 : preset.rule_set;
            frame.actions.set_challenge_rules = preset.name;
            if (search)
            {
                // The search shows here, the rules kept as they went.
                frame.actions.search = challenge;
                state.search_sent = now;
            }
            else
            {
                frame.actions.challenge = challenge;
                while (!frame.screens.Empty() && frame.screens.Top().id != int(OnlinePanelScreen::Main)
                       && frame.screens.Pop()) {}
                ImGui::EndGroup();
                return;
            }
        }
    }
    {
        // Its keys hang from its lower edge at the right.
        auto* draw = ImGui::GetWindowDrawList();
        const float caps = OverlayKeyCapWidth("Y") + Dp(4) + OverlayKeyCapWidth("F");
        float x = button_min.x + button.x - Dp(12) - caps;
        const float y = button_min.y + button.y - Dp(11);
        for (const char* key : {"Y", "F"})
        {
            OverlayKeyCap(draw, {x, y}, key);
            x += OverlayKeyCapWidth(key) + Dp(4);
        }
        ImGui::Dummy({side, Dp(11)});
    }
    ImGui::Dummy({side, Dp(12)});
    if (searching)
    {
        DrawLabel(ImGui::GetWindowDrawList(), fonts.label, ImGui::GetCursorScreenPos(), Col(color::accent),
                  "Searching for an opponent");
        ImGui::Dummy({side, FontSize(fonts.label) + Dp(4)});
        const int seconds = model.search.seconds;
        const int asked = model.search.asked;
        Caption(frame,
                std::to_string(seconds / 60) + (seconds % 60 < 10 ? ":0" : ":") + std::to_string(seconds % 60)
                    + "  ·  "
                    + (asked == 0 ? std::string("nobody asked yet")
                                  : std::to_string(asked) + (asked == 1 ? " player asked" : " players asked")),
                color::text, side);
        ImGui::Dummy({side, Dp(8)});
        Caption(frame, "The rules stay as they went until you stop.", color::muted, side);
    }
    else
    {
        const std::string kept = changed ? "Changed for this one only: the preset stays as it is. " : "";
        if (search)
            Caption(frame,
                    kept + "Players open to challenges get these rules, one after the other; the first who accepts "
                           "plays you.",
                    color::muted, side);
        else if (!row->can_challenge && !row->blocked.empty())
            Caption(frame, row->blocked + ": no challenge now.", color::warning, side);
        else Caption(frame, kept + row->name + " sees the rules before accepting.", color::muted, side);
    }
    ImGui::EndGroup();

    const ImVec2 rules_min{left + side + gap, layout.min.y};
    BeginOverlayScroll(("##setup_" + frame.screens.Top().key).c_str(), rules_min,
                       {rules_min.x + width, layout.max.y});
    ImGui::Dummy({width, Dp(12)});
    auto* draw = ImGui::GetWindowDrawList();
    const int from = draw->VtxBuffer.Size;
    DrawRules(frame, rules, width, !searching);
    if (searching) Fade(draw, from, 0.4f);
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
    if (model.signed_out) DrawSignedOut(frame, layout);
    else if (screens.Top().id == int(OnlinePanelScreen::Player))
        DrawPlayer(frame, layout, FindRow(model, screens.Top().key));
    else if (screens.Top().id == int(OnlinePanelScreen::Setup))
        DrawSetup(frame, layout, FindRow(model, screens.Top().key), screens.Top().key == kSearchSetup);
    else DrawMain(frame, layout);
}
} // namespace mscharged::overlay
