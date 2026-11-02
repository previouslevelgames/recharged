#include "overlay/game_options.h"

#include "imgui_internal.h"

#include <algorithm>
#include <cctype>
#include <cfloat>
#include <vector>

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

// The options the game offers, grouped and ordered as on the launcher's
// Gameplay page (whose other options stay there).
struct Group
{
    const char* title;
    Icon icon;
    std::vector<GameTweak> tweaks;
};

const std::vector<Group>& Groups()
{
    static const std::vector<Group> groups{
        {"Competitive gameplay", Icon::Trophy,
         {GameTweak::FastTerrain, GameTweak::SecureStadiums, GameTweak::ThunderIslandWalls,
          GameTweak::FairBallContests, GameTweak::NoMegaStrikes, GameTweak::MatchPoint}},
        {"Matches", Icon::Target, {GameTweak::CpuVsCpu, GameTweak::MirrorMatch, GameTweak::BluePeach}},
        {"Menus", Icon::Play, {GameTweak::FastMenus}},
    };
    return groups;
}

const Group* GroupOf(GameTweak tweak)
{
    for (const auto& group : Groups())
        if (std::find(group.tweaks.begin(), group.tweaks.end(), tweak) != group.tweaks.end()) return &group;
    return nullptr;
}

// When a change counts, for the description.
std::string Timing(GameTweak tweak, bool in_match)
{
    if (GameOptionAtOnce(tweak)) return "Takes effect at once.";
    return in_match ? "Takes effect from the next match: this one keeps what it started with."
                    : "Takes effect from the next match.";
}

float WrappedHeight(ImFont* font, const char* text, float width)
{
    return font->CalcTextSizeA(FontSize(font), FLT_MAX, width, text).y;
}

// An on/off switch, `on` animated 0..1.
void DrawSwitch(ImDrawList* draw, ImVec2 at, float on, float hover)
{
    const float width = Dp(40), height = Dp(23);
    draw->AddRectFilled(at, {at.x + width, at.y + height},
                        Col(Mix(Mix(color::border, color::muted, 0.25f * hover), color::accent, on)), height * 0.5f);
    draw->AddCircleFilled({at.x + height * 0.5f + (width - height) * on, at.y + height * 0.5f}, height * 0.5f - Dp(3),
                          Col(Mix(color::text, color::accent_ink, on)));
}

// One option: its name (and with `describe` its description under it) and
// its switch; the row is the switch. The option the description shows
// follows the cursor and the mouse.
void DrawOption(const GameOptionsModel& model, GameOptionsState& state, GameTweak tweak, float width, bool describe,
                bool preferred, std::optional<GameOptionChange>& change)
{
    const launcher::Fonts& fonts = launcher::CurrentFonts();
    const std::size_t index = GameTweakIndex(tweak);
    const GameTweakInfo& info = kGameTweaks[index];
    const bool on = model.tweaks[index];
    auto* draw = ImGui::GetWindowDrawList();
    const ImVec2 at = ImGui::GetCursorScreenPos();
    const float pad = Dp(14), switch_w = Dp(40);
    const float text_w = width - 2 * pad - switch_w - Dp(16);
    const float description = describe ? Dp(3) + WrappedHeight(fonts.caption, info.description, text_w) : 0.0f;
    const float height = describe ? pad + FontSize(fonts.label) + description + pad : Dp(50);
    const ImVec2 end{at.x + width, at.y + height};
    ImGui::PushID(info.key);
    const bool pressed = ImGui::InvisibleButton("##option", {width, height}, ImGuiButtonFlags_EnableNav);
    const bool hovered = ImGui::IsItemHovered(), focused = OverlayNavFocused();
    OverlayFocusable(preferred);
    if (hovered || focused) state.shown = tweak;
    const bool shown = !describe && state.shown && *state.shown == tweak;
    const float glow = launcher::Animate(ImGui::GetID("##glow"), hovered || focused, 12.0f);
    const float lit = launcher::Animate(ImGui::GetID("##on"), on, 14.0f);
    ImGui::PopID();
    if (glow > 0.01f || shown)
        draw->AddRectFilled(at, end, Col(color::card_hover, std::max(glow, shown ? 0.55f : 0.0f)), Dp(12));
    const float name_y = describe ? at.y + pad : at.y + (height - FontSize(fonts.label)) * 0.5f;
    draw->PushClipRect(at, {end.x - pad - switch_w - Dp(8), end.y}, true);
    DrawLabel(draw, fonts.label, {at.x + pad, name_y}, Col(on ? color::text : Mix(color::text, color::muted, 0.3f)),
              info.name);
    draw->PopClipRect();
    if (describe)
        draw->AddText(fonts.caption, FontSize(fonts.caption), {at.x + pad, name_y + FontSize(fonts.label) + Dp(3)},
                      Col(color::muted), info.description, nullptr, text_w);
    DrawSwitch(draw, {end.x - pad - switch_w, at.y + (height - Dp(23)) * 0.5f}, lit, glow);
    if (focused) OverlayFocusRing(draw, at, end, Dp(12));
    if (pressed) change = GameOptionChange{tweak, !on};
    ImGui::SetCursorScreenPos(at);
    ImGui::Dummy({width, height});
}

