#include "overlay/overlay_rows.h"

#include "launcher/ui_kit.h"

#include <algorithm>
#include <cmath>
#include <string>

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

int OverlaySegmentedRow(const char* id, const char* label, const std::vector<std::string>& options, int selected,
                        float width, bool interactive)
{
    const auto& fonts = launcher::CurrentFonts();
    auto* draw = ImGui::GetWindowDrawList();
    const ImVec2 at = ImGui::GetCursorScreenPos();
    const float height = Dp(kRowHeight), pad = Dp(12), field_h = Dp(30);
    const ImVec2 max{at.x + width, at.y + height};
    ImGuiID item;
    bool focused;
    float hover;
    const bool pressed = RowControl(id, at, width, interactive, item, focused, hover);
    const float middle = at.y + height * 0.5f;
    const int count = int(options.size());
    if (count == 0) return -1;
    // The options share the row's right part, at most so wide each.
    float widest = 0.0f;
    for (const auto& option : options) widest = std::max(widest, TextSize(fonts.body, option.c_str()).x);
    const float label_w = std::min(TextSize(fonts.label, label).x + Dp(16), width * 0.42f);
    const float each = std::min(std::max(widest + Dp(18), Dp(34)), (width - pad * 2 - label_w) / float(count));
    const ImVec2 field_min{max.x - pad - each * float(count), middle - field_h * 0.5f};
    const ImVec2 field_max{max.x - pad, middle + field_h * 0.5f};
    ImVec4 field = Mix(color::field, color::card_hover, 0.25f + 0.3f * hover);
    field.w = 0.9f;
    draw->AddRectFilled(field_min, field_max, Col(field), Dp(8));
    int hovered = -1;
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    if (interactive && mouse.y >= field_min.y && mouse.y <= field_max.y && mouse.x >= field_min.x
        && mouse.x < field_max.x)
        hovered = std::clamp(int((mouse.x - field_min.x) / each), 0, count - 1);
    for (int n = 0; n < count; ++n)
    {
        const ImVec2 min{field_min.x + float(n) * each, field_min.y};
        const ImVec2 cell_max{min.x + each, field_max.y};
        const bool chosen = n == selected;
        if (chosen)
        {
            draw->AddRectFilled({min.x + Dp(2), min.y + Dp(2)}, {cell_max.x - Dp(2), cell_max.y - Dp(2)},
                                Col(Mix(color::field, color::accent_dark, 0.7f)), Dp(6));
            draw->AddRect({min.x + Dp(2), min.y + Dp(2)}, {cell_max.x - Dp(2), cell_max.y - Dp(2)},
                          Col(color::accent, 0.7f), Dp(6), 0, Dp(1));
        }
        else if (n == hovered) draw->AddRectFilled(min, cell_max, Col(color::text, 0.05f), Dp(6));
        const ImVec2 size = TextSize(fonts.body, options[std::size_t(n)].c_str());
        DrawLabel(draw, fonts.body, {min.x + (each - size.x) * 0.5f, middle - size.y * 0.5f},
                  Col(chosen ? color::text : Mix(color::muted, color::text, n == hovered ? 0.5f : 0.0f)),
                  options[std::size_t(n)].c_str());
    }
    draw->PushClipRect({at.x + pad, at.y}, {field_min.x - Dp(8), max.y}, true);
    DrawLabel(draw, fonts.label, {at.x + pad, middle - FontSize(fonts.label) * 0.5f}, Col(color::text), label);
    draw->PopClipRect();
    if (focused) OverlayFocusRing(draw, at, max, Dp(10));
    int picked = -1;
    if (focused)
    {
        for (const ImGuiKey key :
             {ImGuiKey_LeftArrow, ImGuiKey_RightArrow, ImGuiKey_GamepadDpadLeft, ImGuiKey_GamepadDpadRight})
            ImGui::SetKeyOwner(key, item);
        if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow, ImGuiInputFlags_Repeat, item)
            || ImGui::IsKeyPressed(ImGuiKey_GamepadDpadLeft, ImGuiInputFlags_Repeat, item))
            picked = std::max(0, selected - 1);
        else if (ImGui::IsKeyPressed(ImGuiKey_RightArrow, ImGuiInputFlags_Repeat, item)
                 || ImGui::IsKeyPressed(ImGuiKey_GamepadDpadRight, ImGuiInputFlags_Repeat, item))
            picked = std::min(count - 1, selected + 1);
    }
    if (pressed)
        picked = ImGui::IsMouseReleased(ImGuiMouseButton_Left) && hovered >= 0 ? hovered : (selected + 1) % count;
    return picked == selected ? -1 : picked;
}

