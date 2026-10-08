#include "launcher/ui_kit.h"

#include "imgui_internal.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace mscharged::launcher
{
namespace
{
float g_ui = 1.0f;
Fonts g_fonts;

ImVec2 Add(ImVec2 a, ImVec2 b) { return {a.x + b.x, a.y + b.y}; }
ImVec2 Sub(ImVec2 a, ImVec2 b) { return {a.x - b.x, a.y - b.y}; }
} // namespace

ImU32 Col(const ImVec4& color, float alpha)
{
    return ImGui::ColorConvertFloat4ToU32({color.x, color.y, color.z, color.w * std::clamp(alpha, 0.0f, 1.0f)});
}

ImVec4 Mix(const ImVec4& a, const ImVec4& b, float t)
{
    t = std::clamp(t, 0.0f, 1.0f);
    return {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t};
}

void SetUiScale(float ui) { g_ui = ui; }
float UiScale() { return g_ui; }

Fonts LoadFonts(const std::string& ttf, float font_raster, float density)
{
    auto& io = ImGui::GetIO();
    io.Fonts->Clear();
    // Latin-1 plus the typographic punctuation and arrows used by the launcher.
    static ImVector<ImWchar> ranges;
    if (ranges.empty())
    {
        ImFontGlyphRangesBuilder builder;
        builder.AddRanges(io.Fonts->GetGlyphRangesDefault());
        builder.AddText("\xE2\x80\x93\xE2\x80\x94\xE2\x80\xA2\xE2\x80\xA6\xE2\x86\x90\xE2\x86\x91\xE2\x86\x92"
                        "\xE2\x86\x93\xE2\x88\x92\xE2\x80\x98\xE2\x80\x99\xE2\x80\x9C\xE2\x80\x9D");
        builder.BuildRanges(&ranges);
    }
    auto add = [&](float dp) {
        ImFontConfig config;
        config.OversampleH = 2;
        config.OversampleV = 1;
        config.PixelSnapH = false;
        ImFont* font = io.Fonts->AddFontFromFileTTF(ttf.c_str(), std::round(dp * font_raster), &config, ranges.Data);
        if (!font) throw std::runtime_error("Cannot load the launcher font; rebuild to restore resources");
        return font;
    };
    Fonts fonts;
    fonts.body = add(15.0f); // first font is the ImGui default
    fonts.caption = add(13.0f);
    fonts.label = add(16.5f);
    fonts.heading = add(20.0f);
    fonts.title = add(30.0f);
    fonts.button = add(22.0f);
    fonts.overline = add(21.0f);
    fonts.hero = add(54.0f);
    io.FontDefault = fonts.body;
    io.FontGlobalScale = 1.0f / density;
    g_fonts = fonts;
    return fonts;
}

const Fonts& CurrentFonts() { return g_fonts; }

float FontSize(ImFont* font)
{
    return font->FontSize * ImGui::GetIO().FontGlobalScale;
}

void ApplyTheme(float ui)
{
    ImGuiStyle style;
    ImGui::StyleColorsDark(&style);
    style.WindowPadding = {20, 20};
    style.FramePadding = {12, 8};
    style.ItemSpacing = {10, 10};
    style.ItemInnerSpacing = {8, 6};
    style.CellPadding = {6, 7};
    style.ScrollbarSize = 10;
    style.GrabMinSize = 14;
    style.WindowRounding = 0;
    style.ChildRounding = 14;
    style.FrameRounding = 10;
    style.PopupRounding = 12;
    style.ScrollbarRounding = 10;
    style.GrabRounding = 8;
    style.WindowBorderSize = 0;
    style.ChildBorderSize = 1;
    style.PopupBorderSize = 1;
    style.FrameBorderSize = 0;
    style.SelectableTextAlign = {0.0f, 0.5f};
    auto* c = style.Colors;
    c[ImGuiCol_Text] = color::text;
    c[ImGuiCol_TextDisabled] = color::dim;
    c[ImGuiCol_WindowBg] = color::window;
    c[ImGuiCol_ChildBg] = {0, 0, 0, 0};
    c[ImGuiCol_PopupBg] = Mix(color::card, color::field, 0.5f);
    c[ImGuiCol_Border] = color::border;
    c[ImGuiCol_BorderShadow] = {0, 0, 0, 0};
    c[ImGuiCol_FrameBg] = color::field;
    c[ImGuiCol_FrameBgHovered] = Mix(color::field, color::border, 0.6f);
    c[ImGuiCol_FrameBgActive] = Mix(color::field, color::border, 0.9f);
    c[ImGuiCol_Button] = color::field;
    c[ImGuiCol_ButtonHovered] = Mix(color::field, color::border, 0.7f);
    c[ImGuiCol_ButtonActive] = Mix(color::field, color::accent, 0.25f);
    c[ImGuiCol_Header] = Mix(color::field, color::accent, 0.22f);
    c[ImGuiCol_HeaderHovered] = Mix(color::field, color::accent, 0.12f);
    c[ImGuiCol_HeaderActive] = Mix(color::field, color::accent, 0.30f);
    c[ImGuiCol_CheckMark] = color::accent;
    c[ImGuiCol_SliderGrab] = color::accent;
    c[ImGuiCol_SliderGrabActive] = color::accent_hi;
    c[ImGuiCol_ScrollbarBg] = {0, 0, 0, 0};
    c[ImGuiCol_ScrollbarGrab] = color::border;
    c[ImGuiCol_ScrollbarGrabHovered] = Mix(color::border, color::muted, 0.4f);
    c[ImGuiCol_ScrollbarGrabActive] = color::muted;
    c[ImGuiCol_Separator] = color::border;
    c[ImGuiCol_NavCursor] = color::accent;
    c[ImGuiCol_TextSelectedBg] = {color::accent.x, color::accent.y, color::accent.z, 0.28f};
    c[ImGuiCol_ModalWindowDimBg] = {0.0f, 0.0f, 0.0f, 0.62f};
    c[ImGuiCol_TableRowBg] = {0, 0, 0, 0};
    c[ImGuiCol_TableRowBgAlt] = {0, 0, 0, 0};
    c[ImGuiCol_TableBorderLight] = {0, 0, 0, 0};
    c[ImGuiCol_TableBorderStrong] = {0, 0, 0, 0};
    style.ScaleAllSizes(ui);
    ImGui::GetStyle() = style;
}

void DrawIcon(ImDrawList* draw, Icon icon, ImVec2 c, float s, ImU32 color)
{
    const float t = std::max(1.5f, s * 0.09f);
    auto p = [&](float x, float y) { return ImVec2{c.x + x * s, c.y + y * s}; };
    switch (icon)
    {
    case Icon::None: break;
    case Icon::Play:
        draw->AddTriangleFilled(p(-0.30f, -0.42f), p(-0.30f, 0.42f), p(0.44f, 0.0f), color);
        break;
    case Icon::Disc:
        draw->AddCircle(c, 0.44f * s, color, 0, t);
        draw->AddCircleFilled(c, 0.12f * s, color);
        draw->PathArcTo(c, 0.28f * s, 3.6f, 4.9f);
        draw->PathStroke(color, 0, t * 0.8f);
        break;
    case Icon::Display:
        draw->AddRect(p(-0.48f, -0.38f), p(0.48f, 0.22f), color, 0.08f * s, 0, t);
        draw->AddLine(p(0.0f, 0.22f), p(0.0f, 0.40f), color, t);
        draw->AddLine(p(-0.22f, 0.42f), p(0.22f, 0.42f), color, t);
        break;
    case Icon::Audio:
    {
        const ImVec2 body[] = {p(-0.46f, -0.15f), p(-0.24f, -0.15f), p(0.0f, -0.40f), p(0.0f, 0.40f),
                               p(-0.24f, 0.15f), p(-0.46f, 0.15f)};
        draw->AddConvexPolyFilled(body, 6, color);
        draw->PathArcTo(p(0.04f, 0.0f), 0.20f * s, -0.85f, 0.85f);
        draw->PathStroke(color, 0, t);
        draw->PathArcTo(p(0.04f, 0.0f), 0.38f * s, -0.85f, 0.85f);
        draw->PathStroke(color, 0, t);
        break;
    }
    case Icon::Keyboard:
        draw->AddRect(p(-0.50f, -0.32f), p(0.50f, 0.32f), color, 0.10f * s, 0, t);
        for (int row = 0; row < 2; ++row)
            for (int key = 0; key < 4; ++key)
            {
                const ImVec2 center = p(-0.30f + 0.20f * key, -0.12f + 0.18f * row);
                draw->AddRectFilled(Sub(center, {0.05f * s, 0.045f * s}), Add(center, {0.05f * s, 0.045f * s}),
                                    color, 0.02f * s);
            }
        draw->AddLine(p(-0.22f, 0.20f), p(0.22f, 0.20f), color, t);
        break;
    case Icon::Sliders:
    {
        const float knobs[] = {-0.15f, 0.20f, -0.05f};
        for (int i = 0; i < 3; ++i)
        {
            const float y = -0.30f + 0.30f * float(i);
            draw->AddLine(p(-0.46f, y), p(0.46f, y), color, t);
            draw->AddCircleFilled(p(knobs[i], y), 0.11f * s, color);
        }
        break;
    }
    case Icon::Info:
        draw->AddCircle(c, 0.44f * s, color, 0, t);
        draw->AddCircleFilled(p(0.0f, -0.20f), 0.065f * s, color);
        draw->AddLine(p(0.0f, -0.04f), p(0.0f, 0.24f), color, t * 1.2f);
        break;
    case Icon::Folder:
    {
        const ImVec2 outline[] = {p(-0.46f, -0.30f), p(-0.14f, -0.30f), p(-0.04f, -0.18f), p(0.46f, -0.18f),
                                  p(0.46f, 0.34f), p(-0.46f, 0.34f)};
        draw->AddPolyline(outline, 6, color, ImDrawFlags_Closed, t);
        break;
    }
    case Icon::Check:
    {
        const ImVec2 mark[] = {p(-0.34f, 0.0f), p(-0.10f, 0.24f), p(0.36f, -0.26f)};
        draw->AddPolyline(mark, 3, color, 0, t * 1.4f);
        break;
    }
    case Icon::Warning:
        draw->AddTriangle(p(0.0f, -0.42f), p(0.46f, 0.38f), p(-0.46f, 0.38f), color, t);
        draw->AddLine(p(0.0f, -0.12f), p(0.0f, 0.12f), color, t * 1.2f);
        draw->AddCircleFilled(p(0.0f, 0.25f), 0.06f * s, color);
        break;
    case Icon::Error:
        draw->AddCircle(c, 0.44f * s, color, 0, t);
        draw->AddLine(p(-0.17f, -0.17f), p(0.17f, 0.17f), color, t * 1.2f);
        draw->AddLine(p(0.17f, -0.17f), p(-0.17f, 0.17f), color, t * 1.2f);
        break;
    case Icon::Refresh:
        draw->PathArcTo(c, 0.36f * s, -0.4f, 4.4f);
        draw->PathStroke(color, 0, t);
        draw->AddTriangleFilled(p(0.18f, -0.42f), p(0.48f, -0.30f), p(0.24f, -0.06f), color);
        break;
    case Icon::Mouse:
        draw->AddRect(p(-0.28f, -0.45f), p(0.28f, 0.45f), color, 0.28f * s, 0, t);
        draw->AddLine(p(0.0f, -0.38f), p(0.0f, -0.14f), color, t);
        break;
    case Icon::Gamepad:
        draw->AddRect(p(-0.50f, -0.28f), p(0.50f, 0.30f), color, 0.26f * s, 0, t);
        draw->AddLine(p(-0.34f, 0.0f), p(-0.14f, 0.0f), color, t);
        draw->AddLine(p(-0.24f, -0.10f), p(-0.24f, 0.10f), color, t);
        draw->AddCircleFilled(p(0.18f, 0.06f), 0.06f * s, color);
        draw->AddCircleFilled(p(0.32f, -0.06f), 0.06f * s, color);
        break;
    case Icon::Copy:
        draw->AddRect(p(-0.40f, -0.30f), p(0.16f, 0.40f), color, 0.08f * s, 0, t);
        draw->AddRect(p(-0.16f, -0.44f), p(0.40f, 0.20f), color, 0.08f * s, 0, t);
        break;
    case Icon::Link:
        draw->AddRect(p(-0.42f, -0.30f), p(0.30f, 0.42f), color, 0.08f * s, 0, t);
        draw->AddLine(p(-0.04f, 0.04f), p(0.44f, -0.44f), color, t);
        draw->AddLine(p(0.14f, -0.44f), p(0.44f, -0.44f), color, t);
        draw->AddLine(p(0.44f, -0.44f), p(0.44f, -0.14f), color, t);
        break;
    }
}

float Animate(ImGuiID id, bool target, float speed)
{
    const float goal = target ? 1.0f : 0.0f;
    float* value = ImGui::GetStateStorage()->GetFloatRef(id, goal);
    *value += (goal - *value) * std::min(1.0f, ImGui::GetIO().DeltaTime * speed);
    if (std::fabs(*value - goal) < 0.002f) *value = goal;
    return *value;
}

void TextColored(ImFont* font, const ImVec4& color, const char* text)
{
    ImGui::PushFont(font);
    ImGui::PushStyleColor(ImGuiCol_Text, color);
    ImGui::TextUnformatted(text);
    ImGui::PopStyleColor();
    ImGui::PopFont();
}

void TextWrappedColored(ImFont* font, const ImVec4& color, const char* text)
{
    ImGui::PushFont(font);
    ImGui::PushStyleColor(ImGuiCol_Text, color);
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextUnformatted(text);
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
    ImGui::PopFont();
}

void DrawLabel(ImDrawList* draw, ImFont* font, ImVec2 position, ImU32 color, const char* text)
{
    draw->AddText(font, FontSize(font), position, color, text);
}

void DrawLabelShadowed(ImDrawList* draw, ImFont* font, ImVec2 position, ImU32 color, const char* text, float shadow)
{
    const float offset = Dp(shadow);
    draw->AddText(font, FontSize(font), Add(position, {offset, offset}), IM_COL32(0, 0, 0, 150), text);
    draw->AddText(font, FontSize(font), position, color, text);
}

ImVec2 TextSize(ImFont* font, const char* text)
{
    return font->CalcTextSizeA(FontSize(font), FLT_MAX, 0.0f, text);
}

void FillGradient(ImDrawList* draw, ImVec2 min, ImVec2 max, ImU32 first, ImU32 second, float rounding,
                  bool horizontal)
{
    const int start = draw->VtxBuffer.Size;
    draw->AddRectFilled(min, max, IM_COL32_WHITE, rounding);
    const int end = draw->VtxBuffer.Size;
    const ImVec2 to = horizontal ? ImVec2{max.x, min.y} : ImVec2{min.x, max.y};
    ImGui::ShadeVertsLinearColorGradientKeepAlpha(draw, start, end, min, to, first, second);
}

bool NavItem(const char* label, Icon icon, bool selected, float width)
{
    ImGui::PushID(label);
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const ImVec2 size{width, Dp(44)};
    const bool pressed = ImGui::InvisibleButton("##nav", size);
    const bool hovered = ImGui::IsItemHovered();
    const float hover = Animate(ImGui::GetID("##hover"), hovered || selected, 12.0f);
    const float active = Animate(ImGui::GetID("##active"), selected, 12.0f);
    auto* draw = ImGui::GetWindowDrawList();
    if (hover > 0.0f)
        draw->AddRectFilled(pos, Add(pos, size), Col(selected ? Mix(color::field, color::accent, 0.10f) : color::field,
                                                      hover), Dp(10));
    if (active > 0.0f)
        draw->AddRectFilled({pos.x, pos.y + Dp(11)}, {pos.x + Dp(4), pos.y + size.y - Dp(11)},
                            Col(color::accent, active), Dp(2));
    const ImVec4 tint = Mix(color::muted, selected ? color::accent : color::text, std::max(hover * 0.75f, active));
    DrawIcon(draw, icon, {pos.x + Dp(28), pos.y + size.y * 0.5f}, Dp(19), Col(tint));
    const auto& fonts = CurrentFonts();
    DrawLabel(draw, fonts.label, {pos.x + Dp(50), pos.y + (size.y - FontSize(fonts.label)) * 0.5f},
             Col(Mix(color::muted, color::text, std::max(hover, active))), label);
    ImGui::PopID();
    return pressed;
}

bool PrimaryButton(const char* id, const char* label, ImVec2 size, bool enabled, Icon icon, bool glow)
{
    ImGui::PushID(id);
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    ImGui::BeginDisabled(!enabled);
    const bool pressed = ImGui::InvisibleButton("##primary", size);
    const bool hovered = ImGui::IsItemHovered();
    const bool held = ImGui::IsItemActive();
    ImGui::EndDisabled();
    const float hover = Animate(ImGui::GetID("##hover"), hovered && enabled, 10.0f);
    auto* draw = ImGui::GetWindowDrawList();
    const float rounding = std::min(size.y * 0.5f, Dp(14));
    const ImVec2 max = Add(pos, size);
    if (glow && enabled)
    {
        const float pulse = 0.5f + 0.5f * std::sin(float(ImGui::GetTime()) * 2.2f);
        for (int i = 0; i < 6; ++i)
        {
            const float grow = Dp(2.0f + 2.6f * float(i));
            const float alpha = (0.085f - 0.012f * float(i)) * (0.55f + 0.30f * pulse + 0.45f * hover);
            draw->AddRectFilled(Sub(pos, {grow, grow}), Add(max, {grow, grow}), Col(color::accent, alpha),
                                rounding + grow);
        }
    }
    if (enabled)
    {
        const float light = held ? -0.10f : 0.12f * hover;
        const ImVec4 a = Mix(color::accent_hi, ImVec4{1, 1, 1, 1}, std::max(0.0f, light));
        const ImVec4 b = Mix(color::accent, light < 0 ? color::accent_dark : ImVec4{1, 1, 1, 1}, std::fabs(light));
        FillGradient(draw, pos, max, Col(a), Col(b), rounding, true);
        draw->AddRect(pos, max, IM_COL32(255, 255, 255, 46), rounding, 0, Dp(1));
    }
    else
    {
        draw->AddRectFilled(pos, max, Col(color::field), rounding);
        draw->AddRect(pos, max, Col(color::border), rounding, 0, Dp(1));
    }
    const auto& fonts = CurrentFonts();
    ImFont* font = size.y >= Dp(52) ? fonts.button : fonts.label;
    const ImVec2 text = TextSize(font, label);
    const float icon_size = icon == Icon::None ? 0.0f : FontSize(font) * 0.82f;
    const float gap = icon == Icon::None ? 0.0f : Dp(10);
    const float left = pos.x + (size.x - (text.x + icon_size + gap)) * 0.5f;
    const ImU32 ink = enabled ? IM_COL32(6, 33, 15, 255) : Col(color::dim);
    if (icon != Icon::None)
        DrawIcon(draw, icon, {left + icon_size * 0.5f, pos.y + size.y * 0.5f}, icon_size, ink);
    DrawLabel(draw, font, {left + icon_size + gap, pos.y + (size.y - text.y) * 0.5f}, ink, label);
    ImGui::PopID();
    return pressed && enabled;
}

bool SecondaryButton(const char* id, const char* label, ImVec2 size, bool enabled, Icon icon)
{
    ImGui::PushID(id);
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    ImGui::BeginDisabled(!enabled);
    const bool pressed = ImGui::InvisibleButton("##secondary", size);
    const bool hovered = ImGui::IsItemHovered();
    const bool held = ImGui::IsItemActive();
    ImGui::EndDisabled();
    const float hover = Animate(ImGui::GetID("##hover"), hovered && enabled, 12.0f);
    auto* draw = ImGui::GetWindowDrawList();
    const float rounding = std::min(size.y * 0.5f, Dp(10));
    const ImVec2 max = Add(pos, size);
    ImVec4 fill = Mix(color::field, Mix(color::field, color::border, 0.9f), hover);
    if (held) fill = Mix(color::field, color::accent, 0.18f);
    draw->AddRectFilled(pos, max, Col(fill, enabled ? 1.0f : 0.5f), rounding);
    draw->AddRect(pos, max, Col(Mix(color::border, color::muted, 0.4f * hover)), rounding, 0, Dp(1));
    const auto& fonts = CurrentFonts();
    const ImVec2 text = TextSize(fonts.body, label);
    const float icon_size = icon == Icon::None ? 0.0f : Dp(16);
    const float gap = icon == Icon::None ? 0.0f : Dp(8);
    const float left = pos.x + (size.x - (text.x + icon_size + gap)) * 0.5f;
    const ImU32 ink = Col(enabled ? Mix(color::muted, color::text, 0.6f + 0.4f * hover) : color::dim);
    if (icon != Icon::None) DrawIcon(draw, icon, {left + icon_size * 0.5f, pos.y + size.y * 0.5f}, icon_size, ink);
    DrawLabel(draw, fonts.body, {left + icon_size + gap, pos.y + (size.y - text.y) * 0.5f}, ink, label);
    ImGui::PopID();
    return pressed && enabled;
}

bool Toggle(const char* id, bool* value)
{
    ImGui::PushID(id);
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const ImVec2 size = Dp(48, 28);
    const bool pressed = ImGui::InvisibleButton("##toggle", size);
    if (pressed) *value = !*value;
    const bool hovered = ImGui::IsItemHovered();
    const float on = Animate(ImGui::GetID("##on"), *value, 14.0f);
    const float hover = Animate(ImGui::GetID("##hover"), hovered, 12.0f);
    auto* draw = ImGui::GetWindowDrawList();
    const ImVec4 off_track = Mix(color::field, color::border, 0.6f + 0.4f * hover);
    const ImVec4 on_track = Mix(color::accent, color::accent_hi, 0.25f * hover);
    draw->AddRectFilled(pos, Add(pos, size), Col(Mix(off_track, on_track, on)), size.y * 0.5f);
    const float radius = size.y * 0.5f - Dp(3.5f);
    const ImVec2 knob{pos.x + size.y * 0.5f + on * (size.x - size.y), pos.y + size.y * 0.5f};
    draw->AddCircleFilled({knob.x, knob.y + Dp(1)}, radius, IM_COL32(0, 0, 0, 70));
    draw->AddCircleFilled(knob, radius, Col(Mix(color::muted, ImVec4{1, 1, 1, 1}, std::max(on, 0.4f * hover))));
    ImGui::PopID();
    return pressed;
}

bool Segmented(const char* id, int* selected, std::initializer_list<const char*> items, float width)
{
    ImGui::PushID(id);
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const float height = Dp(38);
    const int count = int(items.size());
    const float segment = width / float(std::max(count, 1));
    auto* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(pos, {pos.x + width, pos.y + height}, Col(color::field), Dp(11));
    float* slide = ImGui::GetStateStorage()->GetFloatRef(ImGui::GetID("##slide"), float(*selected));
    if (*selected >= 0)
    {
        *slide += (float(*selected) - *slide) * std::min(1.0f, ImGui::GetIO().DeltaTime * 16.0f);
        const ImVec2 a{pos.x + *slide * segment + Dp(3), pos.y + Dp(3)};
        const ImVec2 b{a.x + segment - Dp(6), pos.y + height - Dp(3)};
        draw->AddRectFilled(a, b, Col(Mix(color::card, color::accent, 0.16f)), Dp(8));
        draw->AddRect(a, b, Col(color::accent, 0.55f), Dp(8), 0, Dp(1));
    }
    bool changed = false;
    int index = 0;
    const auto& fonts = CurrentFonts();
    for (const char* label : items)
    {
        ImGui::SetCursorScreenPos({pos.x + segment * float(index), pos.y});
        ImGui::PushID(index);
        if (ImGui::InvisibleButton("##segment", {segment, height}) && *selected != index)
        {
            *selected = index;
            changed = true;
        }
        const float hover = Animate(ImGui::GetID("##hover"), ImGui::IsItemHovered(), 12.0f);
        const ImVec2 text = TextSize(fonts.body, label);
        const ImVec4 ink = index == *selected ? color::text : Mix(color::muted, color::text, hover * 0.7f);
        DrawLabel(draw, fonts.body, {pos.x + segment * float(index) + (segment - text.x) * 0.5f,
                                    pos.y + (height - text.y) * 0.5f}, Col(ink), label);
        ImGui::PopID();
        ++index;
    }
    ImGui::SetCursorScreenPos(pos);
    ImGui::Dummy({width, height});
    ImGui::PopID();
    return changed;
}

bool PercentSlider(const char* id, int* value, float width)
{
    ImGui::PushID(id);
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const float height = ImGui::GetFrameHeight();
    auto* draw = ImGui::GetWindowDrawList();
    const float fraction = std::clamp(float(*value) / 100.0f, 0.0f, 1.0f);
    draw->AddRectFilled(pos, {pos.x + width, pos.y + height}, Col(color::field), Dp(10));
    if (fraction > 0.0f)
        FillGradient(draw, pos, {pos.x + std::max(Dp(20), width * fraction), pos.y + height},
                     Col(Mix(color::accent_dark, color::accent, 0.55f)), Col(Mix(color::accent_dark, color::accent, 0.9f)),
                     Dp(10), true);
    ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4{0, 0, 0, 0});
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4{1, 1, 1, 0.04f});
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, ImVec4{1, 1, 1, 0.06f});
    ImGui::PushStyleColor(ImGuiCol_SliderGrab, ImVec4{1, 1, 1, 0.92f});
    ImGui::PushStyleColor(ImGuiCol_SliderGrabActive, ImVec4{1, 1, 1, 1});
    ImGui::PushStyleVar(ImGuiStyleVar_GrabMinSize, Dp(6));
    ImGui::SetNextItemWidth(width);
    const bool changed = ImGui::SliderInt("##slider", value, 0, 100, "%d%%", ImGuiSliderFlags_AlwaysClamp);
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(5);
    ImGui::PopID();
    return changed;
}

