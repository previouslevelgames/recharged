#include "overlay/overlay_frame.h"

#include "imgui_internal.h"

#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <cstring>
#include <iterator>
#include <string>
#include <utility>

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

ImVec2 Add(ImVec2 a, ImVec2 b) { return {a.x + b.x, a.y + b.y}; }

// Each font where none was given: the body font, or ImGui's.
launcher::Fonts Complete(launcher::Fonts fonts)
{
    if (!fonts.body) fonts.body = ImGui::GetFont();
    for (ImFont** font : {&fonts.caption, &fonts.label, &fonts.heading, &fonts.title, &fonts.button, &fonts.overline})
        if (!*font) *font = fonts.body;
    return fonts;
}

// The overlay being drawn, between BeginOverlay and EndOverlay.
struct Current
{
    bool active = false;
    OverlayScreens* screens = nullptr;
    std::uint64_t changes = 0; // the stack as it began: claims only for that screen
    ImVec2 area_min, area_max, window_min, window_max;
    float footer_top = 0.0f;
    std::string close_key;
    bool back_pressed = false;
    // The cursor: taken this frame, or the first item that could take it.
    bool claimed = false;
    ImGuiID first_id = 0, preferred_id = 0;
    ImGuiWindow* first_window = nullptr;
    ImGuiWindow* preferred_window = nullptr;
    // Scrolling parts: their visible width.
    std::vector<float> scroll_widths;
};
Current g;

constexpr int kStyleVars = 9;
constexpr const char* kWindowName = "##recharged_overlay";
constexpr int kStyleColors = 10;

// The vertices drawn since `start` coloured from `top` to `bottom` by their
// height, keeping the shape's own anti-aliased edge: its alpha multiplies.
void ShadeVertical(ImDrawList* draw, int start, float y0, float y1, const ImVec4& top, const ImVec4& bottom)
{
    const float height = std::max(1.0f, y1 - y0);
    for (int i = start; i < draw->VtxBuffer.Size; ++i)
    {
        ImDrawVert& vertex = draw->VtxBuffer[i];
        ImVec4 tint = Mix(top, bottom, (vertex.pos.y - y0) / height);
        tint.w *= float((vertex.col >> IM_COL32_A_SHIFT) & 0xFF) / 255.0f;
        vertex.col = ImGui::ColorConvertFloat4ToU32(tint);
    }
}

// Lines and outlines anti-aliased by their geometry, not by ImGui's baked line
// texture: that one needs a renderer that filters the font atlas linearly
// (SDL's software renderer doesn't, and draws them jagged).
void SmoothLines()
{
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->Flags = (draw->Flags | ImDrawListFlags_AntiAliasedLines | ImDrawListFlags_AntiAliasedFill)
        & ~ImDrawListFlags_AntiAliasedLinesUseTex;
}

bool CanClaim()
{
    return g.active && g.screens && g.screens->changes == g.changes
        && (g.screens->focus_pending || g.screens->restore != 0);
}

// `fallback`: not the item the screen prefers (for want of it).
void Claim(ImGuiID id, ImGuiWindow* window, bool fallback = false)
{
    ImGui::SetFocusID(id, window);
    ImGui::SetNavCursorVisible(true);
    g.claimed = true;
    g.screens->focus_pending = false;
    g.screens->restore = 0;
    g.screens->auto_focus = fallback ? id : 0;
}

// The key cap of the footer's Close.
float KeyCapWidth(const launcher::Fonts& fonts, const char* key)
{
    return std::max(Dp(26), TextSize(fonts.caption, key).x + Dp(14));
}

void DrawKeyCap(const launcher::Fonts& fonts, ImDrawList* draw, ImVec2 pos, const char* key, float lit)
{
    const ImVec2 text = TextSize(fonts.caption, key);
    const ImVec2 max{pos.x + KeyCapWidth(fonts, key), pos.y + Dp(22)};
    draw->AddRectFilled(pos, max, Col(Mix(color::field, color::border, lit)), Dp(6));
    draw->AddRect(pos, max, Col(Mix(color::border, color::muted, 0.5f * lit)), Dp(6), 0, Dp(1));
    draw->AddRectFilled({pos.x + Dp(1), max.y - Dp(3)}, {max.x - Dp(1), max.y - Dp(1)}, Col(color::border, 0.6f),
                        Dp(4));
    DrawLabel(draw, fonts.caption, {pos.x + (max.x - pos.x - text.x) * 0.5f, pos.y + (Dp(20) - text.y) * 0.5f},
              Col(color::text), key);
}

// A parallelogram leaning right in [min, max], its top edge `slant` right of
// its bottom edge's ends.
void Slanted(ImVec2 out[4], ImVec2 min, ImVec2 max, float slant)
{
    out[0] = {min.x + slant, min.y};
    out[1] = {max.x + slant, min.y};
    out[2] = {max.x - slant, max.y};
    out[3] = {min.x - slant, max.y};
}

// A tab's badge: its count in a red pill, or a dot; 0 for none.
float BadgeWidth(const launcher::Fonts& fonts, const OverlayTabs::Tab& tab)
{
    if (tab.badge > 0)
    {
        const std::string text = tab.badge > 9 ? "9+" : std::to_string(tab.badge);
        return std::max(Dp(18), TextSize(fonts.caption, text.c_str()).x + Dp(10));
    }
    return tab.dot ? Dp(9) : 0.0f;
}

// The badge centred on `center`, ringed in the bar's dark so that it stands
// out on the open tab's block too; `alpha` fades it with a dimmed tab.
void DrawBadge(ImDrawList* draw, const launcher::Fonts& fonts, ImVec2 center, const OverlayTabs::Tab& tab,
               float alpha)
{
    const float width = BadgeWidth(fonts, tab);
    if (width <= 0.0f) return;
    const float height = tab.badge > 0 ? Dp(18) : Dp(9), ring = Dp(2);
    const ImVec2 min{center.x - width * 0.5f, center.y - height * 0.5f};
    const ImVec2 max{center.x + width * 0.5f, center.y + height * 0.5f};
    draw->AddRectFilled({min.x - ring, min.y - ring}, {max.x + ring, max.y + ring}, Col(color::window, 0.92f * alpha),
                        height * 0.5f + ring);
    const ImVec4 top = Mix(color::danger, ImVec4{1, 1, 1, 1}, 0.18f);
    const int start = draw->VtxBuffer.Size;
    draw->AddRectFilled(min, max, IM_COL32_WHITE, height * 0.5f);
    ShadeVertical(draw, start, min.y, max.y, ImVec4{top.x, top.y, top.z, alpha},
                  ImVec4{color::danger.x, color::danger.y, color::danger.z, alpha});
    if (tab.badge <= 0) return;
    const std::string text = tab.badge > 9 ? "9+" : std::to_string(tab.badge);
    const ImVec2 size = TextSize(fonts.caption, text.c_str());
    DrawLabel(draw, fonts.caption, {center.x - size.x * 0.5f, center.y - FontSize(fonts.caption) * 0.5f},
              Col(ImVec4{1, 1, 1, 1}, alpha), text.c_str());
}

