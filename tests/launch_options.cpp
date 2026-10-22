#include "bootstrap/antialiasing.h"
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
        "language = french\r\n[display]\r\nbackend = d3d12\r\nwidth = 960\r\nheight = 640\r\nfullscreen = true\r\naspect = 4:3\r\n"
        "[future]\r\nunknown = retained\r\n";
    std::ofstream(path, std::ios::binary) << original;
    const auto stamp = fs::last_write_time(path);
    try
    {
        auto file = LoadConfig(path);
        const auto baseline = ResolveLaunch(file, {});
        Check(baseline.disc_path == root / "config/games/INI disc.rvz", "INI-relative disc base changed");
        Check(baseline.settings.language == "french", "INI text language lost before native handoff");
        Check(baseline.settings.backend == "d3d12", "INI graphics backend lost before native handoff");
        auto graphics_draft = file.settings;
        graphics_draft.backend = "vulkan";
        Check(ResolveLaunch(file, {}, &graphics_draft).settings.backend == "vulkan"
            && file.settings.backend == "d3d12", "Launcher backend choice did not override the INI for launch");
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
        Check(*Parse({"test", "--aspect=stretch"}).aspect == "stretch", "Stretched picture rejected");
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
        // --no-tweaks: the INI's [gameplay] tweaks are off for this run only.
        Check(Parse({"test", "--no-tweaks"}).no_tweaks && !Parse({"test", "--window"}).no_tweaks,
            "--no-tweaks not parsed");
        Reject([&] { Parse({"test", "--no-tweaks=true"}); });
        {
            const auto tweaked = root / "config/tweaked.ini";
            std::ofstream(tweaked, std::ios::binary) << "[gameplay]\nskip_intro = true\nunlock_all = true\n";
            const auto tweakedFile = LoadConfig(tweaked);
            const auto played = ResolveLaunch(tweakedFile, {});
            // The INI's tweaks, plus the recommended one it leaves at its default.
            Check(played.settings.GameTweakMask()
                    == (GameTweakBit(GameTweak::SkipIntro) | GameTweakBit(GameTweak::UnlockAll)
                        | GameTweakBit(GameTweak::FairBallContests))
                && played.tweaks_source == SettingSource::Config, "INI tweaks not resolved");
            LaunchOptions retail; retail.no_tweaks = true;
            const auto original = ResolveLaunch(tweakedFile, retail);
            Check(original.settings.GameTweakMask() == 0 && original.tweaks_source == SettingSource::CommandLine
                && DescribeLaunch(original).find("tweaks: none (command line)") != std::string::npos,
                "--no-tweaks did not turn the tweaks off");
            Check(tweakedFile.settings.GameTweakMask() != 0 && Read(tweaked).find("skip_intro = true") != std::string::npos,
                "--no-tweaks changed the INI");
        }
        // --frame-rate: this run's display.frame_rate, apart from the tweaks.
        for (const char* value : {"original", "display", "unlimited", "30", "120", "144", "1000"})
            Check(*Parse({"test", "--frame-rate", value}).frame_rate == value, "--frame-rate value not parsed");
        Check(*Parse({"test", "--frame-rate=165"}).frame_rate == "165", "--frame-rate=VALUE not parsed");
        for (const char* bad : {"--frame-rate=", "--frame-rate=29", "--frame-rate=1001", "--frame-rate=0120",
                "--frame-rate=60.5", "--frame-rate=Display", "--frame-rate=-120", "--frame-rate=uncapped"})
            Reject([&] { Parse({"test", bad}); });
        {
            const auto paced = root / "config/paced.ini";
            std::ofstream(paced, std::ios::binary) << "[display]\nframe_rate = 144\nresolution = 2x\n"
                "antialiasing = 4x\nperformance_mode = false\n";
            const auto pacedFile = LoadConfig(paced);
            const auto played = ResolveLaunch(pacedFile, {});
            Check(played.settings.frame_rate == "144" && played.frame_rate_source == SettingSource::Config
                && DescribeLaunch(played).find("frame rate: 144 (INI)") != std::string::npos, "INI frame rate not resolved");
            Check(played.settings.resolution == "2x" && played.settings.antialiasing == "4x"
                && !played.settings.performance_mode, "Disabled performance mode changed display choices");
            auto performanceDraft = pacedFile.settings;
            performanceDraft.performance_mode = true;
            const auto performance = ResolveLaunch(pacedFile, {}, &performanceDraft);
            Check(performance.settings.performance_mode && performance.settings.resolution == "2x"
                && performance.settings.antialiasing == "off"
                && performanceDraft.resolution == "2x" && performanceDraft.antialiasing == "4x"
                && pacedFile.settings.resolution == "2x" && pacedFile.settings.antialiasing == "4x"
                && DescribeLaunch(performance).find("performance mode: on") != std::string::npos,
                "Performance mode did not apply a run-only lower-cost render profile");
            performanceDraft.performance_mode = false;
            const auto restored = ResolveLaunch(pacedFile, {}, &performanceDraft);
            Check(restored.settings.resolution == "2x" && restored.settings.antialiasing == "4x",
                "Turning performance mode off did not restore the saved display choices");
            LaunchOptions run; run.frame_rate = "display";
            const auto overridden = ResolveLaunch(pacedFile, run);
            Check(overridden.settings.frame_rate == "display" && overridden.frame_rate_source == SettingSource::CommandLine,
                "--frame-rate did not override the INI");
            LaunchOptions retail; retail.no_tweaks = true;
            Check(ResolveLaunch(pacedFile, retail).settings.frame_rate == "144", "--no-tweaks changed the frame rate");
            run.frame_rate = "original";
            Check(ResolveLaunch(pacedFile, run).settings.frame_rate == "original", "--frame-rate original ignored");
            Check(pacedFile.settings.frame_rate == "144" && Read(paced).find("frame_rate = 144") != std::string::npos,
                "--frame-rate changed the INI");
            Settings draft = pacedFile.settings; draft.frame_rate = "unlimited";
            const auto launcher = ResolveLaunch(pacedFile, {}, &draft);
            Check(launcher.settings.frame_rate == "unlimited" && launcher.frame_rate_source == SettingSource::Launcher,
                "Launcher frame rate draft lost");
            std::ofstream(paced, std::ios::binary) << "[display]\nframe_rate = 25\n";
            Reject([&] { LoadConfig(paced); });
            // display.antialiasing: every kind the launcher offers loads and
            // maps to its own render settings; performance mode turns it off.
            for (const auto& kind : kAntialiasingChoices)
            {
                std::ofstream(paced, std::ios::binary) << "[display]\nantialiasing = " << kind.value << "\n";
                auto loaded = LoadConfig(paced);
                Check(loaded.settings.antialiasing == kind.value
                    && &FindAntialiasing(ResolveLaunch(loaded, {}).settings.antialiasing) == &kind,
                    "Antialiasing kind not loaded");
                loaded.settings.performance_mode = true;
                Check(ResolveLaunch(loaded, {}, &loaded.settings).settings.antialiasing == "off",
                    "Performance mode kept antialiasing");
                Check((kind.samples == 1 || kind.samples == 4) && (!kind.sample_shading || kind.samples > 1),
                    "Antialiasing kind needs a sample count WebGPU offers");
            }
            Check(&FindAntialiasing("off") == &kAntialiasingChoices.front() && !FindAntialiasing("off").fxaa
                && FindAntialiasing("4x").samples == 4 && !FindAntialiasing("4x").fxaa
                && FindAntialiasing("4x_ssaa").sample_shading && FindAntialiasing("fxaa").samples == 1,
                "Antialiasing kinds changed meaning");
            for (const char* bad : {"8x", "2x", "on", "ssaa", "4X"})
            {
                std::ofstream(paced, std::ios::binary) << "[display]\nantialiasing = " << bad << "\n";
                Reject([&] { LoadConfig(paced); });
            }
        }
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
        auto game_mode = baseline;
        ApplyGameModeDisplay(game_mode, false);
        Check(game_mode.settings.fullscreen == baseline.settings.fullscreen
            && game_mode.fullscreen_source == baseline.fullscreen_source, "Desktop launch changed by Game Mode rule");
        game_mode.settings.fullscreen = false;
        ApplyGameModeDisplay(game_mode, true);
        Check(game_mode.settings.fullscreen && game_mode.fullscreen_source == SettingSource::GameMode
            && DescribeLaunch(game_mode).find("Steam Game Mode") != std::string::npos, "Game Mode did not open fullscreen");
        auto windowed = ResolveLaunch(file, Parse({"test", "--window"}));
        ApplyGameModeDisplay(windowed, true);
        Check(!windowed.settings.fullscreen && windowed.fullscreen_source == SettingSource::CommandLine,
            "Game Mode overrode an explicit --window");
        std::ofstream(path) << "[display]\nwidth = 600\nheight = 900\n";
        const auto tall = LoadConfig(path);
        Check(tall.settings.width == 600 && tall.settings.height == 900,
            "INI could not represent a qualified tall native output");
        // Settings from before the rename (#64) are copied to recharged.ini.
        const auto previous_directory = fs::current_path();
        fs::current_path(root / "working");
        fs::create_directories(root / "package/bin");
        std::ofstream(root / "package/recharged.ini.example") << "";
        Check(DefaultConfigPath(root / "package/bin") == root / "package/recharged.ini"
            && !fs::exists(root / "package/recharged.ini"), "Settings belong beside the example");
        std::ofstream(root / "package/mscharged.ini") << "[game]\ndisc = old.rvz\n";
        Check(DefaultConfigPath(root / "package/bin") == root / "package/recharged.ini"
            && Read(root / "package/recharged.ini") == "[game]\ndisc = old.rvz\n"
            && fs::exists(root / "package/mscharged.ini"), "Older settings not copied");
        std::ofstream(root / "package/mscharged.ini") << "[game]\ndisc = changed.rvz\n";
        Check(Read(DefaultConfigPath(root / "package/bin")) == "[game]\ndisc = old.rvz\n",
            "recharged.ini replaced by older settings");
        fs::current_path(previous_directory);
        fs::remove_all(root);
        std::cout << checks << " launch parsing, precedence, provenance, path and no-write checks passed.\n";
        return 0;
    }
    catch (const std::exception& e)
    {
        fs::remove_all(root); std::cerr << e.what() << '\n'; return 1;
    }
}