void Chip(const char* text, const ImVec4& tint, Icon icon)
{
    const auto& fonts = CurrentFonts();
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const ImVec2 label = TextSize(fonts.caption, text);
    const float icon_size = icon == Icon::None ? 0.0f : Dp(13);
    const float gap = icon == Icon::None ? 0.0f : Dp(6);
    const ImVec2 size{label.x + icon_size + gap + Dp(20), Dp(26)};
    auto* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(pos, Add(pos, size), Col(tint, 0.14f), size.y * 0.5f);
    draw->AddRect(pos, Add(pos, size), Col(tint, 0.35f), size.y * 0.5f, 0, Dp(1));
    if (icon != Icon::None)
        DrawIcon(draw, icon, {pos.x + Dp(10) + icon_size * 0.5f, pos.y + size.y * 0.5f}, icon_size, Col(tint));
    DrawLabel(draw, fonts.caption, {pos.x + Dp(10) + icon_size + gap, pos.y + (size.y - label.y) * 0.5f}, Col(tint), text);
    ImGui::Dummy(size);
}

void KeyCap(const char* key)
{
    const auto& fonts = CurrentFonts();
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const ImVec2 label = TextSize(fonts.caption, key);
    const ImVec2 size{std::max(Dp(30), label.x + Dp(18)), Dp(28)};
    auto* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled({pos.x, pos.y + Dp(2)}, Add(pos, {size.x, size.y + Dp(2)}), Col(color::window), Dp(7));
    draw->AddRectFilled(pos, Add(pos, size), Col(Mix(color::field, color::border, 0.5f)), Dp(7));
    draw->AddRect(pos, Add(pos, size), Col(color::border), Dp(7), 0, Dp(1));
    DrawLabel(draw, fonts.caption, {pos.x + (size.x - label.x) * 0.5f, pos.y + (size.y - label.y) * 0.5f},
             Col(color::text), key);
    ImGui::Dummy({size.x, size.y + Dp(2)});
}

