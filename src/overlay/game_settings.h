#pragma once

#include "bootstrap/sound_groups.h"
#include "overlay/overlay_frame.h"

#include <array>
#include <optional>
#include <string>
#include <vector>

// The overlay's Settings tab (game_overlay.h): the display and sound settings
// that change at once while the game runs, in sections (Display, Sound,
// Advanced) of a menu at the left, split as the other tabs. One section is
// always shown, the first at the start: the cursor on a menu item shows its
// section, whose rows are beside it; B closes the overlay. The host applies
// each change and saves it (recharged.ini). Settings that need the game
// started again stay in the launcher, and so do the controls.
namespace mscharged::overlay
{
struct GameSettingsModel
{
    bool fullscreen = false;
    // The picture's shape: its choices' short names, as the launcher's (16:9
    // or 4:3, Stretch, the wider view when set up), the one used, and what
    // they do.
    std::vector<std::string> pictures;
    int picture = 0;
    std::string picture_about;
    // The 3D resolution: its choices' short names (Full, Native, 2x...), the
    // one used, and what they draw at.
    std::vector<std::string> resolutions;
    int resolution = 0;
    std::string resolution_about;
    bool performance_numbers = false; // FPS, GPU and CPU over the picture
    int master_volume = 100;          // 0-100 %
    bool mute = false;
    SoundVolumes sound_volumes = DefaultSoundVolumes(); // kSoundGroups' order, 0-100 %
    bool challenge_sound = true;
    bool reopen_launcher = false; // the launcher opens again when the game closes
    bool reopen_known = true;     // the game was started from the launcher: it can open again
};

enum class GameSetting
{
    Fullscreen,
    Picture,        // value: the index in GameSettingsModel::pictures
    Resolution,     // value: the index in GameSettingsModel::resolutions
    PerformanceNumbers,
    MasterVolume,   // value: 0-100
    Mute,
    SoundVolume,    // group: the kSoundGroups index; value: 0-100
    ChallengeSound,
    ReopenLauncher,
};

// A setting changed in the tab; switches carry their new state in `value` (0 or 1).
// `live`: a slider being dragged: applied now, saved once it is let go.
struct GameSettingChange
{
    GameSetting setting;
    int value = 0;
    int group = 0;
    bool live = false;
};

// The tab's screen (OverlayScreens::Screen::id): the menu with its section.
enum class GameSettingsScreen
{
    Menu = 0,
};

// What the tab keeps between frames: the section shown, and whether the
// cursor was on the menu (coming from the section it lands on that one).
struct GameSettingsState
{
    int section = 0; // Display, Sound, Advanced
    bool menu_focused = false;
    bool enter_section = false; // right on its menu item: the cursor goes to its first row
};

OverlayScreens::Screen GameSettingsFirstScreen();
std::string GameSettingsTitle(const OverlayScreens& screens);
void DrawGameSettings(const GameSettingsModel& model, GameSettingsState& state, OverlayScreens& screens,
                      std::optional<GameSettingChange>& change, const OverlayLayout& layout);
} // namespace mscharged::overlay
