#pragma once

#include "bootstrap/sound_groups.h"
#include "overlay/overlay_frame.h"

#include <array>
#include <optional>
#include <string>
#include <vector>

// The overlay's Settings tab (game_overlay.h): the display and sound settings
// that change at once while the game runs, in sections (Display, Sound,
// Advanced) of a small menu at the left. The first screen shows the chosen
// section beside the menu; A (or a click) opens it, B goes back to the menu.
// The host saves each change (recharged.ini) and applies it. Settings that
// need the game started again stay in the launcher, and so do the controls.
namespace mscharged::overlay
{
struct GameSettingsModel
{
    bool fullscreen = false;
    // The picture's shape: its choices' names (keep its shape, stretch to
    // fill, the wider view when set up) and the one used.
    std::vector<std::string> pictures;
    int picture = 0;
    // The 3D resolution: its choices' names and the one used.
    std::vector<std::string> resolutions;
    int resolution = 0;
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
struct GameSettingChange
{
    GameSetting setting;
    int value = 0;
    int group = 0;
};

// The tab's screens (OverlayScreens::Screen::id): the menu, and a section
// opened from it (Screen::key: its name).
enum class GameSettingsScreen
{
    Menu = 0,
    Section = 1,
};

// What the tab keeps between frames: the section the menu shows.
struct GameSettingsState
{
    int section = 0; // Display, Sound, Advanced
};

OverlayScreens::Screen GameSettingsFirstScreen();
std::string GameSettingsTitle(const OverlayScreens& screens);
void DrawGameSettings(const GameSettingsModel& model, GameSettingsState& state, OverlayScreens& screens,
                      std::optional<GameSettingChange>& change, const OverlayLayout& layout);
} // namespace mscharged::overlay