// How the strip draws its tabs: the first of these that fits the bar.
struct TabLook
{
    ImFont* font;
    float tracking, icon, gap, pad;
    bool icons;    // the tabs other than the open one show their icon alone
    bool all_keys; // every key at the ends, else the first of each (L1, R1)
    bool no_icons = false; // the labels alone
};

// Where a tab sits in the bar.
struct TabCell
{
    float x = 0.0f, width = 0.0f;
    float content = 0.0f; // its icon, label and badge side by side (the label in capitals)
    float label = 0.0f;   // the label's width, 0 for none
    // With icons for the others: its width as an icon and with its label.
    float folded = 0.0f, unfolded = 0.0f;
};

// The tabs: a bar of frosted glass across the top of the window, the tabs
// over it in tracked capitals with their icons, the open one a slanted block
// of the accent with a glow, a sheen and a bright contour, sliding to the tab
// opened; at its ends the keys that switch them; a line of the accent under
// it. Few tabs share the bar in equal parts; more take their own widths, in
// smaller letters; when they don't fit, the open tab keeps its label and the
// others show their icon (their label under the mouse); narrower still, all
// of them their icon alone (`named` false: the open tab doesn't show its
// name). For the mouse (the keys switch them). Its height.
float DrawTabs(const OverlayStyle& style, const launcher::Fonts& fonts, OverlayTabs& tabs, float top, bool& named)
{
    auto* draw = ImGui::GetWindowDrawList();
    const float height = Dp(58), rounding = Dp(22), line = Dp(2);
    const ImVec2 bar_min{g.window_min.x, top}, bar_max{g.window_max.x, top + height};
    {
        // The glass: darker than the window, lighter at its top, a shade
        // above the line under it.
        ImVec4 upper = Mix(color::window, color::card, 0.55f), lower = color::window;
        upper.w = 0.42f;
        lower.w = 0.72f;
        OverlayFade(draw, bar_min, bar_max, upper, lower, rounding, ImDrawFlags_RoundCornersTop);
        ImVec4 clear{0, 0, 0, 0}, shade{0, 0, 0, 0.30f};
        OverlayFade(draw, {bar_min.x, bar_max.y - Dp(14)}, bar_max, clear, shade, 0.0f);
    }
    const float middle = top + height * 0.5f;

    // The tabs in the strip, and which of them is open.
    std::vector<const OverlayTabs::Tab*> shown;
    int open = -1;
    for (const auto& tab : tabs.tabs)
    {
        if (tab.hidden) continue;
        if (tab.id == tabs.current) open = int(shown.size());
        shown.push_back(&tab);
    }
    const int count = int(shown.size());
    std::vector<std::string> labels(shown.size());
    for (int n = 0; n < count; ++n)
    {
        labels[n] = shown[n]->label;
        for (auto& c : labels[n]) c = char(std::toupper(static_cast<unsigned char>(c)));
    }

    // The keys at the ends (none for a single tab): their width, all or the first.
    const auto caps = [&](const std::vector<std::string>& keys, bool all) {
        float width = 0.0f;
        for (std::size_t k = 0; k < keys.size() && (all || k == 0); ++k)
            width += KeyCapWidth(fonts, keys[k].c_str()) + Dp(5);
        return keys.empty() ? 0.0f : width - Dp(5);
    };
    const float edge = Dp(20), chevron = Dp(14), clear = Dp(14);
    const auto ends = [&](bool all) {
        if (count < 2) return Dp(4);
        return std::max(caps(style.previous_tab_keys, all), caps(style.next_tab_keys, all)) + chevron + clear;
    };

    // The first look that fits, and the tabs' cells in it.
    const TabLook looks[] = {
        {fonts.heading, Dp(1.8f), Dp(19), Dp(10), Dp(22), false, true},
        {fonts.label, Dp(1.3f), Dp(17), Dp(8), Dp(15), false, true},
        {fonts.label, Dp(1.3f), Dp(17), Dp(8), Dp(15), false, false},
        {fonts.label, Dp(1.2f), 0.0f, 0.0f, Dp(16), false, false, true},
        {fonts.label, Dp(1.3f), Dp(20), Dp(8), Dp(15), true, true},
        {fonts.label, Dp(1.3f), Dp(20), Dp(8), Dp(15), true, false},
    };
    const float icon_cell = Dp(54), icon_cell_min = Dp(46), equal_cap = Dp(300);
    TabLook look = looks[std::size(looks) - 1];
    std::vector<TabCell> cells(shown.size());
    float x0 = 0.0f, x1 = 0.0f;
    bool fits = false;
    for (const TabLook& candidate : looks)
    {
        x0 = bar_min.x + edge + ends(candidate.all_keys);
        x1 = bar_max.x - edge - ends(candidate.all_keys);
        const float room = x1 - x0;
        float widest = 0.0f, total = 0.0f;
        for (int n = 0; n < count; ++n)
        {
            const OverlayTabs::Tab& tab = *shown[n];
            TabCell& cell = cells[n];
            cell.label = launcher::TrackedWidth(candidate.font, labels[n].c_str(), candidate.tracking);
            const float badge = n == open ? 0.0f : BadgeWidth(fonts, tab); // the open tab's page shows it
            const float icon = tab.icon == Icon::None || candidate.no_icons ? 0.0f : candidate.icon;
            cell.content = icon + (icon > 0.0f && cell.label > 0.0f ? candidate.gap : 0.0f) + cell.label
                + (badge > 0.0f ? (tab.badge > 0 ? Dp(8) : Dp(3)) + badge : 0.0f);
            cell.unfolded = cell.content + 2 * candidate.pad;
            cell.folded = candidate.icons && tab.icon != Icon::None ? icon_cell : cell.unfolded;
            cell.width = n == open ? cell.unfolded : cell.folded;
            widest = std::max(widest, cell.width);
            total += cell.width;
        }
        if (!candidate.icons && widest * float(count) <= room)
        {
            // Equal parts, as wide as the widest needs (a few tabs share the bar).
            const float part = std::min(room / float(std::max(1, count)), std::max(widest, equal_cap));
            for (auto& cell : cells) cell.width = cell.folded = cell.unfolded = part;
        }
        else if (total > room) continue;
        else
        {
            // Their own widths, and what is left spread over them.
            const float spread = std::min((room - total) / float(std::max(1, count)), Dp(candidate.icons ? 36 : 60));
            for (auto& cell : cells)
            {
                cell.width += spread;
                cell.folded += spread;
                cell.unfolded += spread;
            }
        }
        look = candidate;
        fits = true;
        break;
    }
    // Narrower than even that: the icons closer (down to a minimum) beside
    // the open tab's label; without room for that label, icons alone (in
    // equal parts), and the page says its name in its title line.
    bool named_room = true;
    if (!fits)
    {
        const float room = x1 - x0;
        const float own = open >= 0 ? cells[open].unfolded : icon_cell;
        const float others =
            count > 1 ? std::clamp((room - own) / float(count - 1), icon_cell_min, icon_cell) : 0.0f;
        named_room = own + others * float(count - 1) <= room;
        for (int n = 0; n < count; ++n)
            if (shown[n]->icon == Icon::None) named_room = true; // a tab without an icon needs its label
        const float wide = named_room ? own : others;
        const float spread = std::max(0.0f, room - wide - others * float(count - 1)) / float(std::max(1, count));
        for (int n = 0; n < count; ++n)
        {
            cells[n].folded = (shown[n]->icon != Icon::None ? others : cells[n].unfolded) + spread;
            cells[n].unfolded = (named_room ? wide : others) + spread;
            cells[n].width = n == open ? cells[n].unfolded : cells[n].folded;
        }
    }
    named = named_room && open >= 0;

    // With icons for the others, the open tab's label grows out of its icon
    // and the one it leaves folds into its icon (a tenth of a second).
    std::vector<float> unfold(shown.size(), named_room ? 1.0f : 0.0f);
    if (named_room && (look.icons || !fits))
    {
        float total = 0.0f;
        for (int n = 0; n < count; ++n)
        {
            ImGui::PushID(shown[n]->id);
            float& value = *ImGui::GetStateStorage()->GetFloatRef(ImGui::GetID("##overlay_tab_unfold"), 0.0f);
            ImGui::PopID();
            const float target = n == open ? 1.0f : 0.0f;
            if (ImGui::IsWindowAppearing()) value = target;
            value += (target - value) * (1.0f - std::exp(-ImGui::GetIO().DeltaTime * 20.0f));
            if (std::fabs(target - value) < 0.002f) value = target;
            unfold[n] = cells[n].folded < cells[n].unfolded ? value : 1.0f;
            cells[n].width = cells[n].folded + (cells[n].unfolded - cells[n].folded) * unfold[n];
            total += cells[n].width;
        }
        // Folding and unfolding at once, the bar may hold a little more for a moment.
        if (total > x1 - x0)
            for (auto& cell : cells) cell.width *= (x1 - x0) / total;
    }
    {
        float total = 0.0f;
        for (const auto& cell : cells) total += cell.width;
        float x = (x0 + x1 - total) * 0.5f;
        for (auto& cell : cells)
        {
            cell.x = x;
            x += cell.width;
        }
    }

    // The keys at both ends, with a chevron towards the tabs: the previous
    // tab's at the left, the next's at the right.
    if (count >= 2)
    {
        float kx = bar_min.x + edge;
        for (std::size_t k = 0; k < style.previous_tab_keys.size() && (look.all_keys || k == 0); ++k)
        {
            const char* key = style.previous_tab_keys[k].c_str();
            DrawKeyCap(fonts, draw, {kx, middle - Dp(11)}, key, 0.0f);
            kx += KeyCapWidth(fonts, key) + Dp(5);
        }
        const float arm = Dp(4.5f);
        const ImU32 ink = Col(color::muted, 0.75f);
        {
            const float cx = kx - Dp(5) + Dp(6) + chevron * 0.5f;
            const ImVec2 left[] = {
                {cx + arm * 0.5f, middle - arm}, {cx - arm * 0.5f, middle}, {cx + arm * 0.5f, middle + arm}};
            draw->AddPolyline(left, 3, ink, 0, Dp(1.6f));
        }
        const float right_keys = caps(style.next_tab_keys, look.all_keys);
        kx = bar_max.x - edge - right_keys;
        {
            const float cx = kx - Dp(6) - chevron * 0.5f;
            const ImVec2 right[] = {
                {cx - arm * 0.5f, middle - arm}, {cx + arm * 0.5f, middle}, {cx - arm * 0.5f, middle + arm}};
            draw->AddPolyline(right, 3, ink, 0, Dp(1.6f));
        }
        for (std::size_t k = 0; k < style.next_tab_keys.size() && (look.all_keys || k == 0); ++k)
        {
            const char* key = style.next_tab_keys[k].c_str();
            DrawKeyCap(fonts, draw, {kx, middle - Dp(11)}, key, 0.0f);
            kx += KeyCapWidth(fonts, key) + Dp(5);
        }
    }

    const float inset_y = Dp(9), slant = Dp(9), pad = Dp(5);
    // Where the open tab's block is, in tabs: it slides to the tab opened
    // (eased out, about a tenth of a second); in place as the overlay appears.
    float* slide = ImGui::GetStateStorage()->GetFloatRef(ImGui::GetID("##overlay_tab_slide"), float(std::max(0, open)));
    if (ImGui::IsWindowAppearing() || open < 0) *slide = float(std::max(0, open));
    *slide = std::clamp(*slide, 0.0f, float(std::max(0, count - 1)));
    if (open >= 0)
    {
        *slide += (float(open) - *slide) * (1.0f - std::exp(-ImGui::GetIO().DeltaTime * 24.0f));
        if (std::fabs(*slide - float(open)) < 0.002f) *slide = float(open);
    }

    // The open tab's block: its glow, its gradient, a sheen on its upper
    // half, its contour; between two cells while it slides.
    if (open >= 0 && count > 0)
    {
        const int a = std::clamp(int(std::floor(*slide)), 0, count - 1), b = std::min(a + 1, count - 1);
        const float t = *slide - float(a);
        const float bx = cells[a].x + (cells[b].x - cells[a].x) * t;
        const float bw = cells[a].width + (cells[b].width - cells[a].width) * t;
        ImVec2 block[4];
        Slanted(block, {bx + pad, top + inset_y}, {bx + bw - pad, top + height - inset_y}, slant);
        for (int i = 3; i >= 1; --i)
        {
            const float grow = Dp(2.5f * float(i));
            ImVec2 glow[4];
            Slanted(glow, {bx + pad - grow, top + inset_y - grow * 0.6f},
                    {bx + bw - pad + grow, top + height - inset_y + grow * 0.6f}, slant);
            draw->AddConvexPolyFilled(glow, 4, Col(color::accent, 0.07f * float(4 - i)));
        }
        int start = draw->VtxBuffer.Size;
        draw->AddConvexPolyFilled(block, 4, IM_COL32_WHITE);
        ImGui::ShadeVertsLinearColorGradientKeepAlpha(draw, start, draw->VtxBuffer.Size, block[3], block[1],
                                                      Col(color::accent_hi), Col(color::accent));
        const float half = top + height * 0.5f;
        const ImVec2 sheen[4] = {block[0], block[1], {bx + bw - pad, half}, {bx + pad, half}};
        start = draw->VtxBuffer.Size;
        draw->AddConvexPolyFilled(sheen, 4, IM_COL32_WHITE);
        ShadeVertical(draw, start, block[0].y, half, ImVec4{1, 1, 1, 0.30f}, ImVec4{1, 1, 1, 0.0f});
        draw->AddPolyline(block, 4, Col(Mix(color::accent_hi, ImVec4{1, 1, 1, 1}, 0.55f), 0.85f),
                          ImDrawFlags_Closed, Dp(1));
    }

    for (int n = 0; n < count; ++n)
    {
        const OverlayTabs::Tab& tab = *shown[n];
        const TabCell& cell = cells[n];
        const bool selected = n == open;
        const ImVec2 at{cell.x, top + inset_y}, max{cell.x + cell.width, top + height - inset_y};
        ImGui::PushID(tab.id);
        ImGui::SetCursorScreenPos(at);
        if (ImGui::InvisibleButton("##overlay_tab", {std::max(1.0f, max.x - at.x), max.y - at.y}) && !selected
            && tab.available)
            tabs.clicked = tab.id;
        const bool hovered = ImGui::IsItemHovered();
        const float hover = launcher::Animate(ImGui::GetID("##hover"), hovered && !selected && tab.available, 12.0f);
        ImGui::PopID();
        // Lit as much as the block covers it.
        const float lit = std::clamp(1.0f - std::fabs(*slide - float(n)), 0.0f, 1.0f);
        if (hover > 0.01f)
        {
            ImVec2 quad[4];
            Slanted(quad, {at.x + pad, at.y}, {max.x - pad, max.y}, slant);
            draw->AddConvexPolyFilled(quad, 4, Col(color::text, 0.05f * hover * (1.0f - lit)));
            draw->AddPolyline(quad, 4, Col(color::text, 0.14f * hover * (1.0f - lit)), ImDrawFlags_Closed, Dp(1));
        }
        // Not available: in the dim of things that can't be used, fainter.
        const float alpha = tab.available ? 1.0f : 0.6f;
        const ImVec4 base = tab.available ? Mix(color::muted, color::text, hover) : color::dim;
        const ImU32 ink = Col(Mix(base, color::accent_ink, lit), alpha);
        // Its icon, its label (unfolding with icons for the others), its badge.
        const float shows = unfold[n];
        const float icon = tab.icon == Icon::None || look.no_icons ? 0.0f : look.icon;
        const float rest = cell.content - icon;
        const float content = icon + rest * shows;
        // In the middle of its cell; from its left where it doesn't fit (its label cut short).
        const float inner = Dp(12);
        float x = std::max(cell.x + inner, cell.x + (cell.width - content) * 0.5f);
        if (icon > 0.0f)
        {
            DrawIcon(draw, tab.icon, {x + icon * 0.5f, middle}, icon, ink);
            x += icon + (cell.label > 0.0f ? look.gap : 0.0f) * shows;
        }
        const float badge = selected ? 0.0f : BadgeWidth(fonts, tab);
        if (shows > 0.01f && cell.label > 0.0f)
        {
            // Cut to what has unfolded (renderers clip in whole pixels: a
            // little more, and nothing once it all shows).
            const bool cut = shows < 0.999f || x + cell.label > max.x - inner;
            if (cut)
                draw->PushClipRect({std::max(at.x + Dp(2), x - Dp(1)), at.y},
                                   {std::min(max.x - inner, x + cell.label * shows + Dp(1)), max.y}, true);
            const float text_y = middle - FontSize(look.font) * 0.5f;
            launcher::DrawTracked(draw, look.font, {x, text_y},
                                  Col(Mix(base, color::accent_ink, lit),
                                      alpha * std::clamp(shows * 1.4f - 0.4f, 0.0f, 1.0f)),
                                  labels[n].c_str(), look.tracking);
            if (cut) draw->PopClipRect();
            x += cell.label * shows;
        }
        if (badge > 0.0f)
        {
            if (shows > 0.5f && cell.label > 0.0f)
                // After the label: a count beside it, a dot up at its top.
                DrawBadge(draw, fonts,
                          tab.badge > 0 ? ImVec2{x + Dp(8) + badge * 0.5f, middle}
                                        : ImVec2{x + Dp(3) + badge * 0.5f, middle - FontSize(look.font) * 0.30f},
                          tab, alpha);
            else
                // Folded into its icon: on the icon's upper right corner, mostly beside it.
                DrawBadge(draw, fonts,
                          {cell.x + (cell.width + icon) * 0.5f + badge * 0.5f - Dp(4), middle - icon * 0.5f + Dp(1)},
                          tab, alpha);
        }
        // Under the mouse: the label where only the icon shows, and why a tab doesn't open.
        if (hovered && ((shows < 0.5f && !labels[n].empty()) || (!tab.available && !tab.hint.empty())))
        {
            std::string tip = tab.label;
            if (!tab.available && !tab.hint.empty()) tip += (tip.empty() ? "" : ": ") + tab.hint;
            ImGui::SetTooltip("%s", tip.c_str());
        }
    }
    // The accent line under the bar, with a faint glow below it.
    draw->AddRectFilled({bar_min.x + Dp(1), bar_max.y - line}, {bar_max.x - Dp(1), bar_max.y},
                        Col(color::accent, 0.85f));
    ImVec4 glow = color::accent, none = color::accent;
    glow.w = 0.10f;
    none.w = 0.0f;
    OverlayFade(draw, {bar_min.x + Dp(1), bar_max.y}, {bar_max.x - Dp(1), bar_max.y + Dp(12)}, glow, none, 0.0f);
    return height;
}
} // namespace