void KeyCapArrow(int direction)
{
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const ImVec2 size{Dp(30), Dp(28)};
    auto* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled({pos.x, pos.y + Dp(2)}, Add(pos, {size.x, size.y + Dp(2)}), Col(color::window), Dp(7));
    draw->AddRectFilled(pos, Add(pos, size), Col(Mix(color::field, color::border, 0.5f)), Dp(7));
    draw->AddRect(pos, Add(pos, size), Col(color::border), Dp(7), 0, Dp(1));
    const ImVec2 c{pos.x + size.x * 0.5f, pos.y + size.y * 0.5f};
    const float r = Dp(5);
    ImVec2 a, b, tip;
    switch (direction)
    {
    case 0: tip = {c.x, c.y - r}; a = {c.x - r, c.y + r * 0.6f}; b = {c.x + r, c.y + r * 0.6f}; break;
    case 1: tip = {c.x, c.y + r}; a = {c.x + r, c.y - r * 0.6f}; b = {c.x - r, c.y - r * 0.6f}; break;
    case 2: tip = {c.x - r, c.y}; a = {c.x + r * 0.6f, c.y + r}; b = {c.x + r * 0.6f, c.y - r}; break;
    default: tip = {c.x + r, c.y}; a = {c.x - r * 0.6f, c.y - r}; b = {c.x - r * 0.6f, c.y + r}; break;
    }
    draw->AddTriangleFilled(tip, a, b, Col(color::text));
    ImGui::Dummy({size.x, size.y + Dp(2)});
}

