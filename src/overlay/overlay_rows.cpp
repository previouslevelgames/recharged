#include "overlay/overlay_rows.h"

#include "launcher/ui_kit.h"

#include <algorithm>

namespace mscharged::overlay
{
namespace color = launcher::color;
using launcher::Col;
using launcher::Dp;
using launcher::DrawLabel;
using launcher::FontSize;
using launcher::Mix;
using launcher::TextSize;

namespace
{
constexpr float kRowHeight = 46.0f;

// The row's control: a button the whole row, reached by the keyboard and the
// gamepad, lit as the mouse or the cursor comes; or, not interactive, room
// for it. Whether it was pressed, its id, whether it has the cursor and how lit.
bool RowControl(const char* id, ImVec2 at, float width, bool interactive, ImGuiID& item, bool& focused, float& hover)
{
    item = 0;
    focused = false;
    hover = 0.0f;
    if (!interactive)
    {
        ImGui::Dummy({width, Dp(kRowHeight)});
        return false;
    }
    ImGui::PushID(id);
    const bool pressed = ImGui::InvisibleButton("##row", {width, Dp(kRowHeight)}, ImGuiButtonFlags_EnableNav);
    item = ImGui::GetItemID();
    const bool hovered = ImGui::IsItemHovered();
    focused = OverlayNavFocused();
    OverlayFocusable();
    hover = launcher::Animate(ImGui::GetID("##hover"), hovered || focused, 12.0f);
    ImGui::PopID();
    auto* draw = ImGui::GetWindowDrawList();
    if (hover > 0.01f)
        draw->AddRectFilled(at, {at.x + width, at.y + Dp(kRowHeight)}, Col(color::text, 0.04f * hover), Dp(10));
    return pressed;
}
} // namespace

void OverlayChevron(ImDrawList* draw, ImVec2 center, int direction, ImU32 ink)
{
    const float arm = Dp(4.5f), dx = arm * 0.5f * float(direction);
    const ImVec2 points[] = {
        {center.x - dx, center.y - arm}, {center.x + dx, center.y}, {center.x - dx, center.y + arm}};
    draw->AddPolyline(points, 3, ink, 0, Dp(1.6f));
}

int OverlayChoiceRow(const char* id, const char* label, const std::string& value, float width, bool interactive)
{
    const auto& fonts = launcher::CurrentFonts();
    auto* draw = ImGui::GetWindowDrawList();
    const ImVec2 at = ImGui::GetCursorScreenPos();
    const float height = Dp(kRowHeight), pad = Dp(16), field_h = Dp(30), chevron = Dp(24);
    const ImVec2 max{at.x + width, at.y + height};
    ImGuiID item;
    bool focused;
    float hover;
    const bool pressed = RowControl(id, at, width, interactive, item, focused, hover);
    const float middle = at.y + height * 0.5f;
    const float most = std::max(Dp(90), width * 0.56f);
    const float value_w = std::min(TextSize(fonts.body, value.c_str()).x, most - 2 * chevron);
    const ImVec2 field_min{max.x - pad - value_w - 2 * chevron, middle - field_h * 0.5f};
    const ImVec2 field_max{max.x - pad, middle + field_h * 0.5f};
    ImVec4 field = Mix(color::field, color::card_hover, 0.35f + 0.4f * hover);
    field.w = 0.9f;
    draw->AddRectFilled(field_min, field_max, Col(field), field_h * 0.5f);
    draw->AddRect(field_min, field_max, Col(Mix(color::border, color::accent, 0.6f * hover), 0.9f), field_h * 0.5f, 0,
                  Dp(1));
    const ImU32 ink = Col(Mix(color::muted, color::text, hover), 0.9f);
    OverlayChevron(draw, {field_min.x + chevron * 0.55f, middle}, -1, ink);
    OverlayChevron(draw, {field_max.x - chevron * 0.55f, middle}, 1, ink);
    draw->PushClipRect({field_min.x + chevron, field_min.y}, {field_max.x - chevron, field_max.y}, true);
    DrawLabel(draw, fonts.body, {field_min.x + chevron, middle - FontSize(fonts.body) * 0.5f}, Col(color::text),
              value.c_str());
    draw->PopClipRect();
    draw->PushClipRect({at.x + pad, at.y}, {field_min.x - Dp(8), max.y}, true);
    DrawLabel(draw, fonts.label, {at.x + pad, middle - FontSize(fonts.label) * 0.5f}, Col(color::text), label);
    draw->PopClipRect();
    if (focused) OverlayFocusRing(draw, at, max, Dp(10));
    int step = 0;
    if (focused)
    {
        for (const ImGuiKey key :
             {ImGuiKey_LeftArrow, ImGuiKey_RightArrow, ImGuiKey_GamepadDpadLeft, ImGuiKey_GamepadDpadRight})
            ImGui::SetKeyOwner(key, item);
        if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow, ImGuiInputFlags_Repeat, item)
            || ImGui::IsKeyPressed(ImGuiKey_GamepadDpadLeft, ImGuiInputFlags_Repeat, item))
            step = -1;
        else if (ImGui::IsKeyPressed(ImGuiKey_RightArrow, ImGuiInputFlags_Repeat, item)
                 || ImGui::IsKeyPressed(ImGuiKey_GamepadDpadRight, ImGuiInputFlags_Repeat, item))
            step = 1;
    }
    if (pressed)
    {
        const ImVec2 mouse = ImGui::GetIO().MousePos;
        const bool back = ImGui::IsMouseReleased(ImGuiMouseButton_Left) && mouse.x < field_min.x + chevron
            && mouse.x >= field_min.x - Dp(6) && mouse.y >= field_min.y && mouse.y <= field_max.y;
        step = back ? -1 : 1;
    }
    return step;
}

