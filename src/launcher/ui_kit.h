#pragma once

#include "imgui.h"

#include <initializer_list>
#include <vector>
#include <string>

// Visual building blocks of the launcher: palette, fonts, theme and custom
// widgets. All sizes are design units (dp) converted with Dp().
namespace mscharged::launcher
{
// The Recharged logo's colours: night-blue surfaces, its letters' ice-to-sky
// gradient as the accent and their navy outline as the ink on it.
namespace color
{
inline constexpr ImVec4 window{0.027f, 0.047f, 0.098f, 1.0f};      // #070C19
inline constexpr ImVec4 sidebar{0.039f, 0.067f, 0.133f, 1.0f};     // #0A1122
inline constexpr ImVec4 card{0.055f, 0.094f, 0.188f, 1.0f};        // #0E1830
inline constexpr ImVec4 card_hover{0.075f, 0.125f, 0.239f, 1.0f};  // #13203D
inline constexpr ImVec4 field{0.078f, 0.129f, 0.251f, 1.0f};       // #142140
inline constexpr ImVec4 border{0.133f, 0.200f, 0.353f, 1.0f};      // #22335A
inline constexpr ImVec4 text{0.918f, 0.957f, 1.000f, 1.0f};        // #EAF4FF
inline constexpr ImVec4 muted{0.549f, 0.612f, 0.749f, 1.0f};       // #8C9CBF
inline constexpr ImVec4 dim{0.357f, 0.416f, 0.557f, 1.0f};         // #5B6A8E
inline constexpr ImVec4 accent{0.302f, 0.741f, 1.000f, 1.0f};      // #4DBDFF, the letters' sky blue
inline constexpr ImVec4 accent_hi{0.788f, 0.933f, 1.000f, 1.0f};   // #C9EEFF, their icy top
inline constexpr ImVec4 accent_dark{0.039f, 0.165f, 0.420f, 1.0f}; // #0A2A6B, their outline
inline constexpr ImVec4 accent_ink{0.027f, 0.125f, 0.329f, 1.0f};  // #072054, text on the accent
inline constexpr ImVec4 success{0.243f, 0.863f, 0.592f, 1.0f};     // #3EDC97
inline constexpr ImVec4 info{0.659f, 0.580f, 1.000f, 1.0f};        // #A894FF
inline constexpr ImVec4 warning{1.000f, 0.706f, 0.263f, 1.0f};     // #FFB443
inline constexpr ImVec4 danger{1.000f, 0.353f, 0.416f, 1.0f};      // #FF5A6A
inline constexpr ImVec4 discord{0.345f, 0.396f, 0.949f, 1.0f};     // #5865F2, Discord's blurple
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
    Gamepad, Copy, Link, Target, Back, GitHub, Download, Flask, Tag, Trophy, Gear, Close, Up, Down, Archive, More,
    Heart, Bug, Layers, Bulb, Code, Person, Pitch, Clipboard
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
struct NavPage
{
    const char* label;
    Icon icon;
    bool selected;
};
// A sidebar group of pages, open while one of them is shown: its row, and a
// raised panel with the pages under it. Returns 0 for a click on the group's
// row, 1 and up for one on a page, -1 for none.
int NavGroup(const char* label, Icon icon, bool open, std::initializer_list<NavPage> pages, float width);
bool PrimaryButton(const char* id, const char* label, ImVec2 size, bool enabled, Icon icon = Icon::None,
                   bool glow = false);
bool SecondaryButton(const char* id, const char* label, ImVec2 size, bool enabled = true,
                     Icon icon = Icon::None);
// Primary button with an attached segment on its right (an options gear). The
// segment stays usable while the button is disabled; `open` highlights it while
// its menu is shown and `marked` puts a dot on it (an option is on).
struct SplitSegment
{
    float width;
    Icon icon;
    bool open;
    bool marked;
};
struct SplitPress
{
    bool button = false;
    bool segment = false;
    bool button_hovered = false;
    bool segment_hovered = false;
};
SplitPress SplitPrimaryButton(const char* id, const char* label, ImVec2 size, const SplitSegment& segment,
                              bool enabled, Icon icon = Icon::None, bool glow = false);
// Dropdown menu rows (inside a popup), each `width` wide: a switch with a
// description, an action with a chevron, and a divider.
bool MenuToggle(const char* id, const char* label, const char* description, bool* value, float width);
bool MenuAction(const char* id, const char* label, float width);
void MenuSeparator(float width);
// Text button without a frame until hovered, for quiet actions next to a primary button.
bool GhostButton(const char* id, const char* label, ImVec2 size, bool enabled = true, Icon icon = Icon::None);
bool Toggle(const char* id, bool* value);
bool Segmented(const char* id, int* selected, std::initializer_list<const char*> items, float width);
bool Segmented(const char* id, int* selected, const std::vector<const char*>& items, float width);
// Segments with an icon each; the selected one takes its item's tint. `glass`
// draws a translucent track for use over artwork.
struct SegmentItem
{
    const char* label;
    Icon icon;
    ImVec4 tint;
    bool menu = false; // a small down chevron after the label: the segment opens a menu
};
// Returns true when the selection changed; `pressed` (optional) receives the
// segment clicked this frame, also the selected one, else -1.
bool SegmentedIcons(const char* id, int* selected, std::initializer_list<SegmentItem> items, ImVec2 size, bool glass,
                    int* pressed = nullptr);
// `zero_label` replaces "0%" (for example "Off"); the value stays within
// `minimum`..`maximum`.
bool PercentSlider(const char* id, int* value, float width, const char* zero_label = nullptr, int minimum = 0,
                   int maximum = 100);
void Chip(const char* text, const ImVec4& color, Icon icon = Icon::None);
// Small battery gauge (0-100%), coloured by charge, with the percentage after it.
void BatteryIcon(int percent);
void KeyCap(const char* key);
// Clickable keycap; `waiting` shows it pulsing while it waits for a key press.
bool KeyCapButton(const char* id, const char* key, bool waiting);
// Clickable keycaps of keys pressed together ("Ctrl + Shift + F12"), one
// button; `waiting` shows them pulsing while they wait for key presses.
bool KeyComboButton(const char* id, const std::vector<std::string>& keys, bool waiting);
// Keycap with a drawn arrow: 0 up, 1 down, 2 left, 3 right (no font glyphs needed).
void KeyCapArrow(int direction);
void DrawChevron(ImDrawList* draw, ImVec2 center, float size, ImU32 color);
void DrawChevronDown(ImDrawList* draw, ImVec2 center, float size, ImU32 color);
// The Discord logo, `width` wide (three quarters as tall), its eyes in `eyes`:
// the colour it sits on.
void DrawDiscordMark(ImDrawList* draw, ImVec2 center, float width, ImU32 color, ImU32 eyes);
// A button in Discord's colour with its logo, for a link to a Discord server.
bool DiscordButton(const char* id, const char* label, ImVec2 size);
// A small flag of a disc region (the disc ID's fourth letter: E USA, P Europe,
// J Japan, K Korea; grey for others), `height` tall and 1.5 times as wide.
// Always shown with the region's name: the flag alone tells nothing to some.
void DrawRegionFlag(ImDrawList* draw, ImVec2 position, float height, char region);

void BeginCard(const char* id, const char* title, const char* subtitle = nullptr, Icon icon = Icon::None,
               float width = 0.0f);
void EndCard();
// Two-column setting rows inside a card: label/description, then control.
bool BeginRows(const char* id);
void Row(const char* label, const char* description = nullptr);
// Rows that belong to the row above them, such as the options its switch
// shows: BeginSubRows after the parent row's control, then a Row for each.
// They sit indented on an inset panel, with an accent line from the parent's
// label down beside them.
void BeginSubRows();
void EndSubRows();
void EndRows();
void PageHeader(const char* title, const char* description);
void Spinner(ImVec2 center, float radius, ImU32 color);
// Letter-spaced text (overlines and small headings).
void DrawTracked(ImDrawList* draw, ImFont* font, ImVec2 position, ImU32 color, const char* text, float tracking);
float TrackedWidth(ImFont* font, const char* text, float tracking);
} // namespace mscharged::launcher
