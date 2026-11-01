#include "overlay/overlay_prompt.h"

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
using launcher::DrawLabel;
using launcher::FontSize;
using launcher::Mix;
using launcher::TextSize;

constexpr const char* kWindowName = "##recharged_prompt";

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

// The kicker's colour, and the glass's glow: the accent for a set won or a
// rematch, red for a set lost, amber for one that stopped, blue for a challenge.
ImVec4 KindTint(const OverlayPrompt& prompt)
{
    switch (prompt.kind)
    {
    case OverlayPrompt::Kind::SetStopped: return color::warning;
    case OverlayPrompt::Kind::Challenge: return color::info;
    case OverlayPrompt::Kind::Rematch: return color::accent;
    default:
        if (prompt.has_score && prompt.lost > prompt.won) return color::danger;
        if (prompt.has_score && prompt.lost == prompt.won) return color::muted;
        return color::accent;
    }
}

float Wrapped(ImFont* font, const std::string& text, float width)
{
    return text.empty() ? 0.0f : font->CalcTextSizeA(FontSize(font), FLT_MAX, width, text.c_str()).y;
}

float CapsWidth(const std::vector<std::string>& keys)
{
    float width = 0.0f;
    for (const auto& key : keys) width += OverlayKeyCapWidth(key.c_str()) + Dp(4);
    return keys.empty() ? 0.0f : width - Dp(4);
}

// One of the prompt's buttons: its key caps and its label, centred; the
// accent ring while it has the keyboard and gamepad cursor.
bool PromptButton(const char* id, const std::string& label, const std::vector<std::string>& keys, ImVec2 size,
                  bool primary)
{
    const launcher::Fonts& fonts = launcher::CurrentFonts();
    ImGui::PushID(id);
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const bool pressed = ImGui::InvisibleButton("##button", size, ImGuiButtonFlags_EnableNav);
    const bool hovered = ImGui::IsItemHovered(), held = ImGui::IsItemActive(), focused = OverlayNavFocused();
    const float hover = launcher::Animate(ImGui::GetID("##hover"), hovered || focused, 12.0f);
    auto* draw = ImGui::GetWindowDrawList();
    const ImVec2 max{pos.x + size.x, pos.y + size.y};
    const float rounding = Dp(12);
    ImU32 ink;
    if (primary)
    {
        const float light = held ? -0.10f : 0.12f * hover;
        const ImVec4 a = Mix(color::accent_hi, ImVec4{1, 1, 1, 1}, std::max(0.0f, light));
        const ImVec4 b = Mix(color::accent, light < 0 ? color::accent_dark : ImVec4{1, 1, 1, 1}, std::fabs(light));
        launcher::FillGradient(draw, pos, max, Col(a), Col(b), rounding, true);
        ImVec4 sheen{1, 1, 1, 0.22f}, clear{1, 1, 1, 0.0f};
        OverlayFade(draw, {pos.x + Dp(1), pos.y + Dp(1)}, {max.x - Dp(1), pos.y + size.y * 0.5f}, sheen, clear,
                    rounding - Dp(1), ImDrawFlags_RoundCornersTop);
        draw->AddRect(pos, max, Col(Mix(color::accent_hi, ImVec4{1, 1, 1, 1}, 0.5f), 0.7f), rounding, 0, Dp(1));
        ink = Col(color::accent_ink);
    }
    else
    {
        ImVec4 fill = held ? Mix(color::field, color::accent, 0.18f) : Mix(color::field, color::border, 0.8f * hover);
        fill.w = 0.85f;
        OverlayCard(draw, pos, max, rounding, fill, Mix(color::border, color::muted, 0.4f * hover));
        ink = Col(Mix(color::muted, color::text, 0.55f + 0.45f * hover));
    }
    const ImVec2 text = TextSize(fonts.label, label.c_str());
    const float caps = CapsWidth(keys), gap = keys.empty() ? 0.0f : Dp(12);
    float x = pos.x + (size.x - (caps + gap + text.x)) * 0.5f;
    for (const auto& key : keys)
    {
        OverlayKeyCap(draw, {x, pos.y + (size.y - Dp(22)) * 0.5f}, key.c_str(), primary ? 0.0f : hover * 0.5f);
        x += OverlayKeyCapWidth(key.c_str()) + Dp(4);
    }
    if (!keys.empty()) x += gap - Dp(4);
    DrawLabel(draw, fonts.label, {x, pos.y + (size.y - text.y) * 0.5f}, ink, label.c_str());
    if (focused) OverlayFocusRing(draw, pos, max, rounding);
    ImGui::PopID();
    return pressed;
}

