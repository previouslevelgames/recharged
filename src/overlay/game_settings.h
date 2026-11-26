#pragma once

#include "bootstrap/sound_groups.h"

#include <array>
#include <optional>
#include <string>
#include <vector>

// The menus' SETTINGS page (src/ui/rui_pages.cpp): the display, sound,
// controller and advanced settings that change while the game runs. The host
// applies each change and saves it (recharged.ini). Files and folders, the
// window's size, antialiasing and the key and button editors stay in the
// launcher.
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
    int performance_corner = 1;       // kPerformanceCorners' order: top left, top right, ...
    bool performance_mode = false;    // lighter stadiums, from the next match
    // The picture's style: 0 modern, 1 the original TV's; modern pictures
    // drawn at another size than the window are smooth or sharp
    // (upscaling_shown), the TV's has its border or not.
    int picture_style = 0;
    bool upscaling_shown = false;
    bool sharp = true;
    bool tv_border = false;
    bool vsync = true;
    // The frame rate: its choices' names (kFrameRates), the one used (-1:
    // another one, from recharged.ini, named in frame_rate_other).
    std::vector<std::string> frame_rates;
    int frame_rate = 0;
    std::string frame_rate_other;
    // In the background: 0 no limit, else 1 + the kBackgroundFrameRates index.
    int background_rate = 0;
    int master_volume = 100;          // 0-100 %
    bool mute = false;
    SoundVolumes sound_volumes = DefaultSoundVolumes(); // kSoundGroups' order, 0-100 %
    // The controllers.
    bool rumble = true;
    int rumble_strength = 100;  // gamepads, 0-100 %
    bool mouse_pointer = true;  // the mouse points while controllers play
    bool show_hud = true;       // a match starts with its HUD shown
    bool sensor_bar_top = false; // the sensor bar above the screen (from the next start)
    int ir_sensitivity = 3;     // the Wii Remotes' pointer, 1-5 (from the next start)
    int speaker_volume = 70;    // the Wii Remotes' speakers, 0-100 % (0 off)
    // Discord: the game shown on the player's profile (builds with a Discord application).
    bool discord_available = false;
    bool discord_status = true;
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
    ResetSoundLevels,
    ReopenLauncher,
    LauncherSize,       // value: the index in launcher_sizes
    GraphicsApi,        // value: the index in graphics_apis
    GraphicsCard,       // value: the index in graphics_cards
    FrameLogging,
    GraphicsValidation,
    LogDetail,          // value: 0 errors, 1 warnings, 2 info, 3 debug
    VerboseConsole,
    LogWindow,
    PerformanceCorner, // value: the kPerformanceCorners index
    PerformanceMode,
    PictureStyle,      // value: 0 modern, 1 original TV
    Upscaling,         // value: 0 smooth, 1 sharp
    TvBorder,
    VSync,
    FrameRate,         // value: the kFrameRates index
    BackgroundRate,    // value: 0 none, else 1 + the kBackgroundFrameRates index
    Rumble,
    RumbleStrength,    // value: 0-100
    MousePointer,
    ShowHud,
    SensorBar,         // value: 0 below the screen, 1 above it
    IrSensitivity,     // value: 1-5
    SpeakerVolume,     // value: 0-100
    DiscordStatus,
};

// The performance numbers' corners and the frame rates the page offers, as
// recharged.ini has them (display.fps_overlay_corner, display.frame_rate).
inline constexpr const char* kPerformanceCorners[] = {"top-left", "top-right", "bottom-left", "bottom-right"};
inline constexpr const char* kFrameRates[] = {"original", "display", "30", "60", "120", "144", "unlimited"};

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
