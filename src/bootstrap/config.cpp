#include "config.h"
#include "platform/path.h"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <fstream>
#include <locale>
#include <map>
#include <sstream>
#include <stdexcept>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#endif

namespace mscharged
{
namespace
{
using Values = std::map<std::string, std::string>;

std::string Trim(const std::string& value)
{
    const auto start = value.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return {};
    return value.substr(start, value.find_last_not_of(" \t\r\n") - start + 1);
}

Values Encode(const Settings& s)
{
    Values values{{"game.disc", s.disc}, {"game.language", s.language},
            {"display.width", std::to_string(s.width)}, {"display.height", std::to_string(s.height)},
            {"display.fullscreen", s.fullscreen ? "true" : "false"},
            {"display.vsync", s.vsync ? "true" : "false"},
            {"display.backend", s.backend}, {"display.aspect", s.aspect}, {"display.picture", s.picture},
            {"display.antialiasing", s.antialiasing},
            {"audio.master_volume", std::to_string(s.master_volume)},
            {"audio.music_volume", std::to_string(s.music_volume)},
            {"audio.effects_volume", std::to_string(s.effects_volume)},
            {"audio.mute", s.mute ? "true" : "false"},
            {"controls.player1", s.players[0]}, {"controls.player2", s.players[1]},
            {"controls.player3", s.players[2]}, {"controls.player4", s.players[3]},
            {"controls.remote1_calibration", s.remote_calibration[0]},
            {"controls.remote2_calibration", s.remote_calibration[1]},
            {"controls.remote3_calibration", s.remote_calibration[2]},
            {"controls.remote4_calibration", s.remote_calibration[3]},
            {"controls.deadzone", std::to_string(s.deadzone)},
            {"controls.rumble", s.rumble ? "true" : "false"},
            {"controls.mouse_pointer", s.mouse_pointer ? "true" : "false"},
            {"controls.sensor_bar", s.sensor_bar},
            {"display.show_fps", s.show_fps ? "true" : "false"},
            {"display.monitor", std::to_string(s.monitor)},
            {"advanced.graphics_validation", s.graphics_validation ? "true" : "false"},
            {"advanced.log_level", s.log_level},
            {"advanced.verbose_console", s.verbose_console ? "true" : "false"},
            {"launcher.ui_scale", s.ui_scale}};
    for (std::size_t n = 0; n < KeyActionCount; ++n)
        values.emplace(std::string("keyboard.") + kKeyActions[n].key, s.keys[n]);
    return values;
}

Settings Decode(const Values& values)
{
    Settings s;
    auto choice = [&](const char* key, std::string& value, std::initializer_list<const char*> options) {
        const auto it = values.find(key);
        if (it == values.end()) return;
        for (const char* option : options)
            if (it->second == option) { value = it->second; return; }
        throw std::runtime_error(std::string("Invalid configuration: unsupported ") + key);
    };
    auto number = [&](const char* key, int& value, int minimum, int maximum) {
        const auto it = values.find(key);
        if (it == values.end()) return;
        const auto& text = it->second;
        int parsed = 0;
        const auto result = std::from_chars(text.data(), text.data() + text.size(), parsed);
        if (result.ec != std::errc{} || result.ptr != text.data() + text.size()
            || parsed < minimum || parsed > maximum)
            throw std::runtime_error(std::string("Invalid configuration: ") + key
                + " must be between " + std::to_string(minimum) + " and " + std::to_string(maximum));
        value = parsed;
    };
    auto boolean = [&](const char* key, bool& value) {
        const auto it = values.find(key);
        if (it == values.end()) return;
        if (it->second == "true" || it->second == "1") value = true;
        else if (it->second == "false" || it->second == "0") value = false;
        else throw std::runtime_error(std::string("Invalid configuration: ") + key + " must be true or false");
    };
    if (const auto it = values.find("game.disc"); it != values.end()) s.disc = it->second;
    if (s.disc.find_first_of("\r\n") != std::string::npos || s.disc.find('\0') != std::string::npos)
        throw std::runtime_error("Invalid configuration: the disc path contains a line break or NUL");
    choice("game.language", s.language, {"auto", "english", "french", "spanish", "german", "italian", "japanese"});
    number("display.width", s.width, 1, 16384);
    number("display.height", s.height, 1, 16384);
    boolean("display.fullscreen", s.fullscreen);
    boolean("display.vsync", s.vsync);
    choice("display.backend", s.backend, {"auto", "vulkan", "metal", "d3d12"});
    choice("display.aspect", s.aspect, {"auto", "4:3", "16:9", "16:10", "21:9"});
    choice("display.picture", s.picture, {"soft", "clean", "sharp"});
    choice("display.antialiasing", s.antialiasing, {"off", "4x"});
    number("audio.master_volume", s.master_volume, 0, 100);
    number("audio.music_volume", s.music_volume, 0, 100);
    number("audio.effects_volume", s.effects_volume, 0, 100);
    boolean("audio.mute", s.mute);
    for (int n = 0; n < 4; ++n)
    {
        const auto key = "controls.player" + std::to_string(n + 1);
        choice(key.c_str(), s.players[n], {"keyboard", "remote1", "remote2", "remote3", "remote4",
                                            "gamepad1", "gamepad2", "gamepad3", "gamepad4", "off"});
    }
    for (int n = 0; n < 4; ++n)
    {
        const auto key = "controls.remote" + std::to_string(n + 1) + "_calibration";
        const auto it = values.find(key);
        if (it == values.end()) continue;
        if (it->second != "none")
        {
            std::istringstream input(it->second);
            input.imbue(std::locale::classic());
            double value = 0;
            int count = 0;
            while (input >> value && std::isfinite(value)) ++count;
            if (count != 6 || !input.eof())
                throw std::runtime_error("Invalid configuration: " + key + " must be none or six numbers");
        }
        s.remote_calibration[n] = it->second;
    }
    // [keyboard]: one or two key names per action, separated by " | ".
    for (std::size_t n = 0; n < KeyActionCount; ++n)
    {
        const auto key = std::string("keyboard.") + kKeyActions[n].key;
        const auto it = values.find(key);
        if (it == values.end()) continue;
        std::size_t names = 0, start = 0;
        bool valid = !it->second.empty();
        while (valid && start <= it->second.size())
        {
            const auto bar = std::min(it->second.find('|', start), it->second.size());
            valid = !Trim(it->second.substr(start, bar - start)).empty() && ++names <= 2;
            start = bar + 1;
        }
        if (!valid)
            throw std::runtime_error("Invalid configuration: " + key + " must be one or two key names separated by |");
        s.keys[n] = it->second;
    }
    // Players fill in order from player 1, and each device plays once.
    if (s.players[0] == "off")
        throw std::runtime_error("Invalid configuration: controls.player1 must be keyboard, a Wii Remote or a gamepad");
    for (int n = 1; n < 4; ++n)
    {
        if (s.players[n] == "off") continue;
        if (s.players[n - 1] == "off")
            throw std::runtime_error("Invalid configuration: players must be filled in order (controls.player"
                + std::to_string(n + 1) + ")");
        for (int m = 0; m < n; ++m)
            if (s.players[n] == s.players[m])
                throw std::runtime_error("Invalid configuration: " + s.players[n] + " is assigned to two players");
    }
    number("controls.deadzone", s.deadzone, 0, 50);
    boolean("controls.rumble", s.rumble);
    boolean("controls.mouse_pointer", s.mouse_pointer);
    choice("controls.sensor_bar", s.sensor_bar, {"bottom", "top"});
    boolean("display.show_fps", s.show_fps);
    number("display.monitor", s.monitor, 0, 15);
    boolean("advanced.graphics_validation", s.graphics_validation);
    choice("advanced.log_level", s.log_level, {"error", "warning", "info", "debug"});
    boolean("advanced.verbose_console", s.verbose_console);
    choice("launcher.ui_scale", s.ui_scale, {"auto", "75", "100", "125", "150", "175", "200"});
    return s;
}

// Preserve comments and unknown settings when rewriting known keys.
Values Parse(const std::string& contents, std::string* updated = nullptr, Values replacements = {})
{
    Values values;
    std::istringstream input(contents);
    std::string section, line;
    size_t number = 0;
    const std::string newline = contents.find("\r\n") == std::string::npos ? "\n" : "\r\n";
    while (std::getline(input, line))
    {
        ++number;
        const bool bom = number == 1 && line.compare(0, 3, "\xEF\xBB\xBF") == 0;
        if (bom) line.erase(0, 3);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const auto text = Trim(line);
        if (!text.empty() && text[0] != '#' && text[0] != ';')
        {
            if (text.front() == '[' && text.back() == ']')
                section = Trim(text.substr(1, text.size() - 2));
            else
            {
                const auto equals = line.find('=');
                const auto key = equals == std::string::npos ? "" : Trim(line.substr(0, equals));
                if (section.empty() || key.empty())
                    throw std::runtime_error("Invalid configuration at line " + std::to_string(number));
                const auto full_key = section + "." + key;
                auto value = Trim(line.substr(equals + 1));
                if (value.size() >= 2 && value.front() == '"' && value.back() == '"')
                    value = value.substr(1, value.size() - 2);
                if (!values.emplace(full_key, value).second)
                    throw std::runtime_error("Invalid configuration: duplicate " + full_key);
                if (const auto it = replacements.find(full_key); it != replacements.end())
                {
                    if (it->second != value)
                        line = line.substr(0, equals + 1) + " "
                            + (full_key == "game.disc" ? "\"" + it->second + "\"" : it->second);
                    replacements.erase(it);
                }
            }
        }
        if (updated) *updated += (bom ? "\xEF\xBB\xBF" : "") + line + newline;
    }
    if (updated)
    {
        section.clear();
        for (const auto& [full_key, value] : replacements)
        {
            const auto dot = full_key.find('.');
            const auto next = full_key.substr(0, dot);
            if (next != section) { *updated += newline + "[" + next + "]" + newline; section = next; }
            *updated += full_key.substr(dot + 1) + " = "
                + (full_key == "game.disc" ? "\"" + value + "\"" : value) + newline;
        }
    }
    return values;
}

std::string Read(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    if (!input)
        throw std::runtime_error("Cannot read " + PathUtf8(path)
            + ". Copy mscharged.ini.example to mscharged.ini and set [game] disc.");
    if (std::filesystem::file_size(path) > 1024 * 1024)
        throw std::runtime_error("Configuration file is unexpectedly large: " + PathUtf8(path));
    std::ostringstream result;
    result << input.rdbuf();
    if (input.bad()) throw std::runtime_error("Failed to read configuration: " + PathUtf8(path));
    return result.str();
}
}

ConfigFile LoadConfig(const std::filesystem::path& path, bool allow_missing)
{
    ConfigFile file;
    file.path = std::filesystem::absolute(path);
    file.exists = std::filesystem::exists(file.path);
    if (allow_missing && !file.exists) return file;
    file.contents = Read(file.path);
    const auto values = Parse(file.contents);
    file.settings = Decode(values);
    for (const auto& [key, value] : values) file.configured_keys.insert(key);
    return file;
}

void SaveConfig(ConfigFile& file, const Settings& settings)
{
    const auto values = Encode(settings);
    Decode(values);
    const bool exists = std::filesystem::exists(file.path);
    if (exists != file.exists || (exists && Read(file.path) != file.contents))
        throw std::runtime_error("Configuration changed on disk. Reload it before saving.");
    std::string contents;
    Parse(file.contents, &contents, values);
    auto temporary = file.path;
    temporary += ".tmp-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    if (std::filesystem::exists(temporary))
        throw std::runtime_error("Temporary configuration file already exists");
    try
    {
        std::ofstream output(temporary, std::ios::binary);
        output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
        output.close();
        if (!output) throw std::runtime_error("Cannot write configuration: " + PathUtf8(file.path));
#ifdef _WIN32
        if (!MoveFileExW(temporary.c_str(), file.path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            throw std::runtime_error("Cannot replace configuration: " + PathUtf8(file.path));
#else
        std::filesystem::rename(temporary, file.path);
#endif
    }
    catch (...)
    {
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        throw;
    }
    file.contents = contents;
    file.settings = settings;
    file.configured_keys.clear();
    for (const auto& [key, value] : Parse(contents)) file.configured_keys.insert(key);
    file.exists = true;
}

std::filesystem::path ResolveDiscPath(const Settings& settings, const std::filesystem::path& config)
{
    if (settings.disc.empty())
        throw std::runtime_error("Set [game] disc to an ISO or RVZ path in " + PathUtf8(config));
    const auto path = PathFromUtf8(settings.disc);
    return path.is_absolute() ? path : config.parent_path() / path;
}

std::filesystem::path LoadDiscPath(const std::filesystem::path& config)
{
    const auto file = LoadConfig(config);
    return ResolveDiscPath(file.settings, file.path);
}
}