std::optional<int> OverlaySliderRow(const char* id, const char* label, int value, float width, bool& done)
{
    done = false;
    const auto& fonts = launcher::CurrentFonts();
    auto* draw = ImGui::GetWindowDrawList();
    const ImVec2 at = ImGui::GetCursorScreenPos();
    const float height = Dp(kRowHeight), pad = Dp(16);
    const ImVec2 max{at.x + width, at.y + height};
    ImGuiID item;
    bool focused;
    float hover;
    RowControl(id, at, width, true, item, focused, hover);
    const bool active = ImGui::IsItemActive(), released = ImGui::IsItemDeactivated();
    const float middle = at.y + height * 0.5f;
    const float value_w = TextSize(fonts.body, "100 %").x;
    const float x1 = max.x - pad - value_w - Dp(16);
    const float x0 = x1 - std::clamp(width * 0.42f, Dp(120), Dp(260));

    std::optional<int> result;
    ImGui::PushID(id);
    const ImGuiID dragging_key = ImGui::GetID("##dragging");
    ImGui::PopID();
    ImGuiStorage* storage = ImGui::GetStateStorage();
    bool dragging = storage->GetBool(dragging_key, false);
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    // A press on the track takes the knob; the drag then follows anywhere.
    if (active && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && mouse.x >= x0 - Dp(12) && mouse.x <= x1 + Dp(12))
        dragging = true;
    if (dragging && active && ImGui::IsMouseDown(ImGuiMouseButton_Left))
    {
        const int held = int(std::lround(std::clamp((mouse.x - x0) / (x1 - x0), 0.0f, 1.0f) * 100.0f));
        if (held != value) result = held;
    }
    if (dragging && (released || !active))
    {
        // Let go: the value as it is now, done.
        if (!result) result = value;
        done = true;
        dragging = false;
    }
    storage->SetBool(dragging_key, dragging);
    if (focused && !dragging)
    {
        for (const ImGuiKey key :
             {ImGuiKey_LeftArrow, ImGuiKey_RightArrow, ImGuiKey_GamepadDpadLeft, ImGuiKey_GamepadDpadRight})
            ImGui::SetKeyOwner(key, item);
        int step = 0;
        if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow, ImGuiInputFlags_Repeat, item)
            || ImGui::IsKeyPressed(ImGuiKey_GamepadDpadLeft, ImGuiInputFlags_Repeat, item))
            step = -1;
        else if (ImGui::IsKeyPressed(ImGuiKey_RightArrow, ImGuiInputFlags_Repeat, item)
                 || ImGui::IsKeyPressed(ImGuiKey_GamepadDpadRight, ImGuiInputFlags_Repeat, item))
            step = 1;
        // To the next multiple of 5 that way.
        const int next = std::clamp(step > 0 ? (value / 5 + 1) * 5 : step < 0 ? (value + 4) / 5 * 5 - 5 : value, 0, 100);
        if (step && next != value)
        {
            result = next;
            done = true;
        }
    }

    const int shown = result ? *result : value;
    const float lit = launcher::Animate(ImGui::GetID((std::string(id) + "##lit").c_str()), hover > 0.5f || dragging,
                                        14.0f);
    const float track_h = Dp(6), knob = Dp(8) + Dp(2) * lit;
    const float x = x0 + (x1 - x0) * float(shown) / 100.0f;
    draw->AddRectFilled({x0, middle - track_h * 0.5f}, {x1, middle + track_h * 0.5f},
                        Col(Mix(color::border, color::muted, 0.25f * hover)), track_h * 0.5f);
    if (shown > 0)
        draw->AddRectFilled({x0, middle - track_h * 0.5f}, {x, middle + track_h * 0.5f}, Col(color::accent),
                            track_h * 0.5f);
    draw->AddCircleFilled({x, middle}, knob, Col(color::text));
    draw->AddCircle({x, middle}, knob, Col(color::accent, 0.5f + 0.5f * lit), 0, Dp(1.5f));
    const std::string percent = std::to_string(shown) + " %";
    DrawLabel(draw, fonts.body, {max.x - pad - TextSize(fonts.body, percent.c_str()).x, middle - FontSize(fonts.body) * 0.5f},
              Col(color::text), percent.c_str());
    draw->PushClipRect({at.x + pad, at.y}, {x0 - Dp(14), max.y}, true);
    DrawLabel(draw, fonts.label, {at.x + pad, middle - FontSize(fonts.label) * 0.5f}, Col(color::text), label);
    draw->PopClipRect();
    if (focused) OverlayFocusRing(draw, at, max, Dp(10));
    return result;
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