// ------------------------------------------------------------------ tabs

bool OverlayTabOpens(const std::vector<OverlayTabs::Tab>& tabs, int id)
{
    for (const auto& tab : tabs)
        if (tab.id == id) return !tab.hidden && tab.available;
    return false;
}

int OverlayTabStep(const std::vector<OverlayTabs::Tab>& tabs, int current, int step)
{
    const int count = int(tabs.size());
    if (count == 0 || step == 0) return current;
    int at = -1;
    for (int n = 0; n < count; ++n)
        if (tabs[n].id == current) at = n;
    // From outside the strip, the first step lands on the first (or last) tab.
    if (at < 0) at = step > 0 ? count - 1 : 0;
    const int direction = step > 0 ? 1 : -1;
    int left = std::abs(step);
    for (int n = 1; n <= count; ++n)
    {
        const OverlayTabs::Tab& tab = tabs[((at + direction * n) % count + count) % count];
        if (tab.hidden || !tab.available) continue;
        if (tab.id == current) break;
        if (--left == 0) return tab.id;
    }
    return current;
}

// ------------------------------------------------------------------ screens

void OverlayScreens::Open(Screen first)
{
    stack.clear();
    stack.push_back({std::move(first), 0});
    focus_pending = true;
    restore = 0;
    auto_focus = 0;
    ++changes;
}

