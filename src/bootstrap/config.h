#pragma once

#include <filesystem>
#include <string>

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
};

struct ConfigFile
{
    std::filesystem::path path;
    std::string contents;
    bool exists = false;
    Settings settings;
};

ConfigFile LoadConfig(const std::filesystem::path& path, bool allow_missing = false);
void SaveConfig(ConfigFile& file, const Settings& settings);
std::filesystem::path ResolveDiscPath(const Settings& settings, const std::filesystem::path& config);
std::filesystem::path LoadDiscPath(const std::filesystem::path& config);
}
