#include "bootstrap/config.h"
#include "platform/path.h"

#include <array>
#include <chrono>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace fs = std::filesystem;
using namespace mscharged;

void Require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

template <typename F> void Reject(F function)
{
    try { function(); }
    catch (const std::exception&) { return; }
    throw std::runtime_error("Expected configuration operation to fail");
}

int main()
{
    const auto directory = fs::temp_directory_path() / ("mscharged-config-"
        + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directory(directory);
    try
    {
        const auto path = directory / "local.ini";
        std::ofstream(path, std::ios::binary)
            << "\xEF\xBB\xBF; keep my comment\r\n[game]\r\ndisc = \"disc with spaces.rvz\"\r\n"
               "[future]\r\noption = keep me\r\n";
        auto file = LoadConfig(path);
        auto settings = file.settings;
        Require(ResolveDiscPath(settings, path) == directory / "disc with spaces.rvz", "Relative disc path");
        settings.width = 1920;
        settings.height = 1080;
        settings.fullscreen = true;
        settings.master_volume = 65;
        settings.players = {"remote2", "keyboard", "remote1", "off"};
        settings.deadzone = 20;
        settings.rumble = false;
        settings.backend = "d3d12";
        SaveConfig(file, settings);
        const auto reloaded = LoadConfig(path);
        Require(reloaded.settings.width == 1920 && reloaded.settings.height == 1080
            && reloaded.settings.fullscreen && reloaded.settings.master_volume == 65
            && reloaded.settings.players == settings.players && reloaded.settings.deadzone == 20
            && !reloaded.settings.rumble && reloaded.settings.backend == "d3d12", "Settings round trip");
        Require(reloaded.contents.find("; keep my comment\r\n") != std::string::npos
            && reloaded.contents.find("option = keep me\r\n") != std::string::npos
            && reloaded.contents.compare(0, 3, "\xEF\xBB\xBF") == 0, "Preserve comments, unknown keys, BOM and CRLF");
        const auto saved = file.contents;
        SaveConfig(file, settings);
        Require(file.contents == saved, "Repeated saves must be stable");
        settings.master_volume = 101;
        Reject([&] { SaveConfig(file, settings); });
        Require(LoadConfig(path).contents == saved, "Invalid settings must not overwrite the file");
        settings.master_volume = 65;
        std::ofstream(path, std::ios::app) << "; edited elsewhere\r\n";
        Reject([&] { SaveConfig(file, settings); });
        Require(LoadConfig(path).contents.find("edited elsewhere") != std::string::npos, "External edits survive");
        const auto missing = directory / "new.ini";
        auto fresh = LoadConfig(missing, true);
        Require(!fs::exists(missing), "Opening defaults must not create a file");
        SaveConfig(fresh, Settings{});
        Require(LoadConfig(missing).settings.width == 1280, "Create configuration on explicit save");
        std::ofstream(path) << "[display]\nwidth = 1920junk\n";
        Reject([&] { LoadConfig(path); });
        std::ofstream(path) << "[game]\ndisc = a.iso\ndisc = b.iso\n";
        Reject([&] { LoadConfig(path); });
        std::ofstream(path) << "[display]\nvsync = maybe\n";
        Reject([&] { LoadConfig(path); });
        // Players fill in order from player 1 and each device plays once;
        // an old controls.input line is ignored.
        Require(Settings{}.players == std::array<std::string, 4>{"keyboard", "off", "off", "off"},
                "Keyboard & mouse alone is the default player");
        std::ofstream(path) << "[controls]\ninput = controller\nplayer1 = remote1\nplayer2 = keyboard\n";
        Require(LoadConfig(path).settings.players[1] == "keyboard", "Ordered players load");
        std::ofstream(path) << "[controls]\nplayer1 = off\n";
        Reject([&] { LoadConfig(path); });
        std::ofstream(path) << "[controls]\nplayer1 = keyboard\nplayer3 = remote1\n";
        Reject([&] { LoadConfig(path); });
        std::ofstream(path) << "[controls]\nplayer1 = remote1\nplayer2 = remote1\n";
        Reject([&] { LoadConfig(path); });
        std::ofstream(path) << "[controls]\nplayer2 = gamepad\n";
        Reject([&] { LoadConfig(path); });
        // Gamepads are players too.
        std::ofstream(path) << "[controls]\nplayer1 = gamepad1\nplayer2 = keyboard\n";
        Require(LoadConfig(path).settings.players[0] == "gamepad1", "Gamepad player loads");
        std::ofstream(path) << "[controls]\nplayer1 = gamepad1\nplayer2 = gamepad1\n";
        Reject([&] { LoadConfig(path); });
        Require(Settings{}.mouse_pointer, "Mouse pointer default");
        std::ofstream(path) << "[controls]\nmouse_pointer = false\n";
        Require(!LoadConfig(path).settings.mouse_pointer, "Mouse pointer off loads");
        // [keyboard]: one or two key names per action.
        Require(Settings{}.keys[mscharged::KeyActionA] == "Return | Space", "Default keys");
        std::ofstream(path) << "[keyboard]\na = F1\nb = Escape | Left Shift\n";
        Require(LoadConfig(path).settings.keys[mscharged::KeyActionB] == "Escape | Left Shift", "Custom keys load");
        std::ofstream(path) << "[keyboard]\na = F1 | F2 | F3\n";
        Reject([&] { LoadConfig(path); });
        std::ofstream(path) << "[keyboard]\na = F1 |\n";
        Reject([&] { LoadConfig(path); });
        // Gamepad profiles: the built-in Default plus [gamepad_profileN] with a unique name.
        Require(Settings{}.pad_profiles.empty() && Settings{}.PadProfileIndex(2) == -1
                    && Settings{}.PadProfileOf(2).name == "Default"
                    && Settings{}.PadProfileOf(2).inputs[mscharged::GamepadActionA] == "a",
                "Gamepads play with the built-in Default profile");
        std::ofstream(path) << "; my pads\n[controls]\ngamepad2_profile = kyle\ngamepad3_profile = Gone\n"
                               "[gamepad_profile1]\n; Kyle's layout\nname = Kyle\na = b | start\nswap_sticks = true\n"
                               "[gamepad_profile2]\nname = Spare\nb = x\ncontroller = gamecube\n";
        {
            auto file = LoadConfig(path);
            const auto& loaded = file.settings;
            Require(loaded.pad_profiles.size() == 2 && loaded.pad_profiles[0].name == "Kyle"
                        && loaded.pad_profiles[0].inputs[mscharged::GamepadActionA] == "b | start"
                        && loaded.pad_profiles[0].inputs[mscharged::GamepadActionB] == "b"
                        && loaded.pad_profiles[0].swap_sticks && loaded.pad_profiles[0].controller == "xbox"
                        && loaded.pad_profiles[1].controller == "gamecube",
                    "Gamepad profiles load, with their controller kind");
            Require(loaded.PadProfileIndex(1) == 0 && loaded.pad_profile_names[1] == "Kyle",
                    "A gamepad finds its profile by name, ignoring case");
            Require(loaded.PadProfileIndex(2) == -1 && loaded.pad_profile_names[2] == "Default",
                    "A gamepad whose profile is gone plays with Default");
            // Delete Kyle: Spare moves up and the old second section must not come back.
            auto changed = loaded;
            changed.pad_profiles.erase(changed.pad_profiles.begin());
            changed.pad_profile_names[1] = "Default";
            changed.pad_profile_names[3] = "Spare";
            SaveConfig(file, changed);
            const auto saved = LoadConfig(path);
            Require(saved.settings.pad_profiles.size() == 1 && saved.settings.pad_profiles[0].name == "Spare"
                        && saved.settings.pad_profiles[0].inputs[mscharged::GamepadActionB] == "x"
                        && !saved.settings.pad_profiles[0].swap_sticks
                        && saved.settings.pad_profiles[0].controller == "gamecube"
                        && saved.settings.PadProfileIndex(3) == 0
                        && saved.settings.PadProfileIndex(1) == -1,
                    "A deleted gamepad profile stays deleted");
            Require(saved.contents.find("[gamepad_profile2]") == std::string::npos
                        && saved.contents.find("name = Kyle") == std::string::npos
                        && saved.contents.find("; my pads") != std::string::npos,
                    "Deleting a profile removes only its section");
            auto again = saved;
            SaveConfig(again, saved.settings);
            Require(again.contents == saved.contents, "Saving gamepad profiles again changes nothing");
            auto none = saved.settings;
            none.pad_profiles.clear();
            none.pad_profile_names[3] = "Default";
            SaveConfig(again, none);
            Require(LoadConfig(path).settings.pad_profiles.empty()
                        && again.contents.find("[gamepad_profile") == std::string::npos,
                    "Deleting every profile leaves the built-in Default");
        }
        std::ofstream(path) << "[gamepad_profile1]\na = a\n";
        Reject([&] { LoadConfig(path); });
        std::ofstream(path) << "[gamepad_profile1]\nname = A\n[gamepad_profile2]\nname = a\n";
        Reject([&] { LoadConfig(path); });
        std::ofstream(path) << "[gamepad_profile1]\nname = default\n";
        Reject([&] { LoadConfig(path); });
        std::ofstream(path) << "[gamepad_profile1]\nname = Pad\ncontroller = playstation\n";
        Reject([&] { LoadConfig(path); });
        std::ofstream(path) << "[gamepad_profile17]\nname = Many\n";
        Reject([&] { LoadConfig(path); });
        std::ofstream(path) << "[gamepad_profile1]\nname = Pad\na = a | b | x\n";
        Reject([&] { LoadConfig(path); });
        Require(mscharged::ValidPadProfileName("Kyle's pad") && !mscharged::ValidPadProfileName(" Kyle")
                    && !mscharged::ValidPadProfileName("a|b") && !mscharged::ValidPadProfileName("")
                    && !mscharged::ValidPadProfileName("DEFAULT"),
                "Profile name rules");
        // Wii Remotes paired over Bluetooth play by their address.
        {
            std::ofstream(path) << "[controls]\nplayer1 = remote-9ce635c69076\nplayer2 = remote1\n";
            auto bluetooth = LoadConfig(path);
            Require(bluetooth.settings.players[0] == "remote-9ce635c69076" && bluetooth.settings.players[1] == "remote1",
                    "A Bluetooth remote loads");
            auto changed = bluetooth.settings;
            changed.players[2] = "remote-001122334455";
            SaveConfig(bluetooth, changed);
            Require(LoadConfig(path).settings.players == changed.players, "Bluetooth remotes round trip");
            Require(Settings{}.bluetooth_search && LoadConfig(path).settings.bluetooth_search, "Bluetooth search is on");
            auto off = LoadConfig(path);
            auto searchless = off.settings;
            searchless.bluetooth_search = false;
            SaveConfig(off, searchless);
            Require(!LoadConfig(path).settings.bluetooth_search, "Bluetooth search off round trips");
            std::ofstream(path) << "[controls]\nplayer1 = remote-9CE635C69076\n";
            Reject([&] { LoadConfig(path); });
            std::ofstream(path) << "[controls]\nplayer1 = remote-9ce635c69076\nplayer2 = remote-9ce635c69076\n";
            Reject([&] { LoadConfig(path); });
        }
        // Pointer calibrations of earlier versions still load; they are kept
        // in the file untouched and no longer used.
        {
            const std::string old = "[controls]\nremote2_calibration = -0.0024 1e-05 1.68 0 -0.0026 1.58\n"
                                    "remote-9ce635c69076_calibration = 1 0 0 0 1 0\n";
            std::ofstream(path) << old;
            auto file = LoadConfig(path);
            auto changed = file.settings;
            changed.rumble = !changed.rumble;
            SaveConfig(file, changed);
            const auto saved = LoadConfig(path).contents;
            Require(saved.find("remote2_calibration = -0.0024 1e-05 1.68 0 -0.0026 1.58") != std::string::npos
                    && saved.find("remote-9ce635c69076_calibration = 1 0 0 0 1 0") != std::string::npos,
                    "An old calibration is kept in the file");
        }
        // Save folder: empty is the default next to the executable; a relative
        // folder counts from the settings file and is written in quotes.
        {
            const fs::path executable = directory / "bin";
            Require(Settings{}.save_folder.empty()
                    && ResolveSaveFolder(Settings{}, path, executable)
                        == executable / "original-main-credits-data" / "nand", "Default save folder");
            std::ofstream(path) << "[game]\nsave_folder = \"My Saves/charged\"\n";
            auto saves = LoadConfig(path);
            Require(saves.settings.save_folder == "My Saves/charged"
                    && ResolveSaveFolder(saves.settings, saves.path, executable)
                        == saves.path.parent_path() / "My Saves/charged", "A relative save folder loads");
            auto changed = saves.settings;
            changed.save_folder = mscharged::PathUtf8(directory / "absolute saves");
            SaveConfig(saves, changed);
            Require(LoadConfig(path).settings.save_folder == changed.save_folder
                    && ResolveSaveFolder(changed, path, executable) == directory / "absolute saves",
                    "An absolute save folder round trips");
        }

        // Host presentation/diagnostic settings: defaults keep the previous
        // behaviour, values round trip and out-of-range input is rejected.
        const Settings defaults;
        Require(!defaults.fps_overlay && defaults.fps_overlay_corner == "top-right"
            && defaults.monitor == 0 && !defaults.graphics_validation
            && defaults.log_level == "info" && !defaults.verbose_console && !defaults.frame_logging
            && defaults.ui_scale == "auto" && defaults.check_updates && defaults.update_channel == "auto",
            "Host setting defaults");
        const auto host = directory / "host.ini";
        auto hostFile = LoadConfig(host, true);
        Settings hostSettings;
        hostSettings.fps_overlay = true;
        hostSettings.fps_overlay_corner = "bottom-left";
        hostSettings.monitor = 2;
        hostSettings.graphics_validation = true;
        hostSettings.log_level = "warning";
        hostSettings.verbose_console = true;
        hostSettings.frame_logging = true;
        hostSettings.ui_scale = "150";
        hostSettings.check_updates = false;
        hostSettings.update_channel = "test";
        SaveConfig(hostFile, hostSettings);
        const auto hostReloaded = LoadConfig(host).settings;
        Require(hostReloaded.fps_overlay && hostReloaded.fps_overlay_corner == "bottom-left"
            && hostReloaded.monitor == 2 && hostReloaded.graphics_validation
            && hostReloaded.log_level == "warning" && hostReloaded.verbose_console && hostReloaded.frame_logging
            && hostReloaded.ui_scale == "150" && !hostReloaded.check_updates && hostReloaded.update_channel == "test",
            "Host settings round trip");
        const auto hostText = LoadConfig(host).contents;
        Require(hostText.find("[advanced]") != std::string::npos && hostText.find("[launcher]") != std::string::npos
            && hostText.find("show_fps") == std::string::npos
            && hostText.find("fps_overlay = true") != std::string::npos
            && hostText.find("fps_overlay_corner = bottom-left") != std::string::npos
            && hostText.find("frame_logging = true") != std::string::npos, "Host settings sections");
        hostSettings.frame_logging = false;
        SaveConfig(hostFile, hostSettings);
        Require(!LoadConfig(host).settings.frame_logging, "Frame logging can be disabled after enabling");
        // Old title-bar settings are accepted, ignored and retired on explicit save.
        std::ofstream(path) << "[display]\nshow_fps = true\nfps_overlay = false\ncustom = preserved\n";
        auto legacyFps = LoadConfig(path);
        Require(!legacyFps.settings.fps_overlay, "Old title FPS does not opt into the overlay");
        SaveConfig(legacyFps, legacyFps.settings);
        Require(legacyFps.contents.find("show_fps") == std::string::npos
            && legacyFps.contents.find("custom = preserved") != std::string::npos, "Retired title key is removed alone");
        for (const char* invalid : {"[display]\nmonitor = 16\n",
                                    "[display]\nfps_overlay = maybe\n", "[display]\nfps_overlay_corner = center\n",
                                    "[advanced]\nlog_level = verbose\n", "[launcher]\nui_scale = 110\n",
                                    "[advanced]\ngraphics_validation = 2\n", "[advanced]\nverbose_console = yes\n",
                                    "[advanced]\nframe_logging = maybe\n", "[launcher]\ncheck_updates = sometimes\n",
                                    "[launcher]\nupdate_channel = nightly\n"})
        {
            std::ofstream(path) << invalid;
            Reject([&] { LoadConfig(path); });
        }
        // [gameplay] tweaks: all off by default and without the section;
        // each round trips on its own and invalid values are rejected.
        Require(defaults.GameTweakMask() == 0, "Tweaks are off by default");
        std::ofstream(path) << "[game]\ndisc = game.rvz\n";
        Require(LoadConfig(path).settings.GameTweakMask() == 0, "No [gameplay] section means no tweaks");
        for (std::size_t n = 0; n < kGameTweaks.size(); ++n)
        {
            const auto tweaks = directory / ("tweaks" + std::to_string(n) + ".ini");
            auto tweakFile = LoadConfig(tweaks, true);
            Settings tweakSettings;
            tweakSettings.tweaks[n] = true;
            SaveConfig(tweakFile, tweakSettings);
            const auto reloaded = LoadConfig(tweaks);
            Require(reloaded.settings.GameTweakMask() == GameTweakBit(kGameTweaks[n].tweak), "Tweak round trip");
            Require(reloaded.contents.find(std::string("[gameplay]")) != std::string::npos
                && reloaded.contents.find(std::string(kGameTweaks[n].key) + " = true") != std::string::npos,
                "Tweak saved in [gameplay]");
        }
        Require(DescribeGameTweaks(0) == "none"
            && DescribeGameTweaks(GameTweakBit(GameTweak::SkipIntro) | GameTweakBit(GameTweak::UnlockAll))
                == "Skip intro, Unlock everything", "Tweak descriptions");
        for (const char* invalid : {"[gameplay]\nskip_intro = yes\n", "[gameplay]\nfast_menus = 2\n"})
        {
            std::ofstream(path) << invalid;
            Reject([&] { LoadConfig(path); });
        }
        // display.frame_rate: the original by default; named rates and 30-1000
        // round trip, anything else is rejected.
        Require(defaults.frame_rate == "original", "Frame rate is the original by default");
        for (const char* rate : {"original", "display", "unlimited", "30", "144", "1000"})
        {
            const auto paced = directory / (std::string("rate-") + rate + ".ini");
            auto rateFile = LoadConfig(paced, true);
            Settings rateSettings;
            rateSettings.frame_rate = rate;
            SaveConfig(rateFile, rateSettings);
            const auto reloaded = LoadConfig(paced);
            Require(reloaded.settings.frame_rate == rate
                && reloaded.contents.find(std::string("frame_rate = ") + rate) != std::string::npos, "Frame rate round trip");
        }
        for (const char* invalid : {"[display]\nframe_rate = 29\n", "[display]\nframe_rate = 1001\n",
                                    "[display]\nframe_rate = uncapped\n", "[display]\nframe_rate = 60.0\n",
                                    "[display]\nframe_rate =\n"})
        {
            std::ofstream(path) << invalid;
            Reject([&] { LoadConfig(path); });
        }
        fs::remove_all(directory);
        std::cout << "Configuration round trips, preservation, validation, host settings, tweaks and write protection passed.\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        fs::remove_all(directory);
        std::cerr << error.what() << '\n';
        return 1;
    }
}