void OverlayScreens::Push(Screen screen, ImGuiID back_focus)
{
    if (!stack.empty()) stack.back().back_focus = back_focus;
    stack.push_back({std::move(screen), 0});
    focus_pending = true;
    restore = 0;
    auto_focus = 0;
    ++changes;
}

bool OverlayScreens::Pop()
{
    if (stack.size() < 2) return false;
    stack.pop_back();
    restore = stack.back().back_focus;
    focus_pending = true;
    auto_focus = 0;
    ++changes;
    return true;
}

// ------------------------------------------------------------------ widgets

bool OverlayNavFocused() { return ImGui::IsItemFocused() && GImGui->NavCursorVisible; }

void OverlayFocusRing(ImDrawList* draw, ImVec2 min, ImVec2 max, float rounding)
{
    const float grow = Dp(2.5f);
    draw->AddRect({min.x - grow, min.y - grow}, {max.x + grow, max.y + grow}, Col(color::accent, 0.95f),
                  rounding + grow, 0, Dp(2));
}

void OverlayFade(ImDrawList* draw, ImVec2 min, ImVec2 max, const ImVec4& first, const ImVec4& second, float rounding,
                 ImDrawFlags flags)
{
    const int start = draw->VtxBuffer.Size;
    draw->AddRectFilled(min, max, IM_COL32_WHITE, rounding, flags);
    ShadeVertical(draw, start, min.y, max.y, first, second);
}