bool OverlaySwitchRow(const char* id, const char* label, bool on, float width, bool interactive)
{
    const auto& fonts = launcher::CurrentFonts();
    auto* draw = ImGui::GetWindowDrawList();
    const ImVec2 at = ImGui::GetCursorScreenPos();
    const float height = Dp(kRowHeight), pad = Dp(16), track_w = Dp(40), track_h = Dp(23);
    const ImVec2 max{at.x + width, at.y + height};
    ImGuiID item;
    bool focused;
    float hover;
    const bool pressed = RowControl(id, at, width, interactive, item, focused, hover);
    ImGui::PushID(id);
    const float lit = launcher::Animate(ImGui::GetID("##on"), on, 14.0f);
    ImGui::PopID();
    const float middle = at.y + height * 0.5f;
    DrawLabel(draw, fonts.label, {at.x + pad, middle - FontSize(fonts.label) * 0.5f}, Col(color::text), label);
    const ImVec2 track{max.x - pad - track_w, middle - track_h * 0.5f};
    draw->AddRectFilled(track, {track.x + track_w, track.y + track_h},
                        Col(Mix(Mix(color::border, color::muted, 0.25f * hover), color::accent, lit)), track_h * 0.5f);
    draw->AddCircleFilled({track.x + track_h * 0.5f + (track_w - track_h) * lit, track.y + track_h * 0.5f},
                          track_h * 0.5f - Dp(3), Col(Mix(color::text, color::accent_ink, lit)));
    if (focused) OverlayFocusRing(draw, at, max, Dp(10));
    return pressed;
}

namespace
{
// The group being drawn: where its card begins.
ImVec2 sGroupTop;
} // namespace

void OverlayRowGroupBegin(const char* title, float width)
{
    const auto& fonts = launcher::CurrentFonts();
    auto* draw = ImGui::GetWindowDrawList();
    const ImVec2 at = ImGui::GetCursorScreenPos();
    const float height = Dp(38);
    launcher::DrawTracked(draw, fonts.caption, {at.x + Dp(4), at.y + height - FontSize(fonts.caption) - Dp(9)},
                          Col(color::dim), title, Dp(1.5f));
    ImGui::Dummy({width, height});
    sGroupTop = ImGui::GetCursorScreenPos();
    draw->ChannelsSplit(2);
    draw->ChannelsSetCurrent(1);
    ImGui::Dummy({width, Dp(4)});
}

void OverlayRowGroupEnd(float width)
{
    auto* draw = ImGui::GetWindowDrawList();
    ImGui::Dummy({width, Dp(4)});
    const float bottom = ImGui::GetCursorScreenPos().y;
    draw->ChannelsSetCurrent(0);
    ImVec4 fill = color::field;
    fill.w = 0.62f;
    OverlayCard(draw, sGroupTop, {sGroupTop.x + width, bottom}, Dp(16), fill, color::border);
    draw->ChannelsMerge();
}
} // namespace mscharged::overlay
