#include "overlay/game_settings.h"

#include "launcher/ui_kit.h"
#include "overlay/overlay_rows.h"

#include "imgui_internal.h"

#include <algorithm>
#include <cfloat>

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

struct Section
{
    const char* name;
    Icon icon;
    const char* about;
};
constexpr Section kSections[] = {
    {"Display", Icon::Display, "The window and the picture. Changes show at once and stay for the next time."},
    {"Sound", Icon::Audio, "The volume of the game's sounds, at once; saved for the next time."},
    {"Advanced", Icon::Gear, "More settings, the controls and those that need a new start are in the launcher."},
};
constexpr int kSectionCount = int(std::size(kSections));

void Caption(const char* text, float width)
{
    const auto& fonts = launcher::CurrentFonts();
    const ImVec2 at = ImGui::GetCursorScreenPos();
    const float height = fonts.caption->CalcTextSizeA(FontSize(fonts.caption), FLT_MAX, width, text).y;
    ImGui::GetWindowDrawList()->AddText(fonts.caption, FontSize(fonts.caption), at, Col(color::muted), text, nullptr,
                                        width);
    ImGui::Dummy({width, height});
}

// The section's settings, one row each.
void DrawSection(const GameSettingsModel& model, int section, std::optional<GameSettingChange>& change, float width)
{
    constexpr bool interactive = true;
    Caption(kSections[section].about, width);
    if (section == 0)
    {
        OverlayRowGroup("WINDOW", width, [&] {
            if (OverlaySwitchRow("fullscreen", "Fullscreen", model.fullscreen, width, interactive))
                change = GameSettingChange{GameSetting::Fullscreen, model.fullscreen ? 0 : 1};
            if (model.pictures.size() > 1)
            {
                const int count = int(model.pictures.size());
                const int now = std::clamp(model.picture, 0, count - 1);
                if (const int step = OverlayChoiceRow("picture", "Picture", model.pictures[std::size_t(now)], width,
                                                      interactive))
                    change = GameSettingChange{GameSetting::Picture, (now + step + count) % count};
            }
        });
        OverlayRowGroup("RENDERING", width, [&] {
            if (!model.resolutions.empty())
            {
                const int count = int(model.resolutions.size());
                const int now = std::clamp(model.resolution, 0, count - 1);
                if (const int step = OverlayChoiceRow("resolution", "3D resolution",
                                                      model.resolutions[std::size_t(now)], width, interactive))
                    change = GameSettingChange{GameSetting::Resolution, std::clamp(now + step, 0, count - 1)};
            }
            if (OverlaySwitchRow("performance", "Performance numbers", model.performance_numbers, width, interactive))
                change = GameSettingChange{GameSetting::PerformanceNumbers, model.performance_numbers ? 0 : 1};
        });
    }
    else if (section == 1)
    {
        OverlayRowGroup("MASTER", width, [&] {
            bool done = false;
            if (const auto volume = OverlaySliderRow("master", "Master volume", model.master_volume, width, done))
                change = GameSettingChange{GameSetting::MasterVolume, *volume, 0, !done};
            if (OverlaySwitchRow("mute", "Mute", model.mute, width, interactive))
                change = GameSettingChange{GameSetting::Mute, model.mute ? 0 : 1};
        });
        OverlayRowGroup("SOUND LEVELS", width, [&] {
            for (std::size_t n = 0; n < kSoundGroups.size(); ++n)
            {
                bool done = false;
                if (const auto volume = OverlaySliderRow(kSoundGroups[n].key, kSoundGroups[n].name,
                                                         model.sound_volumes[n], width, done))
                    change = GameSettingChange{GameSetting::SoundVolume, *volume, int(n), !done};
            }
        });
        OverlayRowGroup("ONLINE", width, [&] {
            if (OverlaySwitchRow("chime", "Challenge sound", model.challenge_sound, width, interactive))
                change = GameSettingChange{GameSetting::ChallengeSound, model.challenge_sound ? 0 : 1};
        });
    }
    else
    {
        if (model.reopen_known)
            OverlayRowGroup("LAUNCHER", width, [&] {
                if (OverlaySwitchRow("reopen", "Reopen the launcher after the game", model.reopen_launcher, width,
                                     interactive))
                    change = GameSettingChange{GameSetting::ReopenLauncher, model.reopen_launcher ? 0 : 1};
            });
        else Caption("The game started without the launcher: closing it opens nothing.", width);
    }
    ImGui::Dummy({width, Dp(16)});
}