void DrawChevron(ImDrawList* draw, ImVec2 center, float size, ImU32 color)
{
    const ImVec2 points[] = {{center.x - size * 0.2f, center.y - size * 0.4f}, {center.x + size * 0.2f, center.y},
                             {center.x - size * 0.2f, center.y + size * 0.4f}};
    draw->AddPolyline(points, 3, color, 0, std::max(1.5f, size * 0.14f));
}

void BeginCard(const char* id, const char* title, const char* subtitle, Icon icon, float width)
{
    ImGui::PushStyleColor(ImGuiCol_ChildBg, color::card);
    ImGui::PushStyleColor(ImGuiCol_Border, color::border);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, Dp(16));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, Dp(22, 20));
    ImGui::BeginChild(id, {width, 0}, ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY
        | ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_NoScrollbar);
    const auto& fonts = CurrentFonts();
    if (title)
    {
        const ImVec2 pos = ImGui::GetCursorScreenPos();
        float indent = 0.0f;
        if (icon != Icon::None)
        {
            auto* draw = ImGui::GetWindowDrawList();
            const float box = Dp(34);
            draw->AddRectFilled(pos, {pos.x + box, pos.y + box}, Col(color::accent, 0.12f), Dp(10));
            DrawIcon(draw, icon, {pos.x + box * 0.5f, pos.y + box * 0.5f}, Dp(18), Col(color::accent));
            indent = box + Dp(14);
        }
        ImGui::SetCursorScreenPos({pos.x + indent, pos.y + (subtitle ? 0.0f : Dp(4))});
        ImGui::BeginGroup();
        TextColored(fonts.heading, color::text, title);
        if (subtitle)
        {
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() - Dp(6));
            TextWrappedColored(fonts.caption, color::muted, subtitle);
        }
        ImGui::EndGroup();
        ImGui::Dummy({0, Dp(6)});
    }
}

