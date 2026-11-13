#pragma once

#include "launcher/ui_kit.h"

#include "imgui.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

// The frame of the game's overlay (game_overlay.h): a window in the middle of
// the blurred picture with
//   - tabs across the top (Online, Game options, ...: OverlayTabs), with the
//     keys that switch them at both ends;
//   - a title line: a back arrow (from a tab's second screen on) and the
//     title; with tabs only on a tab's later screens (the tab names its first,
//     unless the strip is too narrow for its name);
//   - the screen's own content, which it draws between BeginOverlay and
//     EndOverlay, scrolling where it needs (BeginOverlayScroll);
//   - a footer line, the same on every screen: a note, an optional Done (the
//     screen's confirm action) and Close with its key.
// The screens sit on a stack the caller keeps (OverlayScreens). Escape, Close
// and a click beside the window close the overlay; B, Backspace and the back
// arrow go back one screen (B on the first one closes too). The keyboard and
// gamepad cursor lands on a screen's first item (or the one it prefers) when
// it opens, and back on the item that opened the next one when that goes.
// ImGui only, in the caller's frame: Aurora's in the game, SDL's in the
// launcher's preview.
namespace mscharged::overlay
{
// How the overlay sits over the game's picture.
struct OverlayStyle
{
    // Pixels per design unit (dp): the launcher's UI scale; in the game
    // OverlayUnit (overlay_unit.h): its picture's height / 900, not too small
    // to read in small windows.
    float unit = 1.0f;
    // The fonts at `unit` (AddOnlinePanelFonts): caption, body, label,
    // heading and button are used.
    launcher::Fonts fonts;
    // The picture the overlay sits over, in ImGui's display coordinates; an
    // empty area means the whole display.
    ImVec2 area_min{0.0f, 0.0f}, area_max{0.0f, 0.0f};
    // 0..1: the window rises into place (and the veil fades in) as it opens.
    float appear = 1.0f;
    // A black veil over the picture around the window (0 none), on top of the blur.
    float dim = 0.25f;
    // The key the footer names for closing ("Esc", "F1", "HOME").
    std::string close_key = "Esc";
    // The keys shown at the ends of the tabs, for the previous and the next
    // tab (a Wii Remote's host might show "-" and "+").
    std::vector<std::string> previous_tab_keys{"L1", "Q"}, next_tab_keys{"R1", "E"};
};

// Tabs across the top of the window, as the caller has them, in their order.
// The strip fits from two to about seven: each with its icon and label while
// they fit (in capitals, smaller as they need, then without icons), else the
// open tab with its label and the others as icons (their labels under the
// mouse), else icons alone (the page's title line names it); the keys that
// switch them at its ends, all of them or the first of each.
struct OverlayTabs
{
    struct Tab
    {
        int id = 0; // the caller's, which stays when tabs come and go (unlike its place)
        std::string label;
        launcher::Icon icon = launcher::Icon::None;
        // Things waiting there: a count on the tab (0: none); or, without
        // one, a dot (something new there). Neither on the open tab, whose
        // page shows them.
        int badge = 0;
        bool dot = false;
        bool available = true; // false: dimmed; neither a click nor the keys open it
        bool hidden = false;   // not in the strip at all
        std::string hint;      // under the mouse, after the label: why it isn't available
    };
    std::vector<Tab> tabs;
    int current = 0;            // the open tab's id
    std::optional<int> clicked; // set by BeginOverlay: the id of a tab the mouse picked
};

// Whether the tab `id` is in the strip and can be opened.
bool OverlayTabOpens(const std::vector<OverlayTabs::Tab>& tabs, int id);
// The id of the tab that opens `step` places after `current` (before it for
// a negative step), over the hidden and unavailable ones and round from the
// last to the first; `current` when no other opens. From a `current` not in
// the strip, the first (or last) that opens.
int OverlayTabStep(const std::vector<OverlayTabs::Tab>& tabs, int current, int step);

// The overlay's layer: an ImGui window over the style's area (an empty one
// meaning the whole display), transparent, in the overlay's style, with the
// launcher's widgets at its scale and fonts until EndOverlayLayer. `front`
// brings it in front of other windows (as it opens). BeginOverlay and the
// prompt (overlay_prompt.h) draw in one.
struct OverlayArea
{
    ImVec2 min, max;
};
OverlayArea BeginOverlayLayer(const char* id, const OverlayStyle& style, bool front);
void EndOverlayLayer();
// The overlay's window in [min, max]: its soft shadow (faded by `appear`,
// 0..1), its frosted glass with a glow of `glow` at the top, its contour lit
// from above.
void OverlayGlass(ImDrawList* draw, ImVec2 min, ImVec2 max, float rounding, float appear, const ImVec4& glow);
// A key's cap ("Esc", "A"), 22 dp tall; `lit` 0..1 brightens it.
float OverlayKeyCapWidth(const char* key);
void OverlayKeyCap(ImDrawList* draw, ImVec2 pos, const char* key, float lit = 0.0f);

// The overlay's screens, the first at the bottom; the caller keeps them
// between frames.
struct OverlayScreens
{
    struct Screen
    {
        int id = 0;        // the caller's kind of screen
        std::string key;   // which one (for the online panel, a player's row key)
        std::string title; // its title, until the screen gives a newer one
    };
    // The overlay opens on `first`, alone; its preferred (or first) item gets the cursor.
    void Open(Screen first);
    // A screen over the current one. `back_focus`: the item that gets the
    // cursor again when it goes (ImGui::GetItemID() of the item that opened
    // it), 0 for the old screen's preferred (or first) item.
    void Push(Screen screen, ImGuiID back_focus);
    // Back to the screen before; false on the first.
    bool Pop();
    bool Empty() const { return stack.empty(); }
    bool CanGoBack() const { return stack.size() > 1; }
    const Screen& Top() const { return stack.back().screen; }