ImVec4 OverlaySurface() { return Mix(color::window, color::card, 0.45f); }

void OverlayCard(ImDrawList* draw, ImVec2 min, ImVec2 max, float rounding, const ImVec4& fill, const ImVec4& edge)
{
    ImVec4 upper = Mix(fill, ImVec4{1, 1, 1, 1}, 0.05f);
    upper.w = fill.w;
    OverlayFade(draw, min, max, upper, fill, rounding);
    const int start = draw->VtxBuffer.Size;
    draw->AddRect(min, max, IM_COL32_WHITE, rounding, 0, Dp(1));
    ImVec4 lit = Mix(edge, ImVec4{1, 1, 1, 1}, 0.28f), shade = edge;
    lit.w = edge.w;
    shade.w = edge.w * 0.65f;
    ShadeVertical(draw, start, min.y, max.y, lit, shade);
}

void OverlayFocusable(bool preferred)
{
    // The preferred item showed late (the lists came after the overlay
    // opened): it takes the cursor if the player hasn't moved it since.
    if (preferred && g.active && g.screens && g.screens->auto_focus && !g.claimed
        && g.screens->changes == g.changes && GImGui->NavId == g.screens->auto_focus)
    {
        Claim(ImGui::GetItemID(), ImGui::GetCurrentWindow());
        return;
    }
    if (!CanClaim() || g.claimed) return;
    const ImGuiID id = ImGui::GetItemID();
    if (!id) return;
    if (!g.first_id)
    {
        g.first_id = id;
        g.first_window = ImGui::GetCurrentWindow();
    }
    if (preferred && !g.preferred_id)
    {
        g.preferred_id = id;
        g.preferred_window = ImGui::GetCurrentWindow();
    }
    // Coming back: only the item to come back to, until EndOverlay knows it is gone.
    if (g.screens->restore)
    {
        if (id == g.screens->restore) Claim(id, ImGui::GetCurrentWindow());
        return;
    }
    if (preferred) Claim(id, ImGui::GetCurrentWindow());
}

void OverlayLayoutIs(int layout)
{
    if (!g.active || !g.screens || g.screens->Empty()) return;
    ImGuiStorage* storage = ImGui::GetStateStorage();
    const ImGuiID key = ImGui::GetID(("##layout_" + std::to_string(g.screens->Top().id)).c_str());
    const int before = storage->GetInt(key, 0);
    storage->SetInt(key, layout);
    if (before != 0 && before != layout && g.screens->changes == g.changes && OverlayHasFocus())
    {
        g.screens->focus_pending = true;
        g.screens->restore = 0;
    }
}

