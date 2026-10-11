#include "config.h"
#include "platform/path.h"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <chrono>
#include <cmath>
#include <fstream>
#include <locale>
#include <map>
#include <set>
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
            {"display.antialiasing", s.antialiasing}, {"display.resolution", s.resolution},
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
    for (std::size_t n = 0; n < s.pad_profiles.size(); ++n)
    {
        const auto& profile = s.pad_profiles[n];
        const auto section = "gamepad_profile" + std::to_string(n + 1) + ".";
        values.emplace(section + "name", profile.name);
        for (std::size_t action = 0; action < GamepadActionCount; ++action)
            values.emplace(section + kGamepadActions[action].key, profile.inputs[action]);
        values.emplace(section + "swap_sticks", profile.swap_sticks ? "true" : "false");
        values.emplace(section + "controller", profile.controller);
    }
    for (std::size_t slot = 0; slot < kGamepadSlots; ++slot)
        values.emplace("controls.gamepad" + std::to_string(slot + 1) + "_profile", s.pad_profile_names[slot]);
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
    choice("display.resolution", s.resolution, {"window", "native", "2x", "3x", "4x"});
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
    // [keyboard] and [gamepadN]: one or two names per action, separated by " | ".
    const auto names = [&](const std::string& key, std::string& value, const char* what) {
        const auto it = values.find(key);
        if (it == values.end()) return;
        std::size_t count = 0, start = 0;
        bool valid = !it->second.empty();
        while (valid && start <= it->second.size())
        {
            const auto bar = std::min(it->second.find('|', start), it->second.size());
            valid = !Trim(it->second.substr(start, bar - start)).empty() && ++count <= 2;
            start = bar + 1;
        }
        if (!valid)
            throw std::runtime_error("Invalid configuration: " + key + " must be one or two " + what
                + " separated by |");
        value = it->second;
    };
    for (std::size_t n = 0; n < KeyActionCount; ++n)
        names(std::string("keyboard.") + kKeyActions[n].key, s.keys[n], "key names");
    // [gamepad_profileN]: numbered from 1, each with a unique name.
    for (auto it = values.lower_bound("gamepad_profile"); it != values.end() && it->first.rfind("gamepad_profile", 0) == 0;
         ++it)
    {
        const auto section = it->first.substr(0, it->first.find('.'));
        const auto digits = section.substr(std::string("gamepad_profile").size());
        std::size_t number = 0;
        const auto parsed = std::from_chars(digits.data(), digits.data() + digits.size(), number);
        if (digits.empty() || digits[0] == '0' || parsed.ec != std::errc{} || parsed.ptr != digits.data() + digits.size()
            || number < 1 || number > kMaxGamepadProfiles)
            throw std::runtime_error("Invalid configuration: gamepad profiles are [gamepad_profile1] to [gamepad_profile"
                + std::to_string(kMaxGamepadProfiles) + "], not [" + section + "]");
    }
    std::vector<Settings::PadProfile> profiles;
    for (std::size_t number = 1; number <= kMaxGamepadProfiles; ++number)
    {
        const auto section = "gamepad_profile" + std::to_string(number) + ".";
        const auto first = values.lower_bound(section);
        if (first == values.end() || first->first.rfind(section, 0) != 0) continue;
        const auto name = values.find(section + "name");
        if (name == values.end() || !ValidPadProfileName(name->second))
            throw std::runtime_error("Invalid configuration: " + section
                + "name must be 1-32 characters without quotes, brackets, = or |, and not Default");
        for (const auto& other : profiles)
            if (SamePadProfileName(other.name, name->second))
                throw std::runtime_error("Invalid configuration: two gamepad profiles are named " + name->second);
        Settings::PadProfile profile;
        profile.name = name->second;
        for (std::size_t action = 0; action < GamepadActionCount; ++action)
            names(section + kGamepadActions[action].key, profile.inputs[action], "gamepad inputs");
        boolean((section + "swap_sticks").c_str(), profile.swap_sticks);
        choice((section + "controller").c_str(), profile.controller, {"xbox", "nintendo", "gamecube"});
        profiles.push_back(std::move(profile));
    }
    s.pad_profiles = std::move(profiles);
    // A gamepad whose profile no longer exists plays with the built-in Default.
    for (std::size_t slot = 0; slot < kGamepadSlots; ++slot)
    {
        const auto it = values.find("controls.gamepad" + std::to_string(slot + 1) + "_profile");
        if (it != values.end()) s.pad_profile_names[slot] = it->second;
        s.pad_profile_names[slot] = s.PadProfileOf(slot).name;
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
// Sections the settings own completely: on save, their keys that the
// settings no longer contain are removed (a deleted gamepad profile).
bool OwnedSection(const std::string& section)
{
    const std::string prefix = "gamepad_profile";
    return section.size() > prefix.size() && section.compare(0, prefix.size(), prefix) == 0
        && std::all_of(section.begin() + std::ptrdiff_t(prefix.size()), section.end(),
                       [](unsigned char c) { return std::isdigit(c); });
}

Values Parse(const std::string& contents, std::string* updated = nullptr, Values replacements = {})
{
    Values values;
    std::set<std::string> kept;
    for (const auto& [key, value] : replacements) kept.insert(key);
    // An owned section's header (and comments) wait for its first kept key.
    std::string held;
    bool holding = false;
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
        bool header = false, key_line = false, drop = false;
        if (!text.empty() && text[0] != '#' && text[0] != ';')
        {
            if (text.front() == '[' && text.back() == ']')
            {
                section = Trim(text.substr(1, text.size() - 2));
                header = true;
            }
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
                key_line = true;
                drop = updated && OwnedSection(section) && !kept.count(full_key);
                if (const auto it = replacements.find(full_key); it != replacements.end())
                {
                    if (it->second != value)
                        line = line.substr(0, equals + 1) + " "
                            + (full_key == "game.disc" ? "\"" + it->second + "\"" : it->second);
                    replacements.erase(it);
                }
            }
        }
        if (!updated) continue;
        const std::string piece = (bom ? "\xEF\xBB\xBF" : "") + line + newline;
        if (header)
        {
            // The previous owned section kept no key: leave it out entirely.
            held.clear();
            holding = OwnedSection(section);
            if (holding) { held = piece; continue; }
        }
        if (drop) continue;
        if (holding && !key_line) { held += piece; continue; }
        if (holding) { *updated += held; held.clear(); holding = false; }
        *updated += piece;
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

bool ValidPadProfileName(const std::string& name)
{
    if (name.empty() || name.size() > kMaxGamepadProfileName || name != Trim(name)
        || SamePadProfileName(name, kDefaultGamepadProfile))
        return false;
    return std::none_of(name.begin(), name.end(), [](unsigned char c) {
        return c < 0x20 || c == 0x7f || c == '"' || c == '[' || c == ']' || c == '=' || c == '|';
    });
}

bool SamePadProfileName(const std::string& a, const std::string& b)
{
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](unsigned char x, unsigned char y) {
        return std::tolower(x) == std::tolower(y);
    });
}

int Settings::PadProfileIndex(std::size_t slot) const
{
    for (std::size_t n = 0; n < pad_profiles.size(); ++n)
        if (SamePadProfileName(pad_profiles[n].name, pad_profile_names[slot])) return int(n);
    return -1;
}

const Settings::PadProfile& Settings::PadProfileOf(std::size_t slot) const
{
    const int index = PadProfileIndex(slot);
    return index < 0 ? DefaultPadProfile() : pad_profiles[std::size_t(index)];
}

const Settings::PadProfile& Settings::DefaultPadProfile()
{
    static const PadProfile profile{kDefaultGamepadProfile, DefaultPadInputs(), false};
    return profile;
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