// A group's heading: its icon and its name in tracked capitals.
void DrawGroupHeading(const Group& group, float width)
{
    const launcher::Fonts& fonts = launcher::CurrentFonts();
    auto* draw = ImGui::GetWindowDrawList();
    const ImVec2 at = ImGui::GetCursorScreenPos();
    const float height = Dp(34);
    const float y = at.y + height - FontSize(fonts.caption) - Dp(10);
    DrawIcon(draw, group.icon, {at.x + Dp(12), y + FontSize(fonts.caption) * 0.5f}, Dp(13), Col(color::accent, 0.85f));
    std::string title = group.title;
    for (auto& c : title) c = char(std::toupper(static_cast<unsigned char>(c)));
    launcher::DrawTracked(draw, fonts.caption, {at.x + Dp(26), y}, Col(color::dim), title.c_str(), Dp(1.5f));
    ImGui::Dummy({width, height});
}

// What the player should know first: changes wait for the next match, and
// online matches play by their own rules.
void DrawNotes(const GameOptionsModel& model, float width)
{
    const launcher::Fonts& fonts = launcher::CurrentFonts();
    auto* draw = ImGui::GetWindowDrawList();
    const ImVec2 at = ImGui::GetCursorScreenPos();
    const float pad = Dp(14), wrap = width - 2 * pad - Dp(24);
    const char* match = "Changes take effect from the next match.";
    const char* online = "Online matches play by their own rules.";
    const float line = FontSize(fonts.caption);
    const float height = pad + (model.in_match ? WrappedHeight(fonts.body, match, wrap) + Dp(4) : 0.0f) + line + pad;
    const ImVec4 tint = model.in_match ? color::info : color::border;
    draw->AddRectFilled(at, {at.x + width, at.y + height},
                        Col(Mix(color::field, tint, model.in_match ? 0.10f : 0.0f), 0.8f), Dp(12));
    draw->AddRect(at, {at.x + width, at.y + height}, Col(tint, model.in_match ? 0.45f : 0.9f), Dp(12), 0, Dp(1));
    float y = at.y + pad;
    DrawIcon(draw, Icon::Info, {at.x + pad + Dp(8), y + (model.in_match ? FontSize(fonts.body) : line) * 0.5f}, Dp(15),
             Col(model.in_match ? color::info : color::dim));
    if (model.in_match)
    {
        draw->AddText(fonts.body, FontSize(fonts.body), {at.x + pad + Dp(24), y}, Col(color::text), match, nullptr,
                      wrap);
        y += WrappedHeight(fonts.body, match, wrap) + Dp(4);
    }
    DrawLabel(draw, fonts.caption, {at.x + pad + Dp(24), y}, Col(color::muted), online);
    ImGui::Dummy({width, height});
}

