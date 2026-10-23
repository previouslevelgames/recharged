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
        Require(settings.fullscreen_resolution == "desktop", "Fullscreen keeps the desktop resolution by default");
        settings.fullscreen_resolution = "1280x720";
        settings.master_volume = 65;
        settings.sound_volumes = {20, 80, 35, 50, 70, 0, 60};
        settings.players = {"remote2", "keyboard", "remote1", "off"};
        settings.rumble = false;
        settings.rumble_strength = 40;
        settings.ir_sensitivity = 5;
        settings.speaker_volume = 0;
        settings.backend = "d3d12";
        SaveConfig(file, settings);
        const auto reloaded = LoadConfig(path);
        Require(reloaded.settings.width == 1920 && reloaded.settings.height == 1080
            && reloaded.settings.fullscreen && reloaded.settings.fullscreen_resolution == "1280x720"
            && reloaded.settings.master_volume == 65
            && reloaded.settings.sound_volumes == settings.sound_volumes
            && reloaded.settings.players == settings.players
            && !reloaded.settings.rumble && reloaded.settings.rumble_strength == 40
            && reloaded.settings.ir_sensitivity == 5 && reloaded.settings.speaker_volume == 0
            && reloaded.settings.backend == "d3d12", "Settings round trip");
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
        Require(Settings{}.sound_volumes == DefaultSoundVolumes()
            && DescribeSoundVolumes(Settings{}.sound_volumes) == "original"
            && DescribeSoundVolumes(settings.sound_volumes).find("Music 20%") == 0, "Sound group volumes");
        const auto missing = directory / "new.ini";
        auto fresh = LoadConfig(missing, true);
        Require(!fs::exists(missing), "Opening defaults must not create a file");
        SaveConfig(fresh, Settings{});
        Require(LoadConfig(missing).settings.width == 1280, "Create configuration on explicit save");
        std::ofstream(path) << "[display]\nwidth = 1920junk\n";
        Reject([&] { LoadConfig(path); });
        std::ofstream(path) << "[game]\ndisc = a.iso\ndisc = b.iso\n";
        Reject([&] { LoadConfig(path); });
        std::ofstream(path) << "[audio]\nmusic_volume = 20\ncutscene_volume = 0\n";
        Require(LoadConfig(path).settings.sound_volumes == SoundVolumes{20, 100, 100, 100, 100, 0, 100},
                "Sound group keys");
        std::ofstream(path) << "[audio]\nmovie_volume = 101\n";
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
        // Wii Remote settings: the Wii's defaults (sensitivity 3, speaker 70 %).
        Require(Settings{}.ir_sensitivity == 3 && Settings{}.speaker_volume == 70 && Settings{}.rumble_strength == 100,
                "Wii Remote defaults");
        std::ofstream(path) << "[controls]\nir_sensitivity = 0\n";
        Reject([&] { LoadConfig(path); });
        std::ofstream(path) << "[controls]\nir_sensitivity = 6\n";
        Reject([&] { LoadConfig(path); });
        std::ofstream(path) << "[controls]\nspeaker_volume = 101\n";
        Reject([&] { LoadConfig(path); });
        std::ofstream(path) << "[controls]\nrumble_strength = -1\n";
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
                               "inner_deadzone = 22\nouter_deadzone = 90\ntrigger_press = 30\n"
                               "gyro_pointer = true\ngyro_speed = 150\nrecenter_pointer = leftstick\n"
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
            Require(loaded.pad_profiles[0].inner_deadzone == 22 && loaded.pad_profiles[0].outer_deadzone == 90
                        && loaded.pad_profiles[0].trigger_press == 30 && loaded.pad_profiles[1].inner_deadzone == 15
                        && loaded.pad_profiles[1].outer_deadzone == 71 && loaded.pad_profiles[1].trigger_press == 50
                        && Settings{}.PadProfileOf(0).outer_deadzone == 71,
                    "Gamepad profiles keep their dead zones; missing ones are the Wii Nunchuk's");
            Require(loaded.pad_profiles[0].gyro_pointer && loaded.pad_profiles[0].gyro_speed == 150
                        && loaded.pad_profiles[0].inputs[mscharged::GamepadActionRecenterPointer] == "leftstick"
                        && !loaded.pad_profiles[1].gyro_pointer && loaded.pad_profiles[1].gyro_speed == 100
                        && loaded.pad_profiles[1].inputs[mscharged::GamepadActionRecenterPointer] == "rightstick"
                        && !Settings{}.PadProfileOf(0).gyro_pointer,
                    "Gamepad profiles keep their gyro pointer; it is off unless asked for");
            Require(loaded.PadProfileIndex(1) == 0 && loaded.pad_profile_names[1] == "Kyle",
                    "A gamepad finds its profile by name, ignoring case");
            Require(loaded.PadProfileIndex(2) == -1 && loaded.pad_profile_names[2] == "Default",
                    "A gamepad whose profile is gone plays with Default");
            // Delete Kyle: Spare moves up and the old second section must not come back.
            auto changed = loaded;
            changed.pad_profiles.erase(changed.pad_profiles.begin());
            changed.pad_profile_names[1] = "Default";
            changed.pad_profile_names[3] = "Spare";
            changed.pad_profiles[0].inner_deadzone = 0;
            changed.pad_profiles[0].outer_deadzone = 100;
            changed.pad_profiles[0].trigger_press = 90;
            changed.pad_profiles[0].gyro_pointer = true;
            changed.pad_profiles[0].gyro_speed = 25;
            SaveConfig(file, changed);
            const auto saved = LoadConfig(path);
            Require(saved.settings.pad_profiles.size() == 1 && saved.settings.pad_profiles[0].name == "Spare"
                        && saved.settings.pad_profiles[0].inputs[mscharged::GamepadActionB] == "x"
                        && !saved.settings.pad_profiles[0].swap_sticks
                        && saved.settings.pad_profiles[0].controller == "gamecube"
                        && saved.settings.pad_profiles[0].inner_deadzone == 0
                        && saved.settings.pad_profiles[0].outer_deadzone == 100
                        && saved.settings.pad_profiles[0].trigger_press == 90
                        && saved.settings.pad_profiles[0].gyro_pointer
                        && saved.settings.pad_profiles[0].gyro_speed == 25
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
        for (const char* range : {"inner_deadzone = 31", "inner_deadzone = -1", "outer_deadzone = 69",
                                  "outer_deadzone = 101", "trigger_press = 9", "trigger_press = 91",
                                  "gyro_speed = 24", "gyro_speed = 301", "gyro_pointer = maybe"})
        {
            std::ofstream(path) << "[gamepad_profile1]\nname = Pad\n" << range << "\n";
            Reject([&] { LoadConfig(path); });
        }
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
                        == executable / "data" / "nand", "Default save folder");
            // Earlier builds' data folder stays in use until it has been moved.
            fs::create_directories(executable / "original-main-credits-data" / "nand");
            std::ofstream(executable / "original-main-credits-data" / "nand" / "save.bin") << "save";
            Require(DefaultSaveFolder(executable) == executable / "original-main-credits-data" / "nand",
                    "The old data folder is used before the move");
            Require(MoveOldDataFolder(executable) && DataFolder(executable) == executable / "data"
                        && fs::exists(executable / "data" / "nand" / "save.bin")
                        && !fs::exists(executable / "original-main-credits-data"),
                    "The old data folder moves to data");
            fs::create_directories(executable / "original-main-credits-data");
            Require(!MoveOldDataFolder(executable) && DataFolder(executable) == executable / "data",
                    "An existing data folder is kept");
            fs::remove_all(executable);
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
        // The Super Team's GameCube disc and robot side.
        {
            Require(Settings{}.gamecube_disc.empty() && ResolveGameCubeDiscPath(Settings{}, path).empty()
                    && Settings{}.super_team_side == "away", "Super Team defaults");
            std::ofstream(path) << "[game]\ngamecube_disc = \"game/Strikers.rvz\"\n[gameplay]\nsuper_team = true\n"
                                   "super_team_side = both\n";
            auto loaded = LoadConfig(path);
            Require(loaded.settings.gamecube_disc == "game/Strikers.rvz"
                    && ResolveGameCubeDiscPath(loaded.settings, path) == path.parent_path() / "game/Strikers.rvz"
                    && loaded.settings.super_team_side == "both"
                    && loaded.settings.tweaks[mscharged::GameTweakIndex(mscharged::GameTweak::SuperTeam)],
                    "The Super Team settings load");
            auto changed = loaded.settings;
            changed.super_team_side = "home";
            changed.gamecube_disc = "other.iso";
            SaveConfig(loaded, changed);
            const auto saved = LoadConfig(path);
            Require(saved.settings.super_team_side == "home" && saved.settings.gamecube_disc == "other.iso"
                    && saved.contents.find("gamecube_disc = \"other.iso\"") != std::string::npos,
                    "The Super Team settings round trip");
            std::ofstream(path) << "[gameplay]\nsuper_team_side = left\n";
            bool rejected = false;
            try { LoadConfig(path); } catch (const std::runtime_error&) { rejected = true; }
            Require(rejected, "An unknown Super Team side is rejected");
        }
        // The game folder (#65): game beside the settings file by default, in
        // quotes, with its subfolders; the first builds' [library] list moves to it.
        {
            Require(Settings{}.folder == "game" && Settings{}.folder_subfolders, "Default game folder");
            Require(ResolveGameFolder(Settings{}, path) == path.parent_path() / "game", "The game folder is relative");
            const std::string sd = mscharged::PathUtf8(directory / "sd card" / "Wii");
            std::ofstream(path) << "[game]\ndisc = \"game/R4QE01.rvz\"\nfolder = \"" << sd << "\"\n"
                                   "folder_subfolders = false\n";
            auto loaded = LoadConfig(path);
            Require(loaded.settings.folder == sd && !loaded.settings.folder_subfolders
                        && ResolveGameFolder(loaded.settings, path) == directory / "sd card" / "Wii",
                    "An absolute game folder loads");
            auto changed = loaded.settings;
            changed.folder = "Wii games";
            changed.folder_subfolders = true;
            SaveConfig(loaded, changed);
            Require(loaded.contents.find("folder = \"Wii games\"") != std::string::npos
                        && loaded.contents.find("folder_subfolders = true") != std::string::npos
                        && LoadConfig(path).settings.folder == "Wii games",
                    "The game folder round trips in quotes");
            std::ofstream(path) << "[game]\ndisc = \"x.rvz\"\n\n[library]\nfolder1 = \"" << sd
                                << "\"\nfolder1_subfolders = false\nfolder2 = \"other\"\n";
            auto old = LoadConfig(path);
            Require(old.settings.folder == sd && !old.settings.folder_subfolders, "The first library folder is taken");
            SaveConfig(old, old.settings);
            Require(old.contents.find("[library]") == std::string::npos && old.contents.find("folder2") == std::string::npos
                        && old.contents.find("folder = \"" + sd + "\"") != std::string::npos,
                    "Saving replaces the old folder list");
            std::ofstream(path) << "[game]\nfolder = \"\"\n";
            Require(LoadConfig(path).settings.folder == "game", "An empty game folder is the default");
            std::ofstream(path) << "[game]\nfolder = \"x\"\nfolder_subfolders = maybe\n";
            Reject([&] { LoadConfig(path); });
        }

        // Host presentation/diagnostic settings: defaults keep the previous
        // behaviour, values round trip and out-of-range input is rejected.
        const Settings defaults;
        Require(!defaults.fps_overlay && defaults.fps_overlay_corner == "top-right"
            && defaults.monitor == 0 && !defaults.graphics_validation
            && defaults.log_level == "info" && !defaults.verbose_console && !defaults.frame_logging && !defaults.log_window
            && defaults.ui_scale == "auto" && defaults.check_updates && defaults.update_channel == "auto"
            && defaults.test_build == 0 && !defaults.reopen_after_game && defaults.gpu == "auto"
            && !defaults.performance_mode,
            "Host setting defaults");
        const auto host = directory / "host.ini";
        auto hostFile = LoadConfig(host, true);
        Settings hostSettings;
        hostSettings.fps_overlay = true;
        hostSettings.fps_overlay_corner = "bottom-left";
        hostSettings.performance_mode = true;
        hostSettings.monitor = 2;
        hostSettings.graphics_validation = true;
        hostSettings.log_level = "warning";
        hostSettings.verbose_console = true;
        hostSettings.frame_logging = true;
        hostSettings.log_window = true;
        hostSettings.ui_scale = "150";
        hostSettings.check_updates = false;
        hostSettings.update_channel = "test";
        hostSettings.test_build = 72;
        hostSettings.reopen_after_game = true;
        hostSettings.gpu = "10de:1f99";
        SaveConfig(hostFile, hostSettings);
        const auto hostReloaded = LoadConfig(host).settings;
        Require(hostReloaded.fps_overlay && hostReloaded.fps_overlay_corner == "bottom-left"
            && hostReloaded.performance_mode
            && hostReloaded.monitor == 2 && hostReloaded.graphics_validation
            && hostReloaded.log_level == "warning" && hostReloaded.verbose_console && hostReloaded.frame_logging
            && hostReloaded.log_window
            && hostReloaded.ui_scale == "150" && !hostReloaded.check_updates && hostReloaded.update_channel == "test"
                && hostReloaded.test_build == 72 && hostReloaded.reopen_after_game && hostReloaded.gpu == "10de:1f99",
            "Host settings round trip");
        const auto hostText = LoadConfig(host).contents;
        Require(hostText.find("[advanced]") != std::string::npos && hostText.find("[launcher]") != std::string::npos
            && hostText.find("show_fps") == std::string::npos
            && hostText.find("fps_overlay = true") != std::string::npos
            && hostText.find("performance_mode = true") != std::string::npos
            && hostText.find("fps_overlay_corner = bottom-left") != std::string::npos
            && hostText.find("frame_logging = true") != std::string::npos
            && hostText.find("log_window = true") != std::string::npos, "Host settings sections");
        hostSettings.frame_logging = false;
        SaveConfig(hostFile, hostSettings);
        Require(!LoadConfig(host).settings.frame_logging, "Frame logging can be disabled after enabling");
        // [display] picture shape, TV border and the remembered window (#91).
        {
            Require(defaults.aspect == "16:9" && !defaults.tv_border && defaults.remember_window
                    && defaults.position.empty(), "Display defaults");
            std::ofstream(path) << "[display]\naspect = stretch\ntv_border = true\nremember_window = false\n"
                                   "position = -1280, 40\n";
            auto display = LoadConfig(path);
            Require(display.settings.aspect == "stretch" && display.settings.tv_border
                    && !display.settings.remember_window && display.settings.position == "-1280, 40"
                    && ParseWindowPosition(display.settings.position) == std::array<int, 2>{-1280, 40},
                    "Display settings read");
            auto moved = display.settings;
            moved.position = FormatWindowPosition(100, -20);
            moved.width = 1500;
            moved.height = 844;
            SaveConfig(display, moved);
            const auto remembered = LoadConfig(path);
            Require(remembered.settings.position == "100,-20" && remembered.settings.width == 1500
                    && remembered.settings.height == 844 && remembered.settings.tv_border
                    && remembered.contents.find("position = 100,-20") != std::string::npos
                    && remembered.contents.find("aspect = stretch") != std::string::npos,
                    "The window's size and position did not round trip");
            for (const char* invalid : {"[display]\nposition = 10\n", "[display]\nposition = a,b\n",
                                        "[display]\nposition = 1,2,3\n", "[display]\nposition = 40000,0\n",
                                        "[display]\ntv_border = maybe\n", "[display]\nremember_window = 2\n",
                                        "[display]\naspect = wide\n"})
            {
                std::ofstream(path) << invalid;
                Reject([&] { LoadConfig(path); });
            }
            std::ofstream(path) << "[display]\nposition =\n";
            Require(LoadConfig(path).settings.position.empty(), "An empty position does not centre the window");
        }
        // [discord]: the status is on by default and round trips; an
        // application ID in the INI (testing) is read but never written.
        Require(defaults.discord_status && defaults.discord_application_id.empty(), "Discord defaults");
        std::ofstream(path) << "[discord]\nrich_presence = false\napplication_id = 123456789012345678\n";
        auto discordFile = LoadConfig(path);
        Require(!discordFile.settings.discord_status
                && discordFile.settings.discord_application_id == "123456789012345678", "Discord settings read");
        auto discordSettings = discordFile.settings;
        discordSettings.discord_status = true;
        discordSettings.discord_application_id.clear();
        SaveConfig(discordFile, discordSettings);
        const auto discordReloaded = LoadConfig(path);
        Require(discordReloaded.settings.discord_status
                && discordReloaded.contents.find("rich_presence = true") != std::string::npos
                && discordReloaded.settings.discord_application_id == "123456789012345678",
                "Saving changed or dropped the Discord application ID");
        hostFile = LoadConfig(host);
        Require(hostFile.contents.find("application_id") == std::string::npos
                && hostFile.contents.find("[discord]") != std::string::npos, "Saving wrote a Discord application ID");
        // [online] rollback: off, no log by default; read, never written. The
        // starting input delay is no setting: an old rollback_delay is ignored.
        Require(!defaults.rollback && !defaults.rollback_test && defaults.rollback_log.empty(), "Rollback defaults");
        std::ofstream(path) << "[online]\nrollback = true\nrollback_test = true\nrollback_delay = 9\nrollback_log = rb.txt\n";
        auto rollbackFile = LoadConfig(path);
        Require(rollbackFile.settings.rollback && rollbackFile.settings.rollback_test
                && rollbackFile.settings.rollback_log == "rb.txt", "Rollback settings read");
        SaveConfig(rollbackFile, rollbackFile.settings);
        Require(LoadConfig(path).contents.find("rollback_log = rb.txt") != std::string::npos,
                "Saving dropped the rollback settings");
        Require(LoadConfig(host).contents.find("rollback") == std::string::npos, "Saving wrote rollback settings");
        // [mods]: texture packs on, their folder beside the settings file, no
        // order or switched-off packs and no texture dumps by default; each
        // round trips, and the folder is written in quotes.
        Require(defaults.texture_packs && defaults.texture_folder == "texture_packs" && defaults.texture_pack_order.empty()
                && defaults.texture_packs_off.empty() && !defaults.dump_textures, "Texture pack defaults");
        Require(ResolveTextureFolder(defaults, directory / "recharged.ini") == directory / "texture_packs",
                "The texture packs folder counts from the settings file");
        std::ofstream(path) << "[mods]\ntexture_packs = false\ntexture_folder = \"../packs\"\n"
                               "texture_pack_order = R4Q.zip | HD Pack\ntexture_packs_off = Old Pack\ndump_textures = true\n";
        auto modsFile = LoadConfig(path);
        const auto& mods = modsFile.settings;
        Require(!mods.texture_packs && mods.texture_folder == "../packs" && mods.texture_pack_order == "R4Q.zip | HD Pack"
                && mods.texture_packs_off == "Old Pack" && mods.dump_textures, "Texture pack settings read");
        Require(ResolveTextureFolder(mods, directory / "recharged.ini") == directory / "../packs",
                "A relative texture packs folder");
        auto modsSettings = mods;
        modsSettings.texture_packs = true;
        modsSettings.texture_folder = "/srv/packs";
        modsSettings.texture_pack_order.clear();
        modsSettings.texture_packs_off = "R4Q.zip";
        modsSettings.dump_textures = false;
        SaveConfig(modsFile, modsSettings);
        const auto modsReloaded = LoadConfig(path);
        Require(modsReloaded.settings.texture_packs && modsReloaded.settings.texture_folder == "/srv/packs"
                && modsReloaded.settings.texture_pack_order.empty() && modsReloaded.settings.texture_packs_off == "R4Q.zip"
                && !modsReloaded.settings.dump_textures
                && modsReloaded.contents.find("texture_folder = \"/srv/packs\"") != std::string::npos,
                "Texture pack settings round trip");
        std::ofstream(path) << "[mods]\ntexture_folder =\n";
        Require(LoadConfig(path).settings.texture_folder == "texture_packs", "An empty texture packs folder is the default");
        // Old title-bar settings are accepted, ignored and retired on explicit save.
        std::ofstream(path) << "[display]\nshow_fps = true\nfps_overlay = false\ncustom = preserved\n";
        auto legacyFps = LoadConfig(path);
        Require(!legacyFps.settings.fps_overlay, "Old title FPS does not opt into the overlay");
        SaveConfig(legacyFps, legacyFps.settings);
        Require(legacyFps.contents.find("show_fps") == std::string::npos
            && legacyFps.contents.find("custom = preserved") != std::string::npos, "Retired title key is removed alone");
        // The old controls.deadzone was never read; gamepad profiles have their own (#62).
        std::ofstream(path) << "[controls]\ndeadzone = 40\nrumble = false\n";
        auto legacyDeadzone = LoadConfig(path);
        SaveConfig(legacyDeadzone, legacyDeadzone.settings);
        Require(legacyDeadzone.contents.find("deadzone") == std::string::npos
            && legacyDeadzone.contents.find("rumble = false") != std::string::npos, "Retired dead zone key is removed");
        // A 1.0.12 test build sent the Wii Remote speaker PCM on request (#60).
        std::ofstream(path) << "[controls]\nspeaker_format = pcm\nrumble = false\n";
        auto legacySpeaker = LoadConfig(path);
        SaveConfig(legacySpeaker, legacySpeaker.settings);
        Require(legacySpeaker.contents.find("speaker_format") == std::string::npos
            && legacySpeaker.contents.find("rumble = false") != std::string::npos, "Retired speaker format key is removed");
        for (const char* invalid : {"[display]\nmonitor = 16\n",
                                    "[display]\nfps_overlay = maybe\n", "[display]\nfps_overlay_corner = center\n",
                                    "[display]\nperformance_mode = maybe\n",
                                    "[advanced]\nlog_level = verbose\n", "[launcher]\nui_scale = 110\n",
                                    "[advanced]\ngraphics_validation = 2\n", "[advanced]\nverbose_console = yes\n",
                                    "[advanced]\nframe_logging = maybe\n", "[advanced]\nlog_window = on\n", "[launcher]\ncheck_updates = sometimes\n",
                                    "[launcher]\nupdate_channel = nightly\n", "[launcher]\ntest_build = -1\n",
                                    "[launcher]\nreopen_after_game = later\n",
                                    "[display]\ngpu = nvidia\n", "[display]\ngpu = 10de-1f99\n",
                                    "[display]\ngpu = 0000:1f99\n", "[display]\ngpu = 10de:1f9\n",
                                    "[launcher]\ntest_build = pr72\n", "[discord]\nrich_presence = maybe\n",
                                    "[discord]\napplication_id = 1234\n",
                                    "[discord]\napplication_id = 12345678901234567x8\n",
                                    "[mods]\ntexture_packs = maybe\n", "[mods]\ndump_textures = 2\n"})
        {
            std::ofstream(path) << invalid;
            Reject([&] { LoadConfig(path); });
        }
        // [gameplay] tweaks: only the recommended ones on by default and
        // without the section, and those turn off; each round trips on its
        // own and invalid values are rejected.
        const unsigned recommended = GameTweakBit(GameTweak::FairBallContests);
        Require(defaults.GameTweakMask() == recommended, "Only recommended tweaks are on by default");
        std::ofstream(path) << "[game]\ndisc = game.rvz\n";
        Require(LoadConfig(path).settings.GameTweakMask() == recommended,
            "No [gameplay] section means the recommended tweaks");
        std::ofstream(path) << "[game]\ndisc = game.rvz\n[gameplay]\nfair_ball_contests = false\n";
        Require(LoadConfig(path).settings.GameTweakMask() == 0, "A recommended tweak turns off");
        for (std::size_t n = 0; n < kGameTweaks.size(); ++n)
        {
            const auto tweaks = directory / ("tweaks" + std::to_string(n) + ".ini");
            auto tweakFile = LoadConfig(tweaks, true);
            Settings tweakSettings;
            tweakSettings.tweaks = {};
            tweakSettings.tweaks[n] = true;
            SaveConfig(tweakFile, tweakSettings);
            const auto reloaded = LoadConfig(tweaks);
            Require(reloaded.settings.GameTweakMask() == GameTweakBit(kGameTweaks[n].tweak), "Tweak round trip");
            Require(reloaded.contents.find(std::string("[gameplay]")) != std::string::npos
                && reloaded.contents.find(std::string(kGameTweaks[n].key) + " = true") != std::string::npos,
                "Tweak saved in [gameplay]");
        }
        // [hotkeys]: the screenshot and HUD keys and No HUD's start state,
        // which an old [gameplay] no_hud still sets until the next save.
        {
            Require(defaults.hotkeys
                        == std::array<std::string, HotkeyCount>{"P", "H", "F", "PageUp", "PageDown", "Alt+Return"}
                    && defaults.show_hud && defaults.ModuleTweakMask() == defaults.GameTweakMask(), "Hotkey defaults");
            Require(GameTweakIndex(GameTweak::NoHud) == kGameTweaks.size(), "No HUD is listed as a tweak");
            std::ofstream(path) << "[game]\ndisc = game.rvz\n[gameplay]\nno_hud = true\nskip_intro = true\n";
            auto old = LoadConfig(path);
            const unsigned tweaks = recommended | GameTweakBit(GameTweak::SkipIntro);
            Require(!old.settings.show_hud && old.settings.GameTweakMask() == tweaks
                    && old.settings.ModuleTweakMask() == (tweaks | GameTweakBit(GameTweak::NoHud)),
                    "gameplay.no_hud did not hide the HUD");
            auto changed = old.settings;
            changed.hotkeys[HotkeyScreenshot] = "F12";
            changed.hotkeys[HotkeyHud] = "G";
            changed.hotkeys[HotkeyFullscreen] = "Ctrl+Shift+F11";
            SaveConfig(old, changed);
            const auto saved = LoadConfig(path);
            Require(!saved.settings.show_hud && saved.settings.hotkeys[HotkeyScreenshot] == "F12"
                    && saved.settings.hotkeys[HotkeyHud] == "G" && saved.contents.find("stretch = F") != std::string::npos
                    && saved.settings.hotkeys[HotkeyFullscreen] == "Ctrl+Shift+F11"
                    && saved.contents.find("fullscreen = Ctrl+Shift+F11") != std::string::npos
                    && saved.contents.find("no_hud") == std::string::npos
                    && saved.contents.find("[hotkeys]") != std::string::npos
                    && saved.contents.find("show_hud = false") != std::string::npos
                    && saved.contents.find("skip_intro = true") != std::string::npos,
                    "The hotkeys did not round trip, or no_hud stayed");
            std::ofstream(path) << "[gameplay]\nno_hud = true\n[hotkeys]\nshow_hud = true\n";
            Require(LoadConfig(path).settings.show_hud, "gameplay.no_hud overrode hotkeys.show_hud");
            std::ofstream(path) << "[gameplay]\nno_hud = false\n";
            Require(LoadConfig(path).settings.show_hud, "gameplay.no_hud = false hid the HUD");
            // A hotkey an older INI does not name stays unused where a
            // [keyboard] action has its default key; one it names keeps it,
            // and none leaves a hotkey unused.
            std::ofstream(path) << "[keyboard]\nc = f\nz = V | PageDown\n[hotkeys]\nscreenshot = P\n";
            const auto older = LoadConfig(path).settings;
            Require(older.hotkeys[HotkeyStretch] == kNoHotkey && older.hotkeys[HotkeyResolutionDown] == kNoHotkey
                    && older.hotkeys[HotkeyResolutionUp] == "PageUp" && older.hotkeys[HotkeyScreenshot] == "P"
                    && older.hotkeys[HotkeyHud] == "H" && older.hotkeys[HotkeyFullscreen] == "Alt+Return",
                    "A new hotkey took an older INI's keyboard key");
            std::ofstream(path) << "[keyboard]\nc = F\n[hotkeys]\nstretch = F\nresolution_up = none\n";
            const auto named = LoadConfig(path).settings;
            Require(named.hotkeys[HotkeyStretch] == "F" && named.hotkeys[HotkeyResolutionUp] == kNoHotkey,
                    "A hotkey the INI names, or none, did not stay");
            for (const char* invalid : {"[display]\nfullscreen_resolution = big\n",
                                        "[display]\nfullscreen_resolution = 1280 x 720\n",
                                        "[display]\nfullscreen_resolution = 100x100\n",
                                        "[hotkeys]\nscreenshot = P | O\n", "[hotkeys]\nhud =\n",
                                        "[hotkeys]\nstretch = F | G\n", "[hotkeys]\nresolution_down =\n",
                                        "[hotkeys]\nshow_hud = maybe\n", "[gameplay]\nno_hud = 2\n"})
            {
                std::ofstream(path) << invalid;
                Reject([&] { LoadConfig(path); });
            }
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
