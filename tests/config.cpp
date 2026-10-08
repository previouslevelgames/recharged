#include "bootstrap/config.h"

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
        settings.input = "controller";
        settings.deadzone = 20;
        settings.rumble = false;
        SaveConfig(file, settings);
        const auto reloaded = LoadConfig(path);
        Require(reloaded.settings.width == 1920 && reloaded.settings.height == 1080
            && reloaded.settings.fullscreen && reloaded.settings.master_volume == 65
            && reloaded.settings.input == "controller" && reloaded.settings.deadzone == 20
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

        // Host presentation/diagnostic settings: defaults keep the previous
        // behaviour, values round trip and out-of-range input is rejected.
        const Settings defaults;
        Require(defaults.show_fps && defaults.monitor == 0 && defaults.graphics_validation
            && defaults.log_level == "info" && defaults.ui_scale == "auto", "Host setting defaults");
        const auto host = directory / "host.ini";
        auto hostFile = LoadConfig(host, true);
        Settings hostSettings;
        hostSettings.show_fps = false;
        hostSettings.monitor = 2;
        hostSettings.graphics_validation = false;
        hostSettings.log_level = "warning";
        hostSettings.ui_scale = "150";
        SaveConfig(hostFile, hostSettings);
        const auto hostReloaded = LoadConfig(host).settings;
        Require(!hostReloaded.show_fps && hostReloaded.monitor == 2 && !hostReloaded.graphics_validation
            && hostReloaded.log_level == "warning" && hostReloaded.ui_scale == "150", "Host settings round trip");
        const auto hostText = LoadConfig(host).contents;
        Require(hostText.find("[advanced]") != std::string::npos && hostText.find("[launcher]") != std::string::npos
            && hostText.find("show_fps = false") != std::string::npos, "Host settings sections");
        for (const char* invalid : {"[display]\nmonitor = 16\n", "[display]\nshow_fps = sometimes\n",
                                    "[advanced]\nlog_level = verbose\n", "[launcher]\nui_scale = 110\n",
                                    "[advanced]\ngraphics_validation = 2\n"})
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