// The option the list's cursor is on, described in full.
void DrawDescription(const GameOptionsModel& model, GameTweak tweak, float width)
{
    const launcher::Fonts& fonts = launcher::CurrentFonts();
    const std::size_t index = GameTweakIndex(tweak);
    const GameTweakInfo& info = kGameTweaks[index];
    const bool on = model.tweaks[index];
    const Group* group = GroupOf(tweak);
    auto* draw = ImGui::GetWindowDrawList();
    const ImVec2 at = ImGui::GetCursorScreenPos();
    const float pad = Dp(22), wrap = width - 2 * pad;
    const std::string timing = Timing(tweak, model.in_match);
    const float kicker = FontSize(fonts.caption) + Dp(10);
    const float description = WrappedHeight(fonts.body, info.description, wrap);
    const float timing_h = WrappedHeight(fonts.caption, timing.c_str(), wrap - Dp(22));
    const float height = pad + kicker + FontSize(fonts.button) + Dp(14) + description + Dp(18) + Dp(1) + Dp(14)
        + timing_h + pad;
    const ImVec2 end{at.x + width, at.y + height};
    ImVec4 fill = color::field;
    fill.w = 0.58f;
    OverlayCard(draw, at, end, Dp(16), fill, color::border);
    float y = at.y + pad;
    if (group)
    {
        std::string title = group->title;
        for (auto& c : title) c = char(std::toupper(static_cast<unsigned char>(c)));
        launcher::DrawTracked(draw, fonts.caption, {at.x + pad, y}, Col(color::dim), title.c_str(), Dp(1.5f));
    }
    y += kicker;
    // The name, and On or Off beside it.
    draw->PushClipRect(at, {end.x - pad - Dp(60), end.y}, true);
    DrawLabel(draw, fonts.button, {at.x + pad, y}, Col(color::text), info.name);
    draw->PopClipRect();
    {
        const char* state = on ? "On" : "Off";
        const ImVec2 size = TextSize(fonts.caption, state);
        const float chip_w = size.x + Dp(20), chip_h = Dp(22);
        const ImVec2 chip{end.x - pad - chip_w, y + (FontSize(fonts.button) - chip_h) * 0.5f};
        const ImVec4 tint = on ? color::accent : color::muted;
        draw->AddRectFilled(chip, {chip.x + chip_w, chip.y + chip_h}, Col(tint, 0.16f), chip_h * 0.5f);
        DrawLabel(draw, fonts.caption, {chip.x + Dp(10), chip.y + (chip_h - size.y) * 0.5f}, Col(tint), state);
    }
    y += FontSize(fonts.button) + Dp(14);
    draw->AddText(fonts.body, FontSize(fonts.body), {at.x + pad, y}, Col(Mix(color::muted, color::text, 0.25f)),
                  info.description, nullptr, wrap);
    y += description + Dp(18);
    draw->AddRectFilled({at.x + pad, y}, {end.x - pad, y + Dp(1)}, Col(color::border, 0.8f));
    y += Dp(1) + Dp(14);
    const bool now = GameOptionAtOnce(tweak);
    DrawIcon(draw, now ? Icon::Check : Icon::Info, {at.x + pad + Dp(7), y + FontSize(fonts.caption) * 0.5f}, Dp(13),
             Col(now ? color::accent : color::info));
    draw->AddText(fonts.caption, FontSize(fonts.caption), {at.x + pad + Dp(22), y}, Col(color::muted), timing.c_str(),
                  nullptr, wrap - Dp(22));
    ImGui::Dummy({width, height});
}

void DrawList(const GameOptionsModel& model, GameOptionsState& state, float width, bool describe,
              std::optional<GameOptionChange>& change)
{
    bool first = true;
    for (const auto& group : Groups())
    {
        DrawGroupHeading(group, width);
        for (const GameTweak tweak : group.tweaks)
        {
            DrawOption(model, state, tweak, width, describe, first, change);
            first = false;
        }
        ImGui::Dummy({width, Dp(8)});
    }
}
} // namespace

bool GameOption(GameTweak tweak) { return GroupOf(tweak) != nullptr; }

bool GameOptionAtOnce(GameTweak tweak) { return tweak == GameTweak::FastMenus; }

OverlayScreens::Screen GameOptionsFirstScreen() { return {0, {}, "Game options"}; }

std::string GameOptionsTitle(const OverlayScreens&) { return "Game options"; }

void DrawGameOptions(const GameOptionsModel& model, GameOptionsState& state, OverlayScreens& screens,
                     std::optional<GameOptionChange>& change, const OverlayLayout& layout)
{
    (void)screens; // one screen for now
    const float width = layout.max.x - layout.min.x;
    if (!state.shown) state.shown = Groups().front().tweaks.front();
    const bool wide = width >= Dp(700);
    OverlayLayoutIs(wide ? 2 : 1);
    if (wide)
    {
        // The list at the left, the option the cursor is on described at the right.
        const float list = OverlaySplitLeft(width), gap = OverlaySplitGap();
        BeginOverlayScroll("##options_list", layout.min, {layout.min.x + list, layout.max.y});
        ImGui::Dummy({list, Dp(20)});
        DrawNotes(model, list);
        ImGui::Dummy({list, Dp(6)});
        DrawList(model, state, list, false, change);
        EndOverlayScroll();
        OverlaySplitDivider(layout, list);
        const ImVec2 pane{layout.min.x + list + gap, layout.min.y};
        BeginOverlayScroll("##options_about", pane, layout.max);
        ImGui::Dummy({layout.max.x - pane.x, Dp(20)});
        DrawDescription(model, *state.shown, layout.max.x - pane.x);
        EndOverlayScroll();
        return;
    }
    BeginOverlayScroll("##options_column", layout.min, layout.max);
    ImGui::Dummy({width, Dp(18)});
    DrawNotes(model, width);
    ImGui::Dummy({width, Dp(6)});
    DrawList(model, state, width, true, change);
    EndOverlayScroll();
}
} // namespace mscharged::overlay
