#include "bootstrap/launch_options.h"
#include "platform/path.h"
#include <chrono>
#include <fstream>
#include <iostream>
#include <vector>

using namespace mscharged;
namespace fs = std::filesystem;
unsigned checks;
void Check(bool value, const char* message)
{
    ++checks;
    if (!value) throw std::runtime_error(message);
}
template <class F> void Reject(F function)
{
    try { function(); } catch (const std::exception&) { ++checks; return; }
    throw std::runtime_error("Invalid launch option accepted");
}
LaunchOptions Parse(std::initializer_list<const char*> arguments)
{
    std::vector<const char*> argv(arguments);
    LaunchOptions result;
    for (int i = 1; i < int(argv.size()); ++i)
        Check(ParseLaunchOption(int(argv.size()), argv.data(), i, result), "Known launch option not consumed");
    return result;
}
std::string Read(const fs::path& path)
{
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

int main()
{
    const auto root = fs::temp_directory_path() / ("mscharged-launch-options-"
        + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(root / "config"); fs::create_directories(root / "working");
    const auto path = root / "config/settings.ini";
    const std::string original = "\xEF\xBB\xBF; untouched\r\n[game]\r\ndisc = \"games/INI disc.rvz\"\r\n"
        "language = french\r\n[display]\r\nwidth = 960\r\nheight = 640\r\nfullscreen = true\r\naspect = 4:3\r\n"
        "[future]\r\nunknown = retained\r\n";
    std::ofstream(path, std::ios::binary) << original;
    const auto stamp = fs::last_write_time(path);
    try
    {
        auto file = LoadConfig(path);
        const auto baseline = ResolveLaunch(file, {});
        Check(baseline.disc_path == root / "config/games/INI disc.rvz", "INI-relative disc base changed");
        Check(baseline.settings.language == "french", "INI text language lost before native handoff");
        Check(baseline.settings.width == 960 && baseline.settings.height == 640 && baseline.settings.fullscreen,
            "INI display values not retained");
        Check(baseline.disc_source == SettingSource::Config && baseline.width_source == SettingSource::Config
            && baseline.height_source == SettingSource::Config && baseline.fullscreen_source == SettingSource::Config,
            "INI field provenance lost");
        Check(baseline.settings.aspect == "4:3" && baseline.aspect_source == SettingSource::Config,
            "Original system aspect INI provenance lost");
        const auto parsed = Parse({"test", "--config", "settings.ini", "--disk", "CLI disc.rvz", "--window", "--size", "600x900", "--aspect", "16:9"});
        const auto resolved = ResolveLaunch(file, parsed, nullptr, root / "working");
        Check(resolved.disc_path == root / "working/CLI disc.rvz", "CLI-relative disc did not use cwd");
        Check(resolved.settings.width == 600 && resolved.settings.height == 900 && !resolved.settings.fullscreen,
            "CLI did not override INI display fields");
        Check(resolved.settings.aspect == "16:9" && resolved.aspect_source == SettingSource::CommandLine
            && resolved.config.settings.aspect == "4:3", "Run-only aspect override altered saved settings");
        Check(parsed.explicit_config && parsed.config == "settings.ini", "Explicit configuration source lost");
        const auto inline_options = Parse({"test", "--config=settings.ini", "--disc=CLI disc.rvz",
            "--window", "--size=600x900", "--aspect=16:9"});
        const auto inline_resolved = ResolveLaunch(file, inline_options, nullptr, root / "working");
        Check(inline_options.explicit_config && inline_options.config == parsed.config
            && inline_resolved.disc_path == resolved.disc_path
            && inline_resolved.settings.width == resolved.settings.width
            && inline_resolved.settings.height == resolved.settings.height
            && inline_resolved.settings.fullscreen == resolved.settings.fullscreen
            && inline_resolved.settings.aspect == resolved.settings.aspect,
            "Inline launch values differ from separate arguments");
        Check(resolved.disc_source == SettingSource::CommandLine && resolved.width_source == SettingSource::CommandLine
            && resolved.height_source == SettingSource::CommandLine && resolved.fullscreen_source == SettingSource::CommandLine,
            "CLI field provenance lost");
        Check(file.settings.disc == "games/INI disc.rvz" && file.settings.width == 960 && file.settings.fullscreen
            && resolved.config.contents == original && resolved.config.settings.disc == file.settings.disc,
            "Overrides modified loaded or persistable INI settings");
        const auto last = Parse({"test", "--fullscreen", "--window", "--fullscreen", "--disc", "old.iso", "--disk", "new.rvz"});
        Check(*last.fullscreen && *last.disc == "new.rvz", "Later display/disc options did not win");
        Check(*Parse({"test", "--disc=old.iso", "--disk", "new.rvz", "--disk=disc=final.rvz"}).disc
            == "disc=final.rvz", "Inline alias precedence or equals in a path changed");
        const auto last_aspect = Parse({"test", "--aspect", "16:9", "--aspect", "4:3"});
        Check(*last_aspect.aspect == "4:3", "Later system aspect did not win");
        Check(*Parse({"test", "--aspect", "auto"}).aspect == "auto", "Automatic initial aspect rejected");
        Reject([&] { Parse({"test", "--aspect", "21:9"}); });
        Reject([&] { Parse({"test", "--aspect"}); });
        LaunchOptions invalid_aspect; invalid_aspect.aspect = "invalid";
        Reject([&] { ResolveLaunch(file, invalid_aspect); });
        const auto limits = Parse({"test", "--size", "1x16384"});
        Check(limits.size->width == 1 && limits.size->height == 16384, "Valid boundary dimensions rejected");
        LaunchOptions invalid; invalid.size = WindowSize{0, 600};
        Reject([&] { ResolveLaunch(file, invalid); });
        for (const char* bad : {"0x600", "800x0", "16385x10", "10x16385", "-1x10", "+1x10", "800X600", "800x600junk", "800x600x700"})
            Reject([&] { Parse({"test", "--size", bad}); });
        Reject([&] { Parse({"test", "--disc"}); });
        for (const char* bad : {"--disc=", "--disk=", "--config=", "--size=", "--aspect=",
                "--size=800x600junk", "--aspect=21:9", "--window=true"})
            Reject([&] { Parse({"test", bad}); });
        Reject([&] { Parse({"test", "--config", ""}); });
        Reject([&] { Parse({"test", "--size", "--fullscreen"}); });
        const char* unknown[] = {"test", "--experimental-startup"}; int index = 1;
        LaunchOptions untouched;
        Check(!ParseLaunchOption(2, unknown, index, untouched) && index == 1 && !untouched.disc,
            "Shared parsing consumed an explicit runtime mode");
        const char* mixed[] = {"test", "--disc=before.rvz", "--experimental-frontend", "--window",
            "--disk", "after.rvz", "--size=1280x720"};
        LaunchOptions mixed_options;
        unsigned modes = 0;
        for (int i = 1; i < 7; ++i)
        {
            if (ParseLaunchOption(7, mixed, i, mixed_options)) continue;
            Check(std::string_view(mixed[i]) == "--experimental-frontend", "Unknown shared option in mixed order");
            ++modes;
        }
        Check(modes == 1 && *mixed_options.disc == "after.rvz" && !*mixed_options.fullscreen
            && mixed_options.size->width == 1280 && mixed_options.size->height == 720,
            "Shared arguments depend on runtime-mode ordering");
        const char* unknown_inline[] = {"test", "--unknown=value"}; index = 1;
        Check(!ParseLaunchOption(2, unknown_inline, index, untouched) && index == 1 && !untouched.disc,
            "Shared parsing consumed an unknown inline option");
        Settings draft = file.settings; draft.disc = "launcher.rvz"; draft.width = 1111; draft.aspect = "auto"; draft.language = "spanish";
        const auto edited = ResolveLaunch(file, {}, &draft);
        Check(edited.disc_source == SettingSource::Launcher && edited.width_source == SettingSource::Launcher
            && edited.height_source == SettingSource::Config && edited.disc_path == root / "config/launcher.rvz",
            "Launcher draft precedence or provenance lost");
        Check(edited.settings.language == "spanish" && edited.config.settings.language == "french",
            "Launcher text language must reach handoff without rewriting stored INI settings");
        Check(edited.settings.aspect == "auto" && edited.aspect_source == SettingSource::Launcher,
            "Launcher aspect draft precedence lost");
        const auto override_draft = ResolveLaunch(file, parsed, &draft, root / "working");
        Check(override_draft.settings.language == "spanish", "Unrelated CLI options lost launcher text language");
        Check(override_draft.settings.width == 600 && override_draft.disc_path == resolved.disc_path,
            "Launcher edits overrode explicit CLI options");
        Check(override_draft.settings.aspect == "16:9" && override_draft.aspect_source == SettingSource::CommandLine,
            "Launcher draft overrode explicit system aspect");
        auto missing = LoadConfig(root / "missing.ini", true);
        const auto defaults = ResolveLaunch(missing, {});
        Check(defaults.width_source == SettingSource::Defaults && defaults.disc_path.empty(), "Missing INI defaults misclassified");
        LaunchOptions direct; direct.config = root / "absent.ini"; direct.disc = root / "direct.iso";
        const auto fresh = LoadLaunch(direct, root);
        Check(fresh.disc_path == root / "direct.iso" && !fresh.config.exists, "Direct disc required an implicit INI");
        direct.explicit_config = true;
        Reject([&] { LoadLaunch(direct, root); });
        Check(!fs::exists(root / "missing.ini") && !fs::exists(root / "absent.ini"), "Resolving defaults created an INI");
        const auto absolute_path = PathUtf8(root / "absolute.rvz");
        const auto absolute = ResolveLaunch(file, Parse({"test", "--disc", absolute_path.c_str()}), nullptr, root / "working");
        Check(absolute.disc_path == root / "absolute.rvz", "Absolute CLI path changed");
        Check(Read(path) == original && fs::last_write_time(path) == stamp, "Run overrides wrote the INI");
        Check(DescribeLaunch(resolved).find("command line") != std::string::npos, "Resolved setting source not described");
        std::ofstream(path) << "[display]\nwidth = 600\nheight = 900\n";
        const auto tall = LoadConfig(path);
        Check(tall.settings.width == 600 && tall.settings.height == 900,
            "INI could not represent a qualified tall native output");
        fs::remove_all(root);
        std::cout << checks << " launch parsing, precedence, provenance, path and no-write checks passed.\n";
        return 0;
    }
    catch (const std::exception& e)
    {
        fs::remove_all(root); std::cerr << e.what() << '\n'; return 1;
    }
}