void EndCard()
{
    ImGui::EndChild();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(2);
    ImGui::Dummy({0, Dp(8)});
}

bool BeginRows(const char* id)
{
    const float available = ImGui::GetContentRegionAvail().x;
    const float control = std::clamp(available * 0.46f, Dp(220), Dp(380));
    ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, Dp(0, 9));
    if (!ImGui::BeginTable(id, 2, ImGuiTableFlags_None))
    {
        ImGui::PopStyleVar();
        return false;
    }
    ImGui::TableSetupColumn("label", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("control", ImGuiTableColumnFlags_WidthFixed, control);
    return true;
}

void Row(const char* label, const char* description)
{
    const auto& fonts = CurrentFonts();
    ImGui::TableNextRow(ImGuiTableRowFlags_None, Dp(40));
    ImGui::TableSetColumnIndex(0);
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - Dp(24));
    TextColored(fonts.label, color::text, label);
    if (description)
    {
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() - Dp(6));
        ImGui::PushFont(fonts.caption);
        ImGui::PushStyleColor(ImGuiCol_Text, color::muted);
        ImGui::TextWrapped("%s", description);
        ImGui::PopStyleColor();
        ImGui::PopFont();
    }
    ImGui::PopTextWrapPos();
    ImGui::TableSetColumnIndex(1);
}

