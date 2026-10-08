#pragma once

#include <filesystem>
#include <string>
#include <set>

namespace mscharged
{
struct Settings
{
    std::string disc;
    std::string language = "auto";
    int width = 1280;
    int height = 720;
    bool fullscreen = false;
    bool vsync = true;
    std::string backend = "auto";
    std::string aspect = "16:9";
    int master_volume = 100;
    int music_volume = 100;
    int effects_volume = 100;
    bool mute = false;
    std::string input = "auto";
    int deadzone = 15;
    bool rumble = true;
    // Host presentation and diagnostics (never game behaviour).
    bool show_fps = true;             // display.show_fps: frame rate in the game window title
    int monitor = 0;                  // display.monitor: 0 = default display, N = Nth connected display
    bool graphics_validation = true;  // advanced.graphics_validation: backend graphics debug layers
    std::string log_level = "info";   // advanced.log_level: error | warning | info | debug
    std::string ui_scale = "auto";    // launcher.ui_scale: auto or a percentage (75 to 200)
};

struct ConfigFile
{
    std::filesystem::path path;
    std::string contents;
    bool exists = false;
    Settings settings;
    std::set<std::string> configured_keys; // Parsed INI provenance; run overrides never enter this set.
};

ConfigFile LoadConfig(const std::filesystem::path& path, bool allow_missing = false);
void SaveConfig(ConfigFile& file, const Settings& settings);
std::filesystem::path ResolveDiscPath(const Settings& settings, const std::filesystem::path& config);
std::filesystem::path LoadDiscPath(const std::filesystem::path& config);
}
