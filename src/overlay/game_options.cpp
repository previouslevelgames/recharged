#include "overlay/game_options.h"

#include "imgui_internal.h"

#include <algorithm>
#include <cfloat>
#include <string>
#include <vector>

namespace mscharged::overlay
{
namespace
{
namespace color = launcher::color;
using launcher::Col;
using launcher::Dp;
using launcher::DrawIcon;
using launcher::FontSize;
using launcher::Icon;
using launcher::Mix;

// The options the game offers, in the categories and order of the launcher's
// Gameplay page (whose other options stay there): the menu's sections.
struct Group
{
    const char* title;
    const char* short_title; // in the one-column menu
    Icon icon;
    const char* about;
    std::vector<GameTweak> tweaks;
};

const std::vector<Group>& Groups()
{
    static const std::vector<Group> groups{
        {"Menus & startup", "Menus", Icon::Play,
         "How the menus run; a change takes effect at once. Skip intro is in the launcher: it needs the game "
         "started again.",
         {GameTweak::FastMenus}},
        {"Matches & unlocks", "Matches", Icon::Target,
         "Who plays and how the teams look; changes take effect from the next match. Robot goalies, Super Team and "
         "Unlock everything are in the launcher: they need the game started again.",
         {GameTweak::CpuVsCpu, GameTweak::MirrorMatch, GameTweak::BluePeach}},
        {"Competitive gameplay", "Competitive", Icon::Trophy,
         "Rules for fairer matches; changes take effect from the next match.",
         {GameTweak::FastTerrain, GameTweak::SecureStadiums, GameTweak::ThunderIslandWalls,
          GameTweak::FairBallContests, GameTweak::NoMegaStrikes, GameTweak::MatchPoint}},
    };
    return groups;
}

float WrappedHeight(ImFont* font, const char* text, float width)
{
    return font->CalcTextSizeA(FontSize(font), FLT_MAX, width, text).y;
}

// What the player should know about every section: a running match keeps
// what it started with, and online matches play by their own rules.
void DrawNotes(const GameOptionsModel& model, float width)
{
    const launcher::Fonts& fonts = launcher::CurrentFonts();
    auto* draw = ImGui::GetWindowDrawList();
    const ImVec2 at = ImGui::GetCursorScreenPos();
    const float pad = Dp(14), wrap = width - 2 * pad - Dp(24);
    const char* match = "A match is on: it keeps the options it started with.";
    const char* online = "Online matches play by their own rules.";
    const float online_h = WrappedHeight(fonts.caption, online, wrap);
    const float height = pad + (model.in_match ? WrappedHeight(fonts.body, match, wrap) + Dp(4) : 0.0f) + online_h + pad;
    const ImVec4 tint = model.in_match ? color::info : color::border;
    draw->AddRectFilled(at, {at.x + width, at.y + height},
                        Col(Mix(color::field, tint, model.in_match ? 0.10f : 0.0f), 0.8f), Dp(12));
    draw->AddRect(at, {at.x + width, at.y + height}, Col(tint, model.in_match ? 0.45f : 0.9f), Dp(12), 0, Dp(1));
    float y = at.y + pad;
    DrawIcon(draw, Icon::Info,
             {at.x + pad + Dp(8), y + (model.in_match ? FontSize(fonts.body) : FontSize(fonts.caption)) * 0.5f}, Dp(15),
             Col(model.in_match ? color::info : color::dim));
    if (model.in_match)
    {
        draw->AddText(fonts.body, FontSize(fonts.body), {at.x + pad + Dp(24), y}, Col(color::text), match, nullptr,
                      wrap);
        y += WrappedHeight(fonts.body, match, wrap) + Dp(4);
    }
    draw->AddText(fonts.caption, FontSize(fonts.caption), {at.x + pad + Dp(24), y}, Col(color::muted), online, nullptr,
                  wrap);
    ImGui::Dummy({width, height});
}

// The menu: the sections, each with how many of its options are on.
void DrawMenu(const GameOptionsModel& model, GameOptionsState& state, float width, bool row)
{
    std::vector<OverlaySection> sections;
    for (const Group& group : Groups())
    {
        const auto on = std::count_if(group.tweaks.begin(), group.tweaks.end(),
                                      [&](GameTweak tweak) { return model.tweaks[GameTweakIndex(tweak)]; });
        sections.push_back({group.title, group.icon, group.short_title, on ? std::to_string(on) + " on" : std::string()});
    }
    OverlaySectionMenu(sections, state, width, row);
}

// The section shown: what its options do and when, then their switches,
// each with its description. Beside the menu (`beside`), left on a switch
// goes back to it.
void DrawSection(const GameOptionsModel& model, GameOptionsState& state, const Group& group,
                 std::optional<GameOptionChange>& change, float width, bool beside)
{
    OverlayCaption(group.about, width);
    OverlayRowGroup("", width, [&] {
        for (const GameTweak tweak : group.tweaks)
        {
            const std::size_t index = GameTweakIndex(tweak);
            const GameTweakInfo& info = kGameTweaks[index];
            const bool on = model.tweaks[index];
            if (OverlaySwitchRow(info.key, info.name, on, width, true, info.description))
                change = GameOptionChange{tweak, !on};
            if (beside) OverlaySectionRowBack(state);
        }
    });
    ImGui::Dummy({width, Dp(16)});
}
} // namespace

bool GameOption(GameTweak tweak)
{
    for (const auto& group : Groups())
        if (std::find(group.tweaks.begin(), group.tweaks.end(), tweak) != group.tweaks.end()) return true;
    return false;
}

bool GameOptionAtOnce(GameTweak tweak) { return tweak == GameTweak::FastMenus; }

OverlayScreens::Screen GameOptionsFirstScreen() { return {0, {}, "Game options"}; }

std::string GameOptionsTitle(const OverlayScreens&) { return "Game options"; }

void DrawGameOptions(const GameOptionsModel& model, GameOptionsState& state, OverlayScreens& screens,
                     std::optional<GameOptionChange>& change, const OverlayLayout& layout)
{
    (void)screens; // one screen: the menu with its section
    const auto& groups = Groups();
    state.section = std::clamp(state.section, 0, int(groups.size()) - 1);
    const float total = layout.max.x - layout.min.x;
    const bool wide = total >= Dp(700);
    OverlayLayoutIs(wide ? 2 : 1);
    if (wide)
    {
        // The menu at the left, its section beside it, split as the other tabs.
        const float left = OverlaySplitLeft(total);
        BeginOverlayScroll("##options_menu", layout.min, {layout.min.x + left, layout.max.y});
        ImGui::Dummy({left, Dp(20)});
        DrawMenu(model, state, left, false);
        ImGui::Dummy({left, Dp(14)});
        DrawNotes(model, left);
        EndOverlayScroll();
        OverlaySplitDivider(layout, left);
        const ImVec2 pane{layout.min.x + left + OverlaySplitGap(), layout.min.y};
        const float width = std::min(layout.max.x - pane.x, Dp(720));
        BeginOverlayScroll(("##options_" + std::to_string(state.section)).c_str(), pane,
                           {pane.x + width, layout.max.y});
        ImGui::Dummy({width, Dp(14)});
        // Right from its menu item: the cursor on the section's first option.
        OverlayEnterSection(state);
        DrawSection(model, state, groups[std::size_t(state.section)], change, width, true);
        EndOverlayScroll();
        return;
    }
    // One column: the menu as a row above its section.
    BeginOverlayScroll("##options_column", layout.min, layout.max);
    ImGui::Dummy({total, Dp(14)});
    DrawMenu(model, state, total, true);
    ImGui::Dummy({total, Dp(10)});
    DrawNotes(model, total);
    ImGui::Dummy({total, Dp(14)});
    OverlayEnterSection(state);
    DrawSection(model, state, groups[std::size_t(state.section)], change, total, false);
    EndOverlayScroll();
}
} // namespace mscharged::overlay