void EndRows()
{
    ImGui::EndTable();
    ImGui::PopStyleVar();
}

void PageHeader(const char* title, const char* description)
{
    const auto& fonts = CurrentFonts();
    TextColored(fonts.title, color::text, title);
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() - Dp(4));
    TextWrappedColored(fonts.body, color::muted, description);
    ImGui::Dummy({0, Dp(8)});
}
void Spinner(ImVec2 center, float radius, ImU32 color)
{
    auto* draw = ImGui::GetWindowDrawList();
    const float start = float(ImGui::GetTime()) * 6.0f;
    draw->PathArcTo(center, radius, start, start + 4.2f, 24);
    draw->PathStroke(color, 0, std::max(1.5f, radius * 0.22f));
}

void DrawTracked(ImDrawList* draw, ImFont* font, ImVec2 position, ImU32 color, const char* text, float tracking)
{
    const float size = FontSize(font);
    char glyph[2] = {0, 0};
    for (const char* c = text; *c; ++c)
    {
        glyph[0] = *c;
        draw->AddText(font, size, position, color, glyph);
        position.x += font->CalcTextSizeA(size, FLT_MAX, 0.0f, glyph).x + tracking;
    }
}

float TrackedWidth(ImFont* font, const char* text, float tracking)
{
    const float size = FontSize(font);
    float width = 0.0f;
    char glyph[2] = {0, 0};
    for (const char* c = text; *c; ++c)
    {
        glyph[0] = *c;
        width += font->CalcTextSizeA(size, FLT_MAX, 0.0f, glyph).x + tracking;
    }
    return text[0] ? width - tracking : 0.0f;
}
} // namespace mscharged::launcher