bool OverlayButton(const char* id, const char* label, Icon icon, ImVec2 size, OverlayLook look, bool enabled,
                   bool preferred)
{
    const launcher::Fonts& fonts = launcher::CurrentFonts();
    ImGui::PushID(id);
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    ImGui::BeginDisabled(!enabled);
    const bool pressed = ImGui::InvisibleButton("##button", size, ImGuiButtonFlags_EnableNav);
    const bool hovered = ImGui::IsItemHovered(), held = ImGui::IsItemActive(), focused = OverlayNavFocused();
    ImGui::EndDisabled();
    if (enabled) OverlayFocusable(preferred);
    const float hover = launcher::Animate(ImGui::GetID("##hover"), (hovered || focused) && enabled, 12.0f);
    auto* draw = ImGui::GetWindowDrawList();
    const ImVec2 max = Add(pos, size);
    const float rounding = std::min(size.y * 0.5f, Dp(10));
    ImU32 ink = 0;
    switch (look)
    {
    case OverlayLook::Primary:
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
    case OverlayLook::Secondary:
        draw->AddRectFilled(pos, max, Col(held ? Mix(color::field, color::accent, 0.18f)
                                               : Mix(color::field, color::border, 0.9f * hover)),
                            rounding);
        draw->AddRect(pos, max, Col(Mix(color::border, color::muted, 0.4f * hover)), rounding, 0, Dp(1));
        ink = Col(enabled ? Mix(color::muted, color::text, 0.6f + 0.4f * hover) : color::dim);
        break;
    case OverlayLook::Ghost:
        if (hover > 0.0f || held)
            draw->AddRectFilled(pos, max, Col(color::text, held ? 0.10f : 0.07f * hover), rounding);
        ink = Col(enabled ? Mix(color::muted, color::text, 0.35f + 0.65f * hover) : color::dim);
        break;
    }
    ImFont* font = look == OverlayLook::Primary ? fonts.label : fonts.body;
    const ImVec2 text = TextSize(font, label);
    const float icon_size = icon == Icon::None ? 0.0f : FontSize(font) * 0.9f;
    const float gap = icon == Icon::None || !*label ? 0.0f : Dp(8);
    const float left = pos.x + (size.x - (text.x + icon_size + gap)) * 0.5f;
    if (icon != Icon::None) DrawIcon(draw, icon, {left + icon_size * 0.5f, pos.y + size.y * 0.5f}, icon_size, ink);
    DrawLabel(draw, font, {left + icon_size + gap, pos.y + (size.y - text.y) * 0.5f}, ink, label);
    if (focused) OverlayFocusRing(draw, pos, max, rounding);
    ImGui::PopID();
    return pressed && enabled;
}

// ------------------------------------------------------------------ frame

namespace
{
// The layer being drawn: what it changed, to put back.
struct Layer
{
    bool active = false;
    float saved_unit = 1.0f;
    launcher::Fonts saved_fonts;
};
Layer layer;
} // namespace

OverlayArea BeginOverlayLayer(const char* id, const OverlayStyle& style, bool front)
{
    IM_ASSERT(!layer.active && "BeginOverlayLayer without EndOverlayLayer");
    layer.active = true;
    // The launcher's widgets read their scale and fonts from ui_kit: the
    // overlay's until EndOverlayLayer, the caller's back after it.
    layer.saved_unit = launcher::UiScale();
    layer.saved_fonts = launcher::CurrentFonts();
    const launcher::Fonts fonts = Complete(style.fonts);
    launcher::SetUiScale(style.unit);
    launcher::SetCurrentFonts(fonts);

    OverlayArea area{style.area_min, style.area_max};
    if (area.max.x <= area.min.x || area.max.y <= area.min.y) area = {{0, 0}, ImGui::GetIO().DisplaySize};
    ImGui::SetNextWindowPos(area.min);
    ImGui::SetNextWindowSize({area.max.x - area.min.x, area.max.y - area.min.y});
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0, 0});
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {0, 0});
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarSize, Dp(6));
    ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarRounding, Dp(3));
    ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, Dp(8));
    ImGui::PushStyleVar(ImGuiStyleVar_PopupBorderSize, 1.0f);
    const std::pair<ImGuiCol, ImVec4> colors[kStyleColors] = {
        {ImGuiCol_WindowBg, {0, 0, 0, 0}},
        {ImGuiCol_ChildBg, {0, 0, 0, 0}},
        {ImGuiCol_NavCursor, {0, 0, 0, 0}}, // the overlay draws its own ring
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
    ImGui::Begin(id, nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings
                     | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoScrollbar
                     | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoFocusOnAppearing);
    if (front) ImGui::BringWindowToDisplayFront(ImGui::GetCurrentWindow());
    SmoothLines();
    return area;
}

void EndOverlayLayer()
{
    IM_ASSERT(layer.active && "EndOverlayLayer without BeginOverlayLayer");
    ImGui::End();
    ImGui::PopFont();
    ImGui::PopStyleColor(kStyleColors);
    ImGui::PopStyleVar(kStyleVars);
    launcher::SetUiScale(layer.saved_unit);
    launcher::SetCurrentFonts(layer.saved_fonts);
    layer.active = false;
}

void OverlayGlass(ImDrawList* draw, ImVec2 min, ImVec2 max, float rounding, float appear, const ImVec4& glow_tint)
{
    // The shadow, soft and below; the frosted glass, lighter at the top,
    // with a glow there; the contour, lit from above.
    for (int i = 0; i < 12; ++i)
    {
        const float t = float(i) / 12.0f, grow = Dp(1.5f + 3.5f * float(i));
        draw->AddRectFilled({min.x - grow, min.y - grow * 0.4f + Dp(6)}, {max.x + grow, max.y + grow + Dp(12)},
                            Col(ImVec4{0, 0, 0, 1}, 0.07f * (1.0f - t) * (1.0f - t) * appear), rounding + grow);
    }
    const ImVec4 surface = OverlaySurface();
    ImVec4 top = Mix(surface, color::card_hover, 0.45f), bottom = surface;
    top.w = 0.80f;
    bottom.w = 0.90f;
    OverlayFade(draw, min, max, top, bottom, rounding);
    ImVec4 glow = Mix(surface, glow_tint, 0.22f), none = glow;
    glow.w = 0.45f;
    none.w = 0.0f;
    OverlayFade(draw, {min.x + Dp(1), min.y + Dp(1)}, {max.x - Dp(1), std::min(max.y, min.y + Dp(200))}, glow, none,
                rounding - Dp(1), ImDrawFlags_RoundCornersTop);
    const int start = draw->VtxBuffer.Size;
    draw->AddRect(min, max, IM_COL32_WHITE, rounding, 0, Dp(1));
    ImVec4 lit = Mix(color::border, ImVec4{1, 1, 1, 1}, 0.45f), shade = color::border;
    lit.w = 0.95f;
    shade.w = 0.55f;
    ShadeVertical(draw, start, min.y, max.y, lit, shade);
}

