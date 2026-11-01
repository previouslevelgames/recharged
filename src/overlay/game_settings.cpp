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

// A volume's step: ten points up or down, from 0 to 100 (no wrapping round).
int Volume(int value, int step) { return std::clamp((value + 5) / 10 * 10 + step * 10, 0, 100); }

std::string Percent(int value) { return std::to_string(value) + " %"; }

// The section's settings, one row each; `interactive` false draws them only (the menu's preview).
void DrawSection(const GameSettingsModel& model, int section, std::optional<GameSettingChange>& change, float width,
                 bool interactive)
{
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
            if (const int step =
                    OverlayChoiceRow("master", "Master volume", Percent(model.master_volume), width, interactive))
                change = GameSettingChange{GameSetting::MasterVolume, Volume(model.master_volume, step)};
            if (OverlaySwitchRow("mute", "Mute", model.mute, width, interactive))
                change = GameSettingChange{GameSetting::Mute, model.mute ? 0 : 1};
        });
        OverlayRowGroup("SOUND LEVELS", width, [&] {
            for (std::size_t n = 0; n < kSoundGroups.size(); ++n)
            {
                const int value = model.sound_volumes[n];
                if (const int step =
                        OverlayChoiceRow(kSoundGroups[n].key, kSoundGroups[n].name, Percent(value), width, interactive))
                    change = GameSettingChange{GameSetting::SoundVolume, Volume(value, step), int(n)};
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

// The menu at the left: the sections, the chosen one lit. On the first
// screen its items take the cursor (the one it is on is the one shown) and
// open their section; beside a section they only switch it by the mouse.
void DrawMenu(GameSettingsState& state, OverlayScreens& screens, ImVec2 at, float width, bool first)
{
    const auto& fonts = launcher::CurrentFonts();
    auto* draw = ImGui::GetWindowDrawList();
    const float height = Dp(46), gap = Dp(6);
    if (!first) ImGui::PushItemFlag(ImGuiItemFlags_NoNav, true);
    for (int n = 0; n < kSectionCount; ++n)
    {
        const ImVec2 min{at.x, at.y + float(n) * (height + gap)};
        const ImVec2 max{min.x + width, min.y + height};
        ImGui::SetCursorScreenPos(min);
        ImGui::PushID(n);
        const bool pressed = ImGui::InvisibleButton("##section", {width, height}, ImGuiButtonFlags_EnableNav);
        const ImGuiID item = ImGui::GetItemID();
        const bool hovered = ImGui::IsItemHovered();
        const bool focused = first && OverlayNavFocused();
        if (first) OverlayFocusable(n == state.section);
        ImGui::PopID();
        if (focused) state.section = n;
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
        if (pressed)
        {
            state.section = n;
            if (first) screens.Push({int(GameSettingsScreen::Section), kSections[n].name, kSections[n].name}, item);
            else if (screens.Top().key != kSections[n].name)
            {
                // Another section in place of this one: back still goes to the menu.
                screens.Pop();
                screens.Push({int(GameSettingsScreen::Section), kSections[n].name, kSections[n].name}, 0);
            }
        }
    }
    if (!first) ImGui::PopItemFlag();
}
} // namespace

OverlayScreens::Screen GameSettingsFirstScreen() { return {int(GameSettingsScreen::Menu), {}, "Settings"}; }

std::string GameSettingsTitle(const OverlayScreens& screens)
{
    if (screens.Empty() || screens.Top().id == int(GameSettingsScreen::Menu)) return "Settings";
    return screens.Top().title;
}

void DrawGameSettings(const GameSettingsModel& model, GameSettingsState& state, OverlayScreens& screens,
                      std::optional<GameSettingChange>& change, const OverlayLayout& layout)
{
    if (screens.Empty()) return;
    const bool first = screens.Top().id == int(GameSettingsScreen::Menu);
    if (!first)
        for (int n = 0; n < kSectionCount; ++n)
            if (screens.Top().key == kSections[n].name) state.section = n;
    state.section = std::clamp(state.section, 0, kSectionCount - 1);
    const float total = layout.max.x - layout.min.x;
    const float menu = std::min(Dp(220), total * 0.3f), gap = Dp(24);
    const float width = std::min(total - menu - gap, Dp(620));
    const float left = layout.min.x + (total - menu - gap - width) * 0.5f;
    // The menu in a part of its own, as the overlay's lists are.
    if (!first) ImGui::PushItemFlag(ImGuiItemFlags_NoNav, true);
    BeginOverlayScroll("##settings_menu", {left, layout.min.y}, {left + menu, layout.max.y});
    if (!first) ImGui::PopItemFlag();
    DrawMenu(state, screens, {ImGui::GetCursorScreenPos().x, ImGui::GetCursorScreenPos().y + Dp(20)}, menu, first);
    EndOverlayScroll();
    const ImVec2 content{left + menu + gap, layout.min.y};
    // Beside the menu, the section is a preview the cursor doesn't go to.
    if (first) ImGui::PushItemFlag(ImGuiItemFlags_NoNav, true);
    BeginOverlayScroll(("##settings_" + std::to_string(state.section) + (first ? "_preview" : "")).c_str(), content,
                       {content.x + width, layout.max.y});
    ImGui::Dummy({width, Dp(12)});
    DrawSection(model, state.section, change, width, !first);
    EndOverlayScroll();
    if (first) ImGui::PopItemFlag();
}
} // namespace mscharged::overlay
