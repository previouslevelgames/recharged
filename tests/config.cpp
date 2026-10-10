#include "bootstrap/config.h"

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
        SaveConfig(file, settings);
        const auto reloaded = LoadConfig(path);
        Require(reloaded.settings.width == 1920 && reloaded.settings.height == 1080
            && reloaded.settings.fullscreen && reloaded.settings.master_volume == 65
            && reloaded.settings.players == settings.players && reloaded.settings.deadzone == 20
            && !reloaded.settings.rumble, "Settings round trip");
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
        // Pointer calibration: none or six numbers from the launcher.
        Require(Settings{}.remote_calibration[2] == "none", "Remotes start uncalibrated");
        std::ofstream(path) << "[controls]\nremote2_calibration = -0.0024 1e-05 1.68 0 -0.0026 1.58\n";
        Require(LoadConfig(path).settings.remote_calibration[1] == "-0.0024 1e-05 1.68 0 -0.0026 1.58",
                "A calibration loads");
        std::ofstream(path) << "[controls]\nremote1_calibration = 1 2 3\n";
        Reject([&] { LoadConfig(path); });
        std::ofstream(path) << "[controls]\nremote1_calibration = 1 2 3 4 5 6 x\n";
        Reject([&] { LoadConfig(path); });

        // Host presentation/diagnostic settings: defaults keep the previous
        // behaviour, values round trip and out-of-range input is rejected.
        const Settings defaults;
        Require(defaults.show_fps && defaults.monitor == 0 && !defaults.graphics_validation
            && defaults.log_level == "info" && !defaults.verbose_console && defaults.ui_scale == "auto",
            "Host setting defaults");
        const auto host = directory / "host.ini";
        auto hostFile = LoadConfig(host, true);
        Settings hostSettings;
        hostSettings.show_fps = false;
        hostSettings.monitor = 2;
        hostSettings.graphics_validation = true;
        hostSettings.log_level = "warning";
        hostSettings.verbose_console = true;
        hostSettings.ui_scale = "150";
        SaveConfig(hostFile, hostSettings);
        const auto hostReloaded = LoadConfig(host).settings;
        Require(!hostReloaded.show_fps && hostReloaded.monitor == 2 && hostReloaded.graphics_validation
            && hostReloaded.log_level == "warning" && hostReloaded.verbose_console && hostReloaded.ui_scale == "150",
            "Host settings round trip");
        const auto hostText = LoadConfig(host).contents;
        Require(hostText.find("[advanced]") != std::string::npos && hostText.find("[launcher]") != std::string::npos
            && hostText.find("show_fps = false") != std::string::npos, "Host settings sections");
        for (const char* invalid : {"[display]\nmonitor = 16\n", "[display]\nshow_fps = sometimes\n",
                                    "[advanced]\nlog_level = verbose\n", "[launcher]\nui_scale = 110\n",
                                    "[advanced]\ngraphics_validation = 2\n", "[advanced]\nverbose_console = yes\n"})
        {
            std::ofstream(path) << invalid;
            Reject([&] { LoadConfig(path); });
        }
        fs::remove_all(directory);
        std::cout << "Configuration round trips, preservation, validation, host settings and write protection passed.\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        fs::remove_all(directory);
        std::cerr << error.what() << '\n';
        return 1;
    }
}