    // The frame's bookkeeping.
    struct Entry
    {
        Screen screen;
        ImGuiID back_focus = 0;
    };
    std::vector<Entry> stack;
    bool focus_pending = true; // the top screen's first (or preferred) item takes the cursor
    ImGuiID restore = 0;       // or this one: the screen is back
    // An item the frame gave the cursor for want of the preferred one (the
    // lists still loading): while the cursor stays there, the preferred item
    // takes it once it shows.
    ImGuiID auto_focus = 0;
    std::uint64_t changes = 0; // each Open, Push and Pop
};

// The content's place in the window, for the screen to lay out.
struct OverlayLayout
{
    ImVec2 min, max;
};

// Begins the overlay's window for the screen on top of `screens` (Open it
// first), with its tabs (if any) and title line. The screen draws its content
// in the returned area before EndOverlay.
OverlayLayout BeginOverlay(const OverlayStyle& style, OverlayScreens& screens, const std::string& title,
                           OverlayTabs* tabs = nullptr);

// Whether the overlay's keys reach it (it has the keyboard and gamepad), from
// the frame before; for keys read before BeginOverlay.
bool OverlayHasFocus();

// Call after each item of the content the keyboard and gamepad reach: it
// takes the cursor when its screen opens (the first such item, or the first
// `preferred` one) or comes back to it.
void OverlayFocusable(bool preferred = false);

// Call with the screen's layout (say 1 for one column, 2 for two) before its
// items: when it changed (the window was resized), its items are others and
// the cursor lands on its preferred item again.
void OverlayLayoutIs(int layout);

// The tabs' two parts (Online, Game options, Settings), alike on each so that
// nothing jumps from tab to tab: the left part's width for content `width`
// wide (from 700 dp; below, one column), the gap after it, and the divider
// line in that gap.
float OverlaySplitLeft(float width);
float OverlaySplitGap();
void OverlaySplitDivider(const OverlayLayout& layout, float left);

// A scrolling part of the content in [min, max] (its scroll bar in the
// window's margin at the right); its content is `max.x - min.x` wide.
void BeginOverlayScroll(const char* id, ImVec2 min, ImVec2 max);
void EndOverlayScroll();

struct OverlayFooter
{
    std::string note;          // at the left: what the player should know
    ImVec4 note_color{0, 0, 0, 0}; // transparent: the usual dim text
    std::string done;          // the confirm button's label; empty: none
    bool done_enabled = true;
};

struct OverlayResult
{
    bool close = false; // the overlay is to close (Escape, Close, a click beside, B on the first screen)
    bool back = false;  // it went back one screen (already popped)
    bool done = false;  // Done
};

// Draws the footer, takes the keys, and ends the window.
OverlayResult EndOverlay(const OverlayFooter& footer);

// Widgets for screens, reached by navigation (with the accent ring) like the
// frame's own.
enum class OverlayLook
{
    Primary,   // the accent, filled: the step to take
    Secondary, // a field with a border
    Ghost,     // text until hovered
};
bool OverlayButton(const char* id, const char* label, launcher::Icon icon, ImVec2 size, OverlayLook look,
                   bool enabled = true, bool preferred = false);
// The item just drawn has the keyboard and gamepad cursor, and it shows.
bool OverlayNavFocused();
void OverlayFocusRing(ImDrawList* draw, ImVec2 min, ImVec2 max, float rounding);
// A rectangle from `first` to `second`, top to bottom, alpha included.
void OverlayFade(ImDrawList* draw, ImVec2 min, ImVec2 max, const ImVec4& first, const ImVec4& second, float rounding,
                 ImDrawFlags flags = 0);
// The window's own colour, for what blends into it.
ImVec4 OverlaySurface();
// A card: `fill` a little lighter at its top, and a contour of `edge` lit
// from above.
void OverlayCard(ImDrawList* draw, ImVec2 min, ImVec2 max, float rounding, const ImVec4& fill, const ImVec4& edge);

// The fonts the overlay uses, added to `atlas` from TrueType data (the
// launcher's Roboto) at `unit` pixels per dp, for a host whose atlas holds
// other fonts too. The atlas does not own the data: keep it while the atlas
// is built. Null fonts where the atlas refuses the data.
launcher::Fonts AddOnlinePanelFonts(ImFontAtlas& atlas, void* ttf, int ttf_size, float unit);
} // namespace mscharged::overlay
