#include "config.h"
#include "platform/path.h"

#include <charconv>
#include <chrono>
#include <fstream>
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
    return {{"game.disc", s.disc}, {"game.language", s.language},
            {"display.width", std::to_string(s.width)}, {"display.height", std::to_string(s.height)},
            {"display.fullscreen", s.fullscreen ? "true" : "false"},
            {"display.vsync", s.vsync ? "true" : "false"},
            {"display.backend", s.backend}, {"display.aspect", s.aspect},
            {"audio.master_volume", std::to_string(s.master_volume)},
            {"audio.music_volume", std::to_string(s.music_volume)},
            {"audio.effects_volume", std::to_string(s.effects_volume)},
            {"audio.mute", s.mute ? "true" : "false"}, {"controls.input", s.input},
            {"controls.deadzone", std::to_string(s.deadzone)},
            {"controls.rumble", s.rumble ? "true" : "false"}};
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
    number("audio.master_volume", s.master_volume, 0, 100);
    number("audio.music_volume", s.music_volume, 0, 100);
    number("audio.effects_volume", s.effects_volume, 0, 100);
    boolean("audio.mute", s.mute);
    choice("controls.input", s.input, {"auto", "keyboard", "controller"});
    number("controls.deadzone", s.deadzone, 0, 50);
    boolean("controls.rumble", s.rumble);
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