// The menu: the sections, the one shown lit; a column at the left, or in one
// column a row above its section. The cursor on an item shows its section,
// and right (down in a row) goes into it; coming back, the cursor lands on its
// item rather than on whichever is nearest.
void DrawMenu(GameSettingsState& state, float width, bool row)
{
    const auto& fonts = launcher::CurrentFonts();
    auto* draw = ImGui::GetWindowDrawList();
    const float height = Dp(46), gap = Dp(6);
    const float item_w = row ? (width - gap * float(kSectionCount - 1)) / float(kSectionCount) : width;
    const ImVec2 at = ImGui::GetCursorScreenPos();
    ImGuiID shown = 0;
    int focused_item = -1;
    for (int n = 0; n < kSectionCount; ++n)
    {
        const ImVec2 min = row ? ImVec2{at.x + float(n) * (item_w + gap), at.y}
                               : ImVec2{at.x, at.y + float(n) * (height + gap)};
        const ImVec2 max{min.x + item_w, min.y + height};
        ImGui::SetCursorScreenPos(min);
        ImGui::PushID(n);
        const bool pressed = ImGui::InvisibleButton("##section", {item_w, height}, ImGuiButtonFlags_EnableNav);
        if (n == state.section) shown = ImGui::GetItemID();
        const bool hovered = ImGui::IsItemHovered();
        const bool focused = OverlayNavFocused();
        OverlayFocusable(n == state.section);
        ImGui::PopID();
        if (focused) focused_item = n;
        if (pressed) state.section = n;
        const bool chosen = n == state.section;
        const float lit = launcher::Animate(ImGui::GetID(("##section_lit" + std::to_string(n)).c_str()),
                                            chosen || hovered, 12.0f);
        ImVec4 fill = Mix(color::field, color::accent_dark, chosen ? 0.55f : 0.0f);
        fill.w = chosen ? 0.9f : 0.35f + 0.3f * lit;
        draw->AddRectFilled(min, max, Col(fill), Dp(12));
        if (chosen) draw->AddRect(min, max, Col(color::accent, 0.55f), Dp(12), 0, Dp(1));
        const ImU32 ink = Col(chosen ? color::text : Mix(color::muted, color::text, lit));
        DrawIcon(draw, kSections[n].icon, {min.x + Dp(26), min.y + height * 0.5f}, Dp(18), ink);
        DrawLabel(draw, fonts.label, {min.x + Dp(48), min.y + (height - FontSize(fonts.label)) * 0.5f}, ink,
                  kSections[n].name);
        if (focused) OverlayFocusRing(draw, min, max, Dp(12));
    }
    // The cursor moved along the menu: its section shows. It came from the
    // section: back to the item shown.
    if (focused_item >= 0 && focused_item != state.section)
    {
        if (state.menu_focused) state.section = focused_item;
        else if (shown)
        {
            ImGui::SetFocusID(shown, ImGui::GetCurrentWindow());
            ImGui::SetNavCursorVisible(true);
        }
    }
    // Into the section: its rows needn't be level with the item (the
    // cursor only goes sideways to items level with it).
    if (focused_item >= 0 && focused_item == state.section && !row
        && (ImGui::IsKeyPressed(ImGuiKey_RightArrow) || ImGui::IsKeyPressed(ImGuiKey_GamepadDpadRight)
            || ImGui::IsKeyPressed(ImGuiKey_GamepadLStickRight)))
        state.enter_section = true;
    state.menu_focused = focused_item >= 0;
    ImGui::SetCursorScreenPos({at.x, at.y + (row ? height : float(kSectionCount) * (height + gap))});
    ImGui::Dummy({width, 0.0f});
}
} // namespace

OverlayScreens::Screen GameSettingsFirstScreen() { return {int(GameSettingsScreen::Menu), {}, "Settings"}; }

std::string GameSettingsTitle(const OverlayScreens&) { return "Settings"; }

void DrawGameSettings(const GameSettingsModel& model, GameSettingsState& state, OverlayScreens& screens,
                      std::optional<GameSettingChange>& change, const OverlayLayout& layout)
{
    if (screens.Empty()) return;
    state.section = std::clamp(state.section, 0, kSectionCount - 1);
    const float total = layout.max.x - layout.min.x;
    const bool wide = total >= Dp(700);
    OverlayLayoutIs(wide ? 2 : 1);
    if (wide)
    {
        // The menu at the left, its section beside it, split as the other tabs.
        const float left = OverlaySplitLeft(total);
        BeginOverlayScroll("##settings_menu", layout.min, {layout.min.x + left, layout.max.y});
        ImGui::Dummy({left, Dp(20)});
        DrawMenu(state, left, false);
        EndOverlayScroll();
        OverlaySplitDivider(layout, left);
        const ImVec2 pane{layout.min.x + left + OverlaySplitGap(), layout.min.y};
        const float width = std::min(layout.max.x - pane.x, Dp(720));
        BeginOverlayScroll(("##settings_" + std::to_string(state.section)).c_str(), pane,
                           {pane.x + width, layout.max.y});
        ImGui::Dummy({width, Dp(14)});
        // Right from its menu item: the cursor on the section's first row.
        if (state.enter_section)
        {
            ImGui::SetKeyboardFocusHere();
            ImGui::SetNavCursorVisible(true);
            state.enter_section = false;
        }
        DrawSection(model, state.section, change, width);
        EndOverlayScroll();
        return;
    }
    // One column: the menu as a row above its section.
    BeginOverlayScroll("##settings_column", layout.min, layout.max);
    ImGui::Dummy({total, Dp(14)});
    DrawMenu(state, total, true);
    ImGui::Dummy({total, Dp(10)});
    DrawSection(model, state.section, change, total);
    EndOverlayScroll();
}
} // namespace mscharged::overlay