float OverlayKeyCapWidth(const char* key) { return KeyCapWidth(launcher::CurrentFonts(), key); }

float OverlaySplitLeft(float width) { return std::clamp(width * 0.38f, Dp(300), Dp(380)); }

float OverlaySplitGap() { return Dp(32); }

void OverlaySplitDivider(const OverlayLayout& layout, float left)
{
    const float divider = layout.min.x + left + OverlaySplitGap() * 0.5f;
    ImGui::GetWindowDrawList()->AddRectFilled({divider, layout.min.y + Dp(20)},
                                              {divider + Dp(1), layout.max.y - Dp(20)}, Col(color::border, 0.7f));
}

void OverlayKeyCap(ImDrawList* draw, ImVec2 pos, const char* key, float lit)
{
    DrawKeyCap(launcher::CurrentFonts(), draw, pos, key, lit);
}

OverlayLayout BeginOverlay(const OverlayStyle& style, OverlayScreens& screens, const std::string& title,
                           OverlayTabs* tabs)
{
    IM_ASSERT(!g.active && "BeginOverlay without EndOverlay");
    g = Current{};
    g.active = true;
    g.screens = &screens;
    g.changes = screens.changes;
    g.close_key = style.close_key.empty() ? "Esc" : style.close_key;
    // Not focused by ImGui as it appears: that would put its cursor on the
    // first item (the back arrow); OverlayFocusable puts it where it belongs.
    const OverlayArea area = BeginOverlayLayer(kWindowName, style, screens.focus_pending);
    const launcher::Fonts& fonts = launcher::CurrentFonts();
    g.area_min = area.min;
    g.area_max = area.max;
    const float area_w = g.area_max.x - g.area_min.x, area_h = g.area_max.y - g.area_min.y;
    const float appear = std::clamp(style.appear, 0.0f, 1.0f);
    const float eased = 1.0f - (1.0f - appear) * (1.0f - appear) * (1.0f - appear);

    // The window: about three quarters of the picture, in its middle; more of
    // a small one (in dp: a small game window, at the overlay's smallest
    // scale), up to all of it but the margin; at most 1.5 times as wide as
    // the picture is high (a wide screen).
    const float margin = Dp(16);
    const auto share = [](float room, float small, float large, float usual) {
        const float roomy = std::clamp((room - small) / (large - small), 0.0f, 1.0f);
        return usual + (1.0f - usual) * (1.0f - roomy);
    };
    const float width = std::min(std::clamp(area_w * share(area_w, Dp(760), Dp(1280), 0.72f), Dp(640),
                                            std::max(Dp(640), area_h * 1.53f)),
                                 area_w - 2 * margin);
    const float height =
        std::min(std::max(area_h * share(area_h, Dp(460), Dp(720), 0.86f), Dp(420)), area_h - 2 * margin);
    const float rise = (1.0f - eased) * Dp(28);
    g.window_min = {g.area_min.x + (area_w - width) * 0.5f, g.area_min.y + (area_h - height) * 0.5f + rise};
    g.window_max = {g.window_min.x + width, g.window_min.y + height};

    auto* draw = ImGui::GetWindowDrawList();
    if (style.dim > 0.0f) draw->AddRectFilled(g.area_min, g.area_max, Col(ImVec4{0, 0, 0, 1}, style.dim * eased));
    OverlayGlass(draw, g.window_min, g.window_max, Dp(22), eased, color::accent);

    // The tabs, then the title line: the back arrow (from the second screen
    // on) and the title. With tabs, a tab's first screen has no title line
    // (its tab names it), unless the tabs are too narrow to show its name.
    const float pad = Dp(28);
    float top = g.window_min.y;
    bool named = false;
    if (tabs && !tabs->tabs.empty()) top += DrawTabs(style, fonts, *tabs, top, named);
    if (!tabs || tabs->tabs.empty() || screens.CanGoBack() || !named)
    {
        const float header = tabs && !tabs->tabs.empty() ? Dp(58) : Dp(68);
        float x = g.window_min.x + pad;
        const float middle = top + header * 0.5f + Dp(1);
        if (screens.CanGoBack())
        {
            const float size = Dp(38);
            ImGui::SetCursorScreenPos({x - Dp(8), middle - size * 0.5f});
            const bool pressed = ImGui::InvisibleButton("##overlay_back", {size, size}, ImGuiButtonFlags_EnableNav);
            const bool hovered = ImGui::IsItemHovered(), focused = OverlayNavFocused();
            const float hover = launcher::Animate(ImGui::GetID("##overlay_back_hover"), hovered || focused, 12.0f);
            const ImVec2 center{x - Dp(8) + size * 0.5f, middle};
            draw->AddCircleFilled(center, size * 0.5f, Col(color::text, 0.05f + 0.08f * hover));
            DrawIcon(draw, Icon::Back, center, Dp(17), Col(Mix(color::muted, color::text, 0.4f + 0.6f * hover)));
            if (focused) draw->AddCircle(center, size * 0.5f + Dp(2.5f), Col(color::accent, 0.95f), 0, Dp(2));
            if (hovered) ImGui::SetTooltip("Back");
            if (pressed) g.back_pressed = true;
            x += size + Dp(6);
        }
        draw->PushClipRect({x, top}, {g.window_max.x - pad, top + header}, true);
        DrawLabel(draw, fonts.button, {x, middle - FontSize(fonts.button) * 0.5f}, Col(color::text), title.c_str());
        draw->PopClipRect();
        top += header;
        draw->AddRectFilled({g.window_min.x + pad, top - Dp(1)}, {g.window_max.x - pad, top}, Col(color::border, 0.8f));
    }

    const float footer = Dp(64);
    g.footer_top = g.window_max.y - footer;
    return {{g.window_min.x + pad, top}, {g.window_max.x - pad, g.footer_top}};
}

bool OverlayHasFocus()
{
    const ImGuiWindow* nav = GImGui->NavWindow;
    return nav && nav->RootWindow && std::strcmp(nav->RootWindow->Name, kWindowName) == 0;
}

