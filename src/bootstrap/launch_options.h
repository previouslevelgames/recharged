#pragma once

#include "bootstrap/config.h"
#include <optional>
#include <string_view>

namespace mscharged
{
struct WindowSize { int width, height; };

// Command-line values are independent from the editable/saved INI settings.
struct LaunchOptions
{
    std::filesystem::path config;
    bool explicit_config = false;
    std::optional<std::filesystem::path> disc;
    std::optional<bool> fullscreen;
    std::optional<WindowSize> size;
    std::optional<std::string> aspect;
    std::optional<std::string> frame_rate;
    bool no_tweaks = false; // --no-tweaks: play without the [gameplay] tweaks
};

enum class SettingSource { Defaults, Config, Launcher, CommandLine, GameMode };
struct ResolvedLaunch
{
    ConfigFile config;
    Settings settings;
    std::filesystem::path disc_path;
    SettingSource disc_source = SettingSource::Defaults;
    SettingSource width_source = SettingSource::Defaults;
    SettingSource height_source = SettingSource::Defaults;
    SettingSource fullscreen_source = SettingSource::Defaults;
    SettingSource aspect_source = SettingSource::Defaults;
    SettingSource frame_rate_source = SettingSource::Defaults;
    SettingSource tweaks_source = SettingSource::Defaults;
};

// Consume a shared option at argv[index], including its value where required.
// Return false without changing index for runtime-specific/unknown arguments.
bool ParseLaunchOption(int argc, const char* const* argv, int& index, LaunchOptions& options);
std::filesystem::path DefaultConfigPath(const std::filesystem::path& executable_directory);
ResolvedLaunch ResolveLaunch(const ConfigFile& config, const LaunchOptions& options,
    const Settings* launcher_settings = nullptr,
    const std::filesystem::path& working_directory = std::filesystem::current_path());
ResolvedLaunch LoadLaunch(const LaunchOptions& options, const std::filesystem::path& executable_directory);
// Steam's Game Mode runs games inside gamescope, which shows one window and
// sizes it to the screen only when it asks for fullscreen. There the game
// opens fullscreen unless --window asked for a window on this run.
bool UnderGamescope();
void ApplyGameModeDisplay(ResolvedLaunch& launch, bool game_mode);
std::string DescribeLaunch(const ResolvedLaunch& launch);
const char* SettingSourceName(SettingSource source);
inline constexpr std::string_view LaunchOptionsHelp =
    "Launch settings: [--config FILE] [--disc FILE | --disk FILE] [--window | --fullscreen] [--size WIDTHxHEIGHT] [--aspect auto|4:3|16:9|stretch] [--frame-rate original|display|unlimited|FPS] [--no-tweaks]\n"
    "Command-line settings override the INI for this run only; later options win.\n"
    "--frame-rate takes original, display, unlimited or 30 to 1000 frames per second.\n"
    "--no-tweaks turns the [gameplay] tweaks off for this run.\n"
    "Options with values accept --option VALUE or --option=VALUE, before or after the runtime mode.\n"
    "INI disc paths are relative to that INI; command-line paths are relative to the working directory.\n";
}