// Games won (left, the accent) and lost (right) of a best-of series, as pips
// centred under the score.
void DrawPips(ImDrawList* draw, ImVec2 center_top, int games, int won, int lost)
{
    const int need = games / 2 + 1;
    const float r = Dp(4), step = Dp(13), gap = Dp(16);
    const float group = step * float(need - 1) + 2 * r;
    for (int side = 0; side < 2; ++side)
    {
        const float start = side == 0 ? center_top.x - gap * 0.5f - group : center_top.x + gap * 0.5f;
        const int filled = side == 0 ? won : lost;
        const ImVec4 tint = side == 0 ? color::accent : Mix(color::text, color::muted, 0.3f);
        for (int i = 0; i < need; ++i)
        {
            const ImVec2 c{start + r + step * float(i), center_top.y + r};
            if (i < filled) draw->AddCircleFilled(c, r, Col(tint));
            else draw->AddCircle(c, r - Dp(0.5f), Col(color::muted, 0.55f), 0, Dp(1.2f));
        }
    }
}
} // namespace

void OverlayPromptState::Open()
{
    focus = true;
    shown = 0.0f;
}

void DrawOverlayPrompt(const OverlayPrompt& prompt, OverlayPromptState& state, OverlayPromptActions& actions,
                       const OverlayStyle& style)
{
    // Its entrance: it rises a little and fades in, over a fifth of a second.
    const float own = std::clamp(state.shown / 0.2f, 0.0f, 1.0f);
    state.shown += ImGui::GetIO().DeltaTime;
    const float appear = std::min(std::clamp(style.appear, 0.0f, 1.0f), own);
    const float eased = 1.0f - (1.0f - appear) * (1.0f - appear) * (1.0f - appear);

    const OverlayArea area = BeginOverlayLayer(kWindowName, style, state.focus);
    const launcher::Fonts& fonts = launcher::CurrentFonts();
    auto* draw = ImGui::GetWindowDrawList();
    const int first_vertex = draw->VtxBuffer.Size;
    const float area_w = area.max.x - area.min.x, area_h = area.max.y - area.min.y;

    // Its size: about 560 dp wide, as tall as its content.
    const float width = std::min(Dp(560), area_w - Dp(48));
    const float pad = Dp(28), inner = width - 2 * pad;
    const float kicker_h = FontSize(fonts.caption) + Dp(8);
    const float title_h = FontSize(fonts.button) + Dp(20);
    const float radius = Dp(25), score_size = FontSize(fonts.button) * 1.6f;
    const bool score = prompt.has_score && prompt.games > 0;
    const float person_h = std::max(radius * 2, score ? score_size + Dp(14) : 0.0f);
    const float detail_h = prompt.detail.empty() ? 0.0f : Dp(16) + Wrapped(fonts.caption, prompt.detail, inner);
    const float buttons_h = Dp(48);
    const float note_h = prompt.note.empty() ? 0.0f : Dp(12) + Wrapped(fonts.caption, prompt.note, inner);
    const float height =
        pad + kicker_h + title_h + person_h + detail_h + Dp(22) + Dp(1) + Dp(20) + buttons_h + note_h + pad;
    const float rise = (1.0f - eased) * Dp(22);
    const ImVec2 min{area.min.x + (area_w - width) * 0.5f, area.min.y + (area_h - height) * 0.5f + rise};
    const ImVec2 max{min.x + width, min.y + height};
    const ImVec4 tint = KindTint(prompt);

    if (style.dim > 0.0f) draw->AddRectFilled(area.min, area.max, Col(ImVec4{0, 0, 0, 1}, style.dim));
    OverlayGlass(draw, min, max, Dp(22), 1.0f, tint);

    // The kicker and the title.
    float y = min.y + pad;
    launcher::DrawTracked(draw, fonts.caption, {min.x + pad, y}, Col(tint), prompt.kicker.c_str(), Dp(1.8f));
    y += kicker_h;
    draw->PushClipRect({min.x + pad, y}, {max.x - pad, y + title_h}, true);
    DrawLabel(draw, fonts.button, {min.x + pad, y}, Col(color::text), prompt.title.c_str());
    draw->PopClipRect();
    y += title_h;

    // The opponent, and the score beside them.
    {
        const float middle = y + person_h * 0.5f;
        const ImVec2 center{min.x + pad + radius, middle};
        DrawPlayerPicture(draw, center, radius, prompt.opponent, prompt.image, OverlaySurface());
        const float left = center.x + radius + Dp(14);
        const float score_w = score ? Dp(150) : 0.0f;
        const bool status = !prompt.opponent.status.empty();
        const float block = FontSize(fonts.heading) + (status ? Dp(3) + FontSize(fonts.caption) : 0.0f);
        draw->PushClipRect({left, y}, {max.x - pad - score_w, y + person_h}, true);
        DrawLabel(draw, fonts.heading, {left, middle - block * 0.5f}, Col(color::text), prompt.opponent.name.c_str());
        if (status)
            DrawLabel(draw, fonts.caption, {left, middle - block * 0.5f + FontSize(fonts.heading) + Dp(3)},
                      Col(ToneColor(prompt.opponent.tone)), prompt.opponent.status.c_str());
        draw->PopClipRect();
        if (score)
        {
            // "2 - 1", large, over the pips of the wins each side needs.
            const std::string text = std::to_string(prompt.won) + "  \xE2\x80\x93  " + std::to_string(prompt.lost);
            const ImVec2 size = fonts.button->CalcTextSizeA(score_size, FLT_MAX, 0.0f, text.c_str());
            const float cx = max.x - pad - score_w * 0.5f;
            const float top = middle - (size.y + Dp(14)) * 0.5f;
            draw->AddText(fonts.button, score_size, {cx - size.x * 0.5f, top}, Col(color::text), text.c_str());
            DrawPips(draw, {cx, top + size.y + Dp(4)}, prompt.games, prompt.won, prompt.lost);
        }
        y += person_h;
    }
    if (!prompt.detail.empty())
    {
        y += Dp(16);
        draw->AddText(fonts.caption, FontSize(fonts.caption), {min.x + pad, y}, Col(color::muted),
                      prompt.detail.c_str(), nullptr, inner);
        y += Wrapped(fonts.caption, prompt.detail, inner);
    }
    y += Dp(22);
    draw->AddRectFilled({min.x + pad, y}, {max.x - pad, y + Dp(1)}, Col(color::border, 0.8f));
    y += Dp(1) + Dp(20);

    // The buttons: the secondary at the left, the primary at the right; the
    // primary has the cursor as the prompt opens.
    const float gap = Dp(12);
    const bool two = !prompt.secondary.empty();
    const float button_w = two ? (inner - gap) * 0.5f : inner;
    ImGuiID primary_id = 0;
    if (two)
    {
        ImGui::SetCursorScreenPos({min.x + pad, y});
        if (PromptButton("secondary", prompt.secondary, prompt.secondary_keys, {button_w, buttons_h}, false))
            actions.secondary = true;
    }
    ImGui::SetCursorScreenPos({max.x - pad - button_w, y});
    if (PromptButton("primary", prompt.primary, prompt.primary_keys, {button_w, buttons_h}, true))
        actions.primary = true;
    primary_id = ImGui::GetItemID();
    y += buttons_h;
    if (!prompt.note.empty())
    {
        y += Dp(12);
        draw->AddText(fonts.caption, FontSize(fonts.caption), {min.x + pad, y}, Col(color::dim), prompt.note.c_str(),
                      nullptr, inner);
    }
    if (state.focus && primary_id)
    {
        ImGui::SetFocusID(primary_id, ImGui::GetCurrentWindow());
        ImGui::SetNavCursorVisible(true);
        state.focus = false;
    }

    // B or Escape, or a click beside it: the secondary answer.
    if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)
        && (ImGui::IsKeyPressed(ImGuiKey_Escape, false) || ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false)))
        actions.secondary = true;
    if (ImGui::IsWindowHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Left)
        && !ImGui::IsMouseHoveringRect(min, max, false))
        actions.secondary = true;
    if (actions.primary) actions.secondary = false;

    // The entrance: everything fades in together.
    if (eased < 1.0f)
        for (int i = first_vertex; i < draw->VtxBuffer.Size; ++i)
        {
            ImU32& col = draw->VtxBuffer[i].col;
            const float alpha = float((col >> IM_COL32_A_SHIFT) & 0xFF) * eased;
            col = (col & ~IM_COL32_A_MASK) | (ImU32(alpha + 0.5f) << IM_COL32_A_SHIFT);
        }
    EndOverlayLayer();
}
} // namespace mscharged::overlay
