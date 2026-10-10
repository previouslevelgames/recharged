#pragma once

#include "imgui.h"

#include <initializer_list>
#include <string>

// Visual building blocks of the launcher: palette, fonts, theme and custom
// widgets. All sizes are design units (dp) converted with Dp().
namespace mscharged::launcher
{
namespace color
{
inline constexpr ImVec4 window{0.039f, 0.055f, 0.090f, 1.0f};      // #0A0E17
inline constexpr ImVec4 sidebar{0.051f, 0.071f, 0.118f, 1.0f};     // #0D121E
inline constexpr ImVec4 card{0.071f, 0.098f, 0.161f, 1.0f};        // #121929
inline constexpr ImVec4 card_hover{0.090f, 0.125f, 0.200f, 1.0f};
inline constexpr ImVec4 field{0.094f, 0.129f, 0.208f, 1.0f};       // #182135
inline constexpr ImVec4 border{0.141f, 0.188f, 0.282f, 1.0f};      // #243048
inline constexpr ImVec4 text{0.918f, 0.941f, 0.980f, 1.0f};        // #EAF0FA
inline constexpr ImVec4 muted{0.545f, 0.592f, 0.690f, 1.0f};       // #8B97B0
inline constexpr ImVec4 dim{0.369f, 0.416f, 0.514f, 1.0f};         // #5E6A83
inline constexpr ImVec4 accent{0.298f, 0.941f, 0.478f, 1.0f};      // #4CF07A
inline constexpr ImVec4 accent_hi{0.714f, 1.000f, 0.169f, 1.0f};   // #B6FF2B
inline constexpr ImVec4 accent_dark{0.039f, 0.196f, 0.086f, 1.0f};
inline constexpr ImVec4 info{0.227f, 0.690f, 1.000f, 1.0f};        // #3AB0FF
inline constexpr ImVec4 warning{1.000f, 0.706f, 0.263f, 1.0f};     // #FFB443
inline constexpr ImVec4 danger{1.000f, 0.353f, 0.416f, 1.0f};      // #FF5A6A
} // namespace color

ImU32 Col(const ImVec4& color, float alpha = 1.0f);
ImVec4 Mix(const ImVec4& a, const ImVec4& b, float t);

// Current design-unit scale, set once per frame by the launcher.
void SetUiScale(float ui);
float UiScale();
inline float Dp(float value) { return value * UiScale(); }
inline ImVec2 Dp(float x, float y) { return {x * UiScale(), y * UiScale()}; }

struct Fonts
{
    ImFont* caption = nullptr;  // 13 dp
    ImFont* body = nullptr;     // 15 dp
    ImFont* label = nullptr;    // 16.5 dp
    ImFont* heading = nullptr;  // 20 dp
    ImFont* title = nullptr;    // 30 dp
    ImFont* button = nullptr;   // 22 dp
    ImFont* overline = nullptr; // 21 dp
    ImFont* hero = nullptr;     // 54 dp
};
// Rasterizes Roboto at the exact framebuffer pixel size for `font_raster`
// pixels per dp; io.FontGlobalScale maps the glyphs back to window units.
Fonts LoadFonts(const std::string& ttf, float font_raster, float density);
const Fonts& CurrentFonts();
float FontSize(ImFont* font); // in window units, including FontGlobalScale

// Theme for the current scale (call on a fresh style when the scale changes).
void ApplyTheme(float ui);

enum class Icon
{
    None, Play, Disc, Display, Audio, Keyboard, Sliders, Info, Folder, Check, Warning, Error, Refresh, Mouse,
    Gamepad, Copy, Link, Target, Back, GitHub
};
void DrawIcon(ImDrawList* draw, Icon icon, ImVec2 center, float size, ImU32 color);

// Animated 0..1 value bound to an ImGui ID (hover/selection transitions).
float Animate(ImGuiID id, bool target, float speed = 10.0f);

void TextColored(ImFont* font, const ImVec4& color, const char* text);
void TextWrappedColored(ImFont* font, const ImVec4& color, const char* text);
void DrawLabel(ImDrawList* draw, ImFont* font, ImVec2 position, ImU32 color, const char* text);
void DrawLabelShadowed(ImDrawList* draw, ImFont* font, ImVec2 position, ImU32 color, const char* text,
                      float shadow = 2.0f);
ImVec2 TextSize(ImFont* font, const char* text);

// Rounded rectangle with a vertical or horizontal two-colour gradient.
void FillGradient(ImDrawList* draw, ImVec2 min, ImVec2 max, ImU32 top_left, ImU32 bottom_right,
                  float rounding, bool horizontal);

bool NavItem(const char* label, Icon icon, bool selected, float width);
bool PrimaryButton(const char* id, const char* label, ImVec2 size, bool enabled, Icon icon = Icon::None,
                   bool glow = false);
bool SecondaryButton(const char* id, const char* label, ImVec2 size, bool enabled = true,
                     Icon icon = Icon::None);
bool Toggle(const char* id, bool* value);
bool Segmented(const char* id, int* selected, std::initializer_list<const char*> items, float width);
bool PercentSlider(const char* id, int* value, float width);
void Chip(const char* text, const ImVec4& color, Icon icon = Icon::None);
// Small battery gauge (0-100%), coloured by charge, with the percentage after it.
void BatteryIcon(int percent);
void KeyCap(const char* key);
// Clickable keycap; `waiting` shows it pulsing while it waits for a key press.
bool KeyCapButton(const char* id, const char* key, bool waiting);
// Keycap with a drawn arrow: 0 up, 1 down, 2 left, 3 right (no font glyphs needed).
void KeyCapArrow(int direction);
void DrawChevron(ImDrawList* draw, ImVec2 center, float size, ImU32 color);

void BeginCard(const char* id, const char* title, const char* subtitle = nullptr, Icon icon = Icon::None,
               float width = 0.0f);
void EndCard();
// Two-column setting rows inside a card: label/description, then control.
bool BeginRows(const char* id);
void Row(const char* label, const char* description = nullptr);
void EndRows();
void PageHeader(const char* title, const char* description);
void Spinner(ImVec2 center, float radius, ImU32 color);
// Letter-spaced text (overlines and small headings).
void DrawTracked(ImDrawList* draw, ImFont* font, ImVec2 position, ImU32 color, const char* text, float tracking);
float TrackedWidth(ImFont* font, const char* text, float tracking);
} // namespace mscharged::launcher
