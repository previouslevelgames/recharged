#pragma once

#include "bootstrap/sound_groups.h"

#include <array>
#include <optional>
#include <string>
#include <vector>

// The menus' SETTINGS page (src/ui/rui_pages.cpp): the display, sound and
// advanced settings that change while the game runs. The host applies each
// change and saves it (recharged.ini). Files and folders stay in the
// launcher, and so do the controls.
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
    // The launcher's Advanced settings (not its files and folders): the
    // launcher's own (its size), the next start's graphics and the
    // diagnostics. Lists hold their choices' names, the int the one chosen.
    std::vector<std::string> launcher_sizes;
    int launcher_size = 0;
    std::vector<std::string> graphics_apis; // one name only: nothing to choose (Linux, macOS)
    int graphics_api = 0;
    bool graphics_cards_shown = false;      // the PC lists its cards (not macOS)
    std::vector<std::string> graphics_cards; // "Automatic (fastest)" first
    int graphics_card = 0;
    bool graphics_cards_listing = false;    // still looking for them
    bool frame_logging = false;
    bool graphics_validation = false;
    int log_detail = 2; // errors, warnings, info, debug
    bool verbose_console = false;
    bool log_window_shown = false; // Windows and macOS
    bool log_window = false;
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
    LauncherSize,       // value: the index in launcher_sizes
    GraphicsApi,        // value: the index in graphics_apis
    GraphicsCard,       // value: the index in graphics_cards
    FrameLogging,
    GraphicsValidation,
    LogDetail,          // value: 0 errors, 1 warnings, 2 info, 3 debug
    VerboseConsole,
    LogWindow,
};

// A setting changed on the page; switches carry their new state in `value` (0 or 1).
// `live`: a slider being dragged: applied now, saved once it is let go.
struct GameSettingChange
{
    GameSetting setting;
    int value = 0;
    int group = 0;
    bool live = false;
};

} // namespace mscharged::overlay
