#include "bootstrap/launch_options.h"
#include "platform/path.h"
#include <charconv>
#include <stdexcept>

namespace mscharged
{
namespace
{
int Dimension(std::string_view value)
{
    int result = 0;
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result);
    if (value.empty() || parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size()
        || result < 1 || result > 16384)
        throw std::invalid_argument("Window size requires WIDTHxHEIGHT with dimensions between 1 and 16384");
    return result;
}

SettingSource Source(const ConfigFile& config, const char* key)
{
    return config.configured_keys.count(key) ? SettingSource::Config : SettingSource::Defaults;
}

void ValidateAspect(std::string_view value)
{
    if (value != "auto" && value != "4:3" && value != "16:9")
        throw std::invalid_argument("Aspect requires auto, 4:3 or 16:9");
}
}

bool ParseLaunchOption(int argc, const char* const* argv, int& index, LaunchOptions& options)
{
    const std::string_view argument = argv[index];
    if (argument == "--window") { options.fullscreen = false; return true; }
    if (argument == "--fullscreen") { options.fullscreen = true; return true; }
    if (argument != "--config" && argument != "--disc" && argument != "--disk"
        && argument != "--size" && argument != "--aspect")
        return false;
    if (index + 1 >= argc || std::string_view(argv[index + 1]).empty()
        || std::string_view(argv[index + 1]).substr(0, 2) == "--")
        throw std::invalid_argument(std::string(argument) + " requires a value");
    const std::string_view value = argv[++index];
    if (argument == "--config") { options.config = PathFromUtf8(value); options.explicit_config = true; }
    else if (argument == "--disc" || argument == "--disk") options.disc = PathFromUtf8(value);
    else if (argument == "--aspect") { ValidateAspect(value); options.aspect = value; }
    else
    {
        const auto separator = value.find('x');
        if (separator == std::string_view::npos)
            throw std::invalid_argument("Window size requires WIDTHxHEIGHT");
        options.size = WindowSize{Dimension(value.substr(0, separator)), Dimension(value.substr(separator + 1))};
    }
    return true;
}

std::filesystem::path DefaultConfigPath(const std::filesystem::path& executable_directory)
{
    namespace fs = std::filesystem;
    if (fs::exists("mscharged.ini")) return fs::absolute("mscharged.ini");
    for (auto directory = executable_directory; !directory.empty(); directory = directory.parent_path())
    {
        if (fs::exists(directory / "mscharged.ini.example")) return directory / "mscharged.ini";
        if (directory == directory.parent_path()) break;
    }
    return executable_directory / "mscharged.ini";
}

ResolvedLaunch ResolveLaunch(const ConfigFile& config, const LaunchOptions& options,
    const Settings* launcher_settings, const std::filesystem::path& working_directory)
{
    ResolvedLaunch result;
    result.config = config; // Keep original contents/settings for explicit SaveConfig callers.
    result.settings = launcher_settings ? *launcher_settings : config.settings;
    result.disc_source = Source(config, "game.disc");
    result.width_source = Source(config, "display.width");
    result.height_source = Source(config, "display.height");
    result.fullscreen_source = Source(config, "display.fullscreen");
    result.aspect_source = Source(config, "display.aspect");
    if (launcher_settings)
    {
        if (launcher_settings->disc != config.settings.disc) result.disc_source = SettingSource::Launcher;
        if (launcher_settings->width != config.settings.width) result.width_source = SettingSource::Launcher;
        if (launcher_settings->height != config.settings.height) result.height_source = SettingSource::Launcher;
        if (launcher_settings->fullscreen != config.settings.fullscreen) result.fullscreen_source = SettingSource::Launcher;
        if (launcher_settings->aspect != config.settings.aspect) result.aspect_source = SettingSource::Launcher;
    }
    if (options.disc)
    {
        if (options.disc->empty()) throw std::invalid_argument("Command-line disc path is empty");
        result.disc_path = options.disc->is_absolute() ? *options.disc : working_directory / *options.disc;
        result.disc_path = std::filesystem::absolute(result.disc_path).lexically_normal();
        result.settings.disc = PathUtf8(result.disc_path);
        result.disc_source = SettingSource::CommandLine;
    }
    else if (!result.settings.disc.empty())
        result.disc_path = ResolveDiscPath(result.settings, config.path).lexically_normal();
    if (options.size)
    {
        if (options.size->width < 1 || options.size->width > 16384
            || options.size->height < 1 || options.size->height > 16384)
            throw std::invalid_argument("Window dimensions must be between 1 and 16384");
        result.settings.width = options.size->width; result.settings.height = options.size->height;
        result.width_source = result.height_source = SettingSource::CommandLine;
    }
    if (options.fullscreen)
    {
        result.settings.fullscreen = *options.fullscreen;
        result.fullscreen_source = SettingSource::CommandLine;
    }
    if (options.aspect)
    {
        ValidateAspect(*options.aspect);
        result.settings.aspect = *options.aspect;
        result.aspect_source = SettingSource::CommandLine;
    }
    return result;
}

ResolvedLaunch LoadLaunch(const LaunchOptions& options, const std::filesystem::path& executable_directory)
{
    const auto path = options.config.empty() ? DefaultConfigPath(executable_directory) : options.config;
    return ResolveLaunch(LoadConfig(path, !options.explicit_config), options);
}

const char* SettingSourceName(SettingSource source)
{
    switch (source)
    {
    case SettingSource::Defaults: return "defaults";
    case SettingSource::Config: return "INI";
    case SettingSource::Launcher: return "launcher";
    case SettingSource::CommandLine: return "command line";
    }
    return "unknown";
}

std::string DescribeLaunch(const ResolvedLaunch& launch)
{
    return "Disc: " + PathUtf8(launch.disc_path) + " (" + SettingSourceName(launch.disc_source)
        + "); window: " + std::to_string(launch.settings.width) + "x" + std::to_string(launch.settings.height)
        + " (" + SettingSourceName(launch.width_source) + "/" + SettingSourceName(launch.height_source)
        + "), " + (launch.settings.fullscreen ? "fullscreen" : "windowed")
        + " (" + SettingSourceName(launch.fullscreen_source) + "); aspect: "
        + launch.settings.aspect + " (" + SettingSourceName(launch.aspect_source) + ")";
}
}
