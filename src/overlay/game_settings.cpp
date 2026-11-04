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

// The section's settings, one row each.
void DrawSection(const GameSettingsModel& model, int section, std::optional<GameSettingChange>& change, float width)
{
    constexpr bool interactive = true;
    OverlayCaption(kSections[section].about, width);
    if (section == 0)
    {
        OverlayRowGroup("WINDOW", width, [&] {
            if (OverlaySwitchRow("fullscreen", "Fullscreen", model.fullscreen, width, interactive))
                change = GameSettingChange{GameSetting::Fullscreen, model.fullscreen ? 0 : 1};
            if (model.pictures.size() > 1)
            {
                const int now = std::clamp(model.picture, 0, int(model.pictures.size()) - 1);
                const int picked = OverlaySegmentedRow("picture", "Picture", model.pictures, now, width, interactive,
                                                       model.picture_about.c_str());
                if (picked >= 0) change = GameSettingChange{GameSetting::Picture, picked};
            }
        });
        OverlayRowGroup("RENDERING", width, [&] {
            if (!model.resolutions.empty())
            {
                const int now = std::clamp(model.resolution, 0, int(model.resolutions.size()) - 1);
                const int picked = OverlaySegmentedRow("resolution", "3D resolution", model.resolutions, now, width,
                                                       interactive, model.resolution_about.c_str());
                if (picked >= 0) change = GameSettingChange{GameSetting::Resolution, picked};
            }
            if (OverlaySwitchRow("performance", "Performance numbers", model.performance_numbers, width, interactive,
                                 "The frame rate and how busy the graphics card and processor are, in a corner of "
                                 "the picture."))
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
            if (OverlaySwitchRow("chime", "Challenge sound", model.challenge_sound, width, interactive,
                                 "A chime when someone challenges you or accepts your challenge."))
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
        else OverlayCaption("The game started without the launcher: closing it opens nothing.", width);
    }
    ImGui::Dummy({width, Dp(16)});
}

// The menu's items: the sections' names and icons.
void DrawMenu(GameSettingsState& state, float width, bool row)
{
    static const std::vector<OverlaySection> sections = [] {
        std::vector<OverlaySection> items;
        for (const Section& section : kSections) items.push_back({section.name, section.icon});
        return items;
    }();
    OverlaySectionMenu(sections, state, width, row);
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
        OverlayEnterSection(state);
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