void BeginOverlayScroll(const char* id, ImVec2 min, ImVec2 max)
{
    // A little room left of the content, so the cursor's ring around an item
    // at its edge shows whole.
    const float width = max.x - min.x, gutter = Dp(14), inset = Dp(4);
    ImGui::SetCursorScreenPos({min.x - inset, min.y});
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {inset, 0.0f});
    ImGui::BeginChild(id, {width + inset + gutter, std::max(Dp(40), max.y - min.y)},
                      ImGuiChildFlags_NavFlattened | ImGuiChildFlags_AlwaysUseWindowPadding,
                      ImGuiWindowFlags_NoBackground);
    ImGui::PopStyleVar();
    SmoothLines();
    g.scroll_widths.push_back(width);
}

void EndOverlayScroll()
{
    const float width = g.scroll_widths.empty() ? ImGui::GetWindowWidth() : g.scroll_widths.back();
    if (!g.scroll_widths.empty()) g.scroll_widths.pop_back();
    // The content fades out at an edge it scrolls on beyond.
    auto* draw = ImGui::GetWindowDrawList();
    const ImVec2 at{ImGui::GetWindowPos().x + ImGui::GetStyle().WindowPadding.x, ImGui::GetWindowPos().y};
    const float bottom = at.y + ImGui::GetWindowHeight();
    ImVec4 clear = OverlaySurface(), solid = clear;
    clear.w = 0.0f;
    solid.w = 0.88f;
    if (ImGui::GetScrollY() < ImGui::GetScrollMaxY() - 1.0f)
        OverlayFade(draw, {at.x, bottom - Dp(32)}, {at.x + width, bottom}, clear, solid, 0.0f);
    if (ImGui::GetScrollY() > 1.0f) OverlayFade(draw, at, {at.x + width, at.y + Dp(20)}, solid, clear, 0.0f);
    ImGui::EndChild();
}

OverlayResult EndOverlay(const OverlayFooter& footer)
{
    IM_ASSERT(g.active && "EndOverlay without BeginOverlay");
    OverlayResult result;
    const launcher::Fonts& fonts = launcher::CurrentFonts();
    auto* draw = ImGui::GetWindowDrawList();
    const float pad = Dp(28);
    draw->AddRectFilled({g.window_min.x + pad, g.footer_top}, {g.window_max.x - pad, g.footer_top + Dp(1)},
                        Col(color::border, 0.8f));
    const float middle = (g.footer_top + g.window_max.y) * 0.5f;

    // Close and its key at the right; Done before it.
    float right = g.window_max.x - pad + Dp(10);
    ImGuiID close_id = 0;
    {
        const char* label = "Close";
        const ImVec2 text = TextSize(fonts.body, label);
        const float key_w = KeyCapWidth(fonts, g.close_key.c_str());
        const ImVec2 size{Dp(12) + key_w + Dp(8) + text.x + Dp(14), Dp(38)};
        const ImVec2 at{right - size.x, middle - size.y * 0.5f};
        ImGui::SetCursorScreenPos(at);
        const bool pressed = ImGui::InvisibleButton("##overlay_close", size, ImGuiButtonFlags_EnableNav);
        close_id = ImGui::GetItemID();
        const bool hovered = ImGui::IsItemHovered(), focused = OverlayNavFocused();
        const float hover = launcher::Animate(ImGui::GetID("##overlay_close_hover"), hovered || focused, 12.0f);
        if (hover > 0.0f) draw->AddRectFilled(at, Add(at, size), Col(color::text, 0.07f * hover), Dp(10));
        DrawKeyCap(fonts, draw, {at.x + Dp(12), middle - Dp(11)}, g.close_key.c_str(), hover);
        DrawLabel(draw, fonts.body, {at.x + Dp(12) + key_w + Dp(8), middle - text.y * 0.5f},
                  Col(Mix(color::muted, color::text, 0.4f + 0.6f * hover)), label);
        if (focused) OverlayFocusRing(draw, at, Add(at, size), Dp(10));
        if (pressed) result.close = true;
        right = at.x - Dp(10);
    }
    if (!footer.done.empty())
    {
        const ImVec2 size{std::max(Dp(120), TextSize(fonts.label, footer.done.c_str()).x + Dp(64)), Dp(40)};
        ImGui::SetCursorScreenPos({right - size.x, middle - size.y * 0.5f});
        if (OverlayButton("overlay_done", footer.done.c_str(), Icon::Check, size, OverlayLook::Primary,
                          footer.done_enabled))
            result.done = true;
        right -= size.x + Dp(10);
    }
    if (!footer.note.empty())
    {
        const ImVec4 tint = footer.note_color.w > 0.0f ? footer.note_color : color::dim;
        draw->PushClipRect({g.window_min.x + pad, g.footer_top}, {right - Dp(8), g.window_max.y}, true);
        DrawLabel(draw, fonts.caption, {g.window_min.x + pad, middle - FontSize(fonts.caption) * 0.5f}, Col(tint),
                  footer.note.c_str());
        draw->PopClipRect();
    }

    // The keys: Escape closes; B and Backspace go back, or close on the first screen.
    OverlayScreens& screens = *g.screens;
    if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows))
    {
        if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) result.close = true;
        if (ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false) || ImGui::IsKeyPressed(ImGuiKey_Backspace, false))
        {
            if (screens.CanGoBack()) g.back_pressed = true;
            else result.close = true;
        }
    }
    // A click beside the window closes it.
    if (ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows) && ImGui::IsMouseClicked(ImGuiMouseButton_Left)
        && !ImGui::IsMouseHoveringRect(g.window_min, g.window_max, false))
        result.close = true;
    if (g.back_pressed && !result.close && screens.Pop()) result.back = true;

    // The cursor: the screen's preferred item when nothing took it (or the
    // item to come back to is gone), else its first, else Done, else Close.
    if (!g.claimed && CanClaim())
    {
        if (g.preferred_id) Claim(g.preferred_id, g.preferred_window);
        else if (g.first_id) Claim(g.first_id, g.first_window, true);
        else if (close_id) Claim(close_id, ImGui::GetCurrentWindow(), true);
    }
    // The player moved the cursor: it stays where they put it.
    if (screens.auto_focus && GImGui->NavId != screens.auto_focus) screens.auto_focus = 0;

    EndOverlayLayer();
    g.active = false;
    g.screens = nullptr;
    return result;
}

launcher::Fonts AddOnlinePanelFonts(ImFontAtlas& atlas, void* ttf, int ttf_size, float unit)
{
    // Latin-1 and the punctuation the overlay writes (dashes, dots, quotes).
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
    fonts.button = add(22.0f);
    fonts.title = fonts.button;
    fonts.overline = fonts.label;
    return fonts;
}
} // namespace mscharged::overlay
