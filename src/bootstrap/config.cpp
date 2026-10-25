#include "config.h"
#include "display_shape.h"
#include "graphics_card.h"
#include "platform/path.h"
#include "platform/wiimote_ids.h"

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

// Whether a [keyboard] binding ("Return | Space") names the key `name`; SDL
// key names compare case-insensitively.
bool BindingNamesKey(const std::string& binding, const char* name)
{
    std::size_t start = 0;
    while (start <= binding.size())
    {
        const auto bar = std::min(binding.find('|', start), binding.size());
        const auto key = Trim(binding.substr(start, bar - start));
        if (std::equal(key.begin(), key.end(), name, name + std::char_traits<char>::length(name),
                       [](unsigned char a, unsigned char b) { return std::tolower(a) == std::tolower(b); }))
            return true;
        start = bar + 1;
    }
    return false;
}

Values Encode(const Settings& s)
{
    Values values{{"game.disc", s.disc}, {"game.folder", s.folder},
            {"game.folder_subfolders", s.folder_subfolders ? "true" : "false"},
            {"game.save_folder", s.save_folder}, {"game.gamecube_disc", s.gamecube_disc},
            {"game.language", s.language}, {"gameplay.super_team_side", s.super_team_side},
            {"gameplay.training_item", s.training_item}, {"gameplay.training_captain", s.training_captain},
            {"gameplay.training_stadium", s.training_stadium},
            {"display.width", std::to_string(s.width)}, {"display.height", std::to_string(s.height)},
            {"display.fullscreen", s.fullscreen ? "true" : "false"},
            {"display.fullscreen_resolution", s.fullscreen_resolution},
            {"display.vsync", s.vsync ? "true" : "false"}, {"display.frame_rate", s.frame_rate},
            {"display.backend", s.backend}, {"display.gpu", s.gpu}, {"display.aspect", s.aspect},
            {"display.picture", s.picture}, {"display.tv_border", s.tv_border ? "true" : "false"},
            {"display.wider_view", s.wider_view ? "true" : "false"},
            {"display.remember_window", s.remember_window ? "true" : "false"}, {"display.position", s.position},
            {"display.performance_mode", s.performance_mode ? "true" : "false"},
            {"display.antialiasing", s.antialiasing}, {"display.resolution", s.resolution},
            {"audio.master_volume", std::to_string(s.master_volume)},
            {"audio.mute", s.mute ? "true" : "false"},
            {"controls.player1", s.players[0]}, {"controls.player2", s.players[1]},
            {"controls.player3", s.players[2]}, {"controls.player4", s.players[3]},
            {"controls.rumble", s.rumble ? "true" : "false"},
            {"controls.rumble_strength", std::to_string(s.rumble_strength)},
            {"controls.ir_sensitivity", std::to_string(s.ir_sensitivity)},
            {"controls.speaker_volume", std::to_string(s.speaker_volume)},
            {"controls.mouse_pointer", s.mouse_pointer ? "true" : "false"},
            {"controls.bluetooth_search", s.bluetooth_search ? "true" : "false"},
            {"controls.sensor_bar", s.sensor_bar},
            {"display.fps_overlay", s.fps_overlay ? "true" : "false"},
            {"display.fps_overlay_corner", s.fps_overlay_corner},
            {"display.monitor", std::to_string(s.monitor)},
            {"advanced.graphics_validation", s.graphics_validation ? "true" : "false"},
            {"advanced.log_level", s.log_level},
            {"advanced.verbose_console", s.verbose_console ? "true" : "false"},
            {"advanced.frame_logging", s.frame_logging ? "true" : "false"},
            {"advanced.log_window", s.log_window ? "true" : "false"},
            {"launcher.ui_scale", s.ui_scale},
            {"launcher.check_updates", s.check_updates ? "true" : "false"},
            {"online.server", s.online_server}, {"online.rollback", s.rollback ? "true" : "false"},
            {"launcher.reopen_after_game", s.reopen_after_game ? "true" : "false"},
            {"launcher.show_welcome", s.show_welcome ? "true" : "false"},
            {"launcher.update_channel", s.update_channel},
            {"launcher.test_build", std::to_string(s.test_build)},
            {"discord.rich_presence", s.discord_status ? "true" : "false"},
            {"mods.texture_packs", s.texture_packs ? "true" : "false"}, {"mods.texture_folder", s.texture_folder},
            {"mods.texture_pack_order", s.texture_pack_order}, {"mods.texture_packs_off", s.texture_packs_off},
            {"mods.dump_textures", s.dump_textures ? "true" : "false"}};
    for (std::size_t n = 0; n < KeyActionCount; ++n)
        values.emplace(std::string("keyboard.") + kKeyActions[n].key, s.keys[n]);
    for (std::size_t n = 0; n < HotkeyCount; ++n)
        values.emplace(std::string("hotkeys.") + kHotkeys[n].key, s.hotkeys[n]);
    values.emplace("hotkeys.show_hud", s.show_hud ? "true" : "false");
    for (std::size_t n = 0; n < kGameTweaks.size(); ++n)
        values.emplace(std::string("gameplay.") + kGameTweaks[n].key, s.tweaks[n] ? "true" : "false");
    for (std::size_t n = 0; n < kSoundGroups.size(); ++n)
        values.emplace(std::string("audio.") + kSoundGroups[n].key, std::to_string(s.sound_volumes[n]));
    for (std::size_t n = 0; n < s.pad_profiles.size(); ++n)
    {
        const auto& profile = s.pad_profiles[n];
        const auto section = "gamepad_profile" + std::to_string(n + 1) + ".";
        values.emplace(section + "name", profile.name);
        for (std::size_t action = 0; action < GamepadActionCount; ++action)
            values.emplace(section + kGamepadActions[action].key, profile.inputs[action]);
        values.emplace(section + "swap_sticks", profile.swap_sticks ? "true" : "false");
        values.emplace(section + "inner_deadzone", std::to_string(profile.inner_deadzone));
        values.emplace(section + "outer_deadzone", std::to_string(profile.outer_deadzone));
        values.emplace(section + "trigger_press", std::to_string(profile.trigger_press));
        values.emplace(section + "gyro_pointer", profile.gyro_pointer ? "true" : "false");
        values.emplace(section + "gyro_speed", std::to_string(profile.gyro_speed));
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
    // The first builds of #65 kept a list of folders in [library]: the first one
    // becomes the game folder.
    const auto folder = values.count("game.folder") ? values.find("game.folder") : values.find("library.folder1");
    if (folder != values.end() && !folder->second.empty()) s.folder = folder->second;
    if (s.folder.find_first_of("\r\n") != std::string::npos || s.folder.find('\0') != std::string::npos)
        throw std::runtime_error("Invalid configuration: the game folder contains a line break or NUL");
    if (values.count("game.folder")) boolean("game.folder_subfolders", s.folder_subfolders);
    else if (const auto old = values.find("library.folder1_subfolders"); old != values.end())
        s.folder_subfolders = old->second != "false" && old->second != "0";
    if (const auto it = values.find("game.save_folder"); it != values.end()) s.save_folder = it->second;
    if (s.save_folder.find_first_of("\r\n") != std::string::npos || s.save_folder.find('\0') != std::string::npos)
        throw std::runtime_error("Invalid configuration: the save folder contains a line break or NUL");
    if (const auto it = values.find("game.gamecube_disc"); it != values.end()) s.gamecube_disc = it->second;
    if (s.gamecube_disc.find_first_of("\r\n") != std::string::npos || s.gamecube_disc.find('\0') != std::string::npos)
        throw std::runtime_error("Invalid configuration: the GameCube disc path contains a line break or NUL");
    choice("game.language", s.language, {"auto", "english", "french", "spanish", "german", "italian", "japanese"});
    number("display.width", s.width, 1, 16384);
    number("display.height", s.height, 1, 16384);
    boolean("display.fullscreen", s.fullscreen);
    if (const auto it = values.find("display.fullscreen_resolution"); it != values.end())
    {
        if (it->second != kDesktopResolution && ParseResolution(it->second).width == 0)
            throw std::runtime_error("Invalid configuration: display.fullscreen_resolution must be desktop or "
                                     "WIDTHxHEIGHT, such as 1920x1080");
        s.fullscreen_resolution = it->second;
    }
    boolean("display.vsync", s.vsync);
    if (const auto it = values.find("display.frame_rate"); it != values.end())
    {
        if (!ParseFrameRate(it->second))
            throw std::runtime_error("Invalid configuration: display.frame_rate must be original, display, unlimited "
                "or a number of frames per second from " + std::to_string(kMinFrameRate) + " to "
                + std::to_string(kMaxFrameRate));
        s.frame_rate = it->second;
    }
    choice("display.backend", s.backend, {"auto", "vulkan", "metal", "d3d12"});
    if (const auto it = values.find("display.gpu"); it != values.end())
    {
        try
        {
            ParseGraphicsCard(it->second);
        }
        catch (const std::invalid_argument&)
        {
            throw std::runtime_error("Invalid configuration: display.gpu must be auto or a graphics card's PCI ID "
                                     "such as 10de:1f99");
        }
        s.gpu = it->second;
    }
    choice("display.aspect", s.aspect, {"auto", "4:3", "16:9", "stretch", "16:10", "21:9"});
    choice("display.picture", s.picture, {"soft", "clean", "sharp"});
    boolean("display.tv_border", s.tv_border);
    boolean("display.wider_view", s.wider_view);
    boolean("display.remember_window", s.remember_window);
    if (const auto it = values.find("display.position"); it != values.end())
    {
        if (!it->second.empty() && !ParseWindowPosition(it->second))
            throw std::runtime_error("Invalid configuration: display.position must be X,Y (the window's position on "
                                     "the desktop) or empty");
        s.position = it->second;
    }
    boolean("display.performance_mode", s.performance_mode);
    choice("display.antialiasing", s.antialiasing, {"off", "fxaa", "4x", "4x_fxaa", "4x_ssaa"});
    choice("display.resolution", s.resolution, {"window", "native", "2x", "3x", "4x"});
    number("audio.master_volume", s.master_volume, 0, 100);
    for (std::size_t n = 0; n < kSoundGroups.size(); ++n)
        number((std::string("audio.") + kSoundGroups[n].key).c_str(), s.sound_volumes[n], 0, 100);
    boolean("audio.mute", s.mute);
    for (int n = 0; n < 4; ++n)
    {
        const auto key = "controls.player" + std::to_string(n + 1);
        const auto it = values.find(key);
        if (it == values.end()) continue;
        if (platform::IsBluetoothWiimoteId(it->second)) { s.players[n] = it->second; continue; }
        choice(key.c_str(), s.players[n], {"keyboard", "remote1", "remote2", "remote3", "remote4",
                                            "gamepad1", "gamepad2", "gamepad3", "gamepad4", "off"});
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
    // [hotkeys]: one key name each, after the Ctrl+, Shift+ and Alt+ it is
    // pressed with, or none. A hotkey an older INI does not name yet stays
    // unused when a [keyboard] action has its default key.
    for (std::size_t n = 0; n < HotkeyCount; ++n)
    {
        const auto key = std::string("hotkeys.") + kHotkeys[n].key;
        const auto it = values.find(key);
        if (it == values.end())
        {
            for (const auto& keys : s.keys)
                if (BindingNamesKey(keys, kHotkeys[n].defaults)) s.hotkeys[n] = kNoHotkey;
            continue;
        }
        if (it->second.empty() || it->second.find('|') != std::string::npos)
            throw std::runtime_error("Invalid configuration: " + key
                + " must be one key name, after Ctrl+, Shift+ or Alt+ if needed, or none");
        s.hotkeys[n] = it->second;
    }
    // No HUD was the [gameplay] tweak no_hud until it moved beside its key.
    if (values.count("hotkeys.show_hud")) boolean("hotkeys.show_hud", s.show_hud);
    else if (bool no_hud = false; values.count("gameplay.no_hud"))
    {
        boolean("gameplay.no_hud", no_hud);
        s.show_hud = !no_hud;
    }
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
    for (std::size_t index = 1; index <= kMaxGamepadProfiles; ++index)
    {
        const auto section = "gamepad_profile" + std::to_string(index) + ".";
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
        number((section + "inner_deadzone").c_str(), profile.inner_deadzone, kMinInnerDeadzone, kMaxInnerDeadzone);
        number((section + "outer_deadzone").c_str(), profile.outer_deadzone, kMinOuterDeadzone, kMaxOuterDeadzone);
        number((section + "trigger_press").c_str(), profile.trigger_press, kMinTriggerPress, kMaxTriggerPress);
        boolean((section + "gyro_pointer").c_str(), profile.gyro_pointer);
        number((section + "gyro_speed").c_str(), profile.gyro_speed, kMinGyroSpeed, kMaxGyroSpeed);
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
    boolean("controls.rumble", s.rumble);
    number("controls.rumble_strength", s.rumble_strength, 0, 100);
    number("controls.ir_sensitivity", s.ir_sensitivity, 1, 5);
    number("controls.speaker_volume", s.speaker_volume, 0, 100);
    boolean("controls.mouse_pointer", s.mouse_pointer);
    boolean("controls.bluetooth_search", s.bluetooth_search);
    choice("controls.sensor_bar", s.sensor_bar, {"bottom", "top"});
    boolean("display.fps_overlay", s.fps_overlay);
    choice("display.fps_overlay_corner", s.fps_overlay_corner, {"top-left", "top-right", "bottom-left", "bottom-right"});
    number("display.monitor", s.monitor, 0, 15);
    boolean("advanced.graphics_validation", s.graphics_validation);
    choice("advanced.log_level", s.log_level, {"error", "warning", "info", "debug"});
    boolean("advanced.verbose_console", s.verbose_console);
    boolean("advanced.frame_logging", s.frame_logging);
    boolean("advanced.log_window", s.log_window);
    choice("launcher.ui_scale", s.ui_scale, {"auto", "75", "100", "125", "150", "175", "200"});
    boolean("launcher.check_updates", s.check_updates);
    boolean("launcher.reopen_after_game", s.reopen_after_game);
    boolean("launcher.show_welcome", s.show_welcome);
    choice("launcher.update_channel", s.update_channel, {"auto", "release", "test"});
    number("launcher.test_build", s.test_build, 0, 999999999);
    boolean("discord.rich_presence", s.discord_status);
    if (const auto it = values.find("discord.application_id"); it != values.end() && !it->second.empty())
    {
        const auto& id = it->second;
        if (id.size() < 17 || id.size() > 20 || id.find_first_not_of("0123456789") != std::string::npos)
            throw std::runtime_error("Invalid configuration: discord.application_id must be a Discord application ID "
                "(17 to 20 digits)");
        s.discord_application_id = id;
    }
    boolean("mods.texture_packs", s.texture_packs);
    if (const auto it = values.find("mods.texture_folder"); it != values.end() && !it->second.empty())
        s.texture_folder = it->second;
    if (const auto it = values.find("mods.texture_pack_order"); it != values.end()) s.texture_pack_order = it->second;
    if (const auto it = values.find("mods.texture_packs_off"); it != values.end()) s.texture_packs_off = it->second;
    for (const auto* text : {&s.texture_folder, &s.texture_pack_order, &s.texture_packs_off})
        if (text->find_first_of("\r\n") != std::string::npos || text->find('\0') != std::string::npos)
            throw std::runtime_error("Invalid configuration: a [mods] setting contains a line break or NUL");
    boolean("mods.dump_textures", s.dump_textures);
    for (std::size_t n = 0; n < kGameTweaks.size(); ++n)
    {
        bool value = kGameTweaks[n].on_by_default;
        boolean((std::string("gameplay.") + kGameTweaks[n].key).c_str(), value);
        s.tweaks[n] = value;
    }
    if (const auto it = values.find("online.server"); it != values.end() && !it->second.empty())
        s.online_server = it->second;
    boolean("online.rollback", s.rollback);
    boolean("online.rollback_test", s.rollback_test);
    if (const auto it = values.find("online.rollback_log"); it != values.end()) s.rollback_log = it->second;
    if (s.rollback_log.find_first_of("\r\n") != std::string::npos || s.rollback_log.find('\0') != std::string::npos)
        throw std::runtime_error("Invalid configuration: online.rollback_log contains a line break or NUL");
    if (s.online_server.find_first_of("\r\n") != std::string::npos || s.online_server.find('\0') != std::string::npos)
        throw std::runtime_error("Invalid configuration: the online server redirect contains a line break or NUL");

    choice("gameplay.super_team_side", s.super_team_side, {"away", "home", "both"});
    auto training = [&](const char* key, std::string& value, const auto& names) {
        const auto it = values.find(key);
        if (it == values.end()) return;
        if (TrainingIndex(names, it->second) < 0)
            throw std::runtime_error(std::string("Invalid configuration: unsupported ") + key);
        value = it->second;
    };
    training("gameplay.training_item", s.training_item, kTrainingItems);
    training("gameplay.training_captain", s.training_captain, kTrainingCaptains);
    training("gameplay.training_stadium", s.training_stadium, kTrainingStadiums);
    return s;
}

// Preserve comments and unknown settings when rewriting known keys.
// Sections the settings own completely: on save, their keys that the
// settings no longer contain are removed (a deleted gamepad profile).
bool OwnedSection(const std::string& section)
{
    // [library]: the folder list of the first #65 builds, now game.folder; saving drops it.
    if (section == "library") return true;
    const std::string prefix = "gamepad_profile";
    return section.size() > prefix.size() && section.compare(0, prefix.size(), prefix) == 0
        && std::all_of(section.begin() + std::ptrdiff_t(prefix.size()), section.end(),
                       [](unsigned char c) { return std::isdigit(c); });
}

// Paths are written in quotes.
bool QuotedKey(const std::string& key)
{
    return key == "game.disc" || key == "game.folder" || key == "game.save_folder" || key == "game.gamecube_disc"
        || key == "mods.texture_folder";
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
                // Retired keys, accepted in old INIs: the title-bar counter, a
                // dead zone nothing read (gamepad profiles have their own, #62),
                // a 1.0.12 test build's Wii Remote speaker format (#60) and No
                // HUD's tweak key, now hotkeys.show_hud.
                drop = updated && ((OwnedSection(section) && !kept.count(full_key))
                    || full_key == "display.show_fps" || full_key == "controls.deadzone"
                    || full_key == "controls.speaker_format" || full_key == "gameplay.no_hud");
                if (const auto it = replacements.find(full_key); it != replacements.end())
                {
                    if (it->second != value)
                        line = line.substr(0, equals + 1) + " "
                            + (QuotedKey(full_key) ? "\"" + it->second + "\"" : it->second);
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
                + (QuotedKey(full_key) ? "\"" + value + "\"" : value) + newline;
        }
    }
    return values;
}

std::string Read(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    if (!input)
        throw std::runtime_error("Cannot read " + PathUtf8(path)
            + ". Copy recharged.ini.example to recharged.ini and set [game] disc.");
    if (std::filesystem::file_size(path) > 1024 * 1024)
        throw std::runtime_error("Configuration file is unexpectedly large: " + PathUtf8(path));
    std::ostringstream result;
    result << input.rdbuf();
    if (input.bad()) throw std::runtime_error("Failed to read configuration: " + PathUtf8(path));
    return result.str();
}
}

std::optional<std::array<int, 2>> ParseWindowPosition(std::string_view text)
{
    const auto comma = text.find(',');
    if (comma == std::string_view::npos) return std::nullopt;
    std::array<int, 2> position{};
    for (int n = 0; n < 2; ++n)
    {
        const auto part = Trim(std::string(n == 0 ? text.substr(0, comma) : text.substr(comma + 1)));
        const auto result = std::from_chars(part.data(), part.data() + part.size(), position[std::size_t(n)]);
        if (part.empty() || result.ec != std::errc{} || result.ptr != part.data() + part.size()
            || position[std::size_t(n)] < -32768 || position[std::size_t(n)] > 32767)
            return std::nullopt;
    }
    return position;
}

std::string FormatWindowPosition(int x, int y)
{
    return std::to_string(x) + "," + std::to_string(y);
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
        throw std::runtime_error("Set [game] disc to an ISO, RVZ or WBFS path in " + PathUtf8(config));
    const auto path = PathFromUtf8(settings.disc);
    return path.is_absolute() ? path : config.parent_path() / path;
}

namespace
{
constexpr const char* kDataFolder = "data";
constexpr const char* kOldDataFolder = "original-main-credits-data";
}

std::filesystem::path DataFolder(const std::filesystem::path& executable_directory)
{
    auto folder = executable_directory / kDataFolder;
    auto old = executable_directory / kOldDataFolder;
    std::error_code error;
    if (!std::filesystem::exists(folder, error) && std::filesystem::is_directory(old, error)) return old;
    return folder;
}

bool MoveOldDataFolder(const std::filesystem::path& executable_directory)
{
    const auto folder = executable_directory / kDataFolder;
    const auto old = executable_directory / kOldDataFolder;
    std::error_code error;
    if (std::filesystem::exists(folder, error) || !std::filesystem::is_directory(old, error)) return false;
    std::filesystem::rename(old, folder, error);
    return !error;
}

std::filesystem::path DefaultSaveFolder(const std::filesystem::path& executable_directory)
{
    return DataFolder(executable_directory) / "nand";
}

std::filesystem::path ResolveGameCubeDiscPath(const Settings& settings, const std::filesystem::path& config)
{
    if (settings.gamecube_disc.empty()) return {};
    const auto path = PathFromUtf8(settings.gamecube_disc);
    return path.is_absolute() ? path : config.parent_path() / path;
}

std::filesystem::path ResolveSaveFolder(const Settings& settings, const std::filesystem::path& config,
                                        const std::filesystem::path& executable_directory)
{
    if (settings.save_folder.empty()) return DefaultSaveFolder(executable_directory);
    const auto path = PathFromUtf8(settings.save_folder);
    return path.is_absolute() ? path : config.parent_path() / path;
}

std::filesystem::path ResolveGameFolder(const Settings& settings, const std::filesystem::path& config)
{
    const auto path = PathFromUtf8(settings.folder.empty() ? "game" : settings.folder);
    return path.is_absolute() ? path : config.parent_path() / path;
}

std::filesystem::path ResolveTextureFolder(const Settings& settings, const std::filesystem::path& config)
{
    const auto path = PathFromUtf8(settings.texture_folder.empty() ? "texture_packs" : settings.texture_folder);
    return path.is_absolute() ? path : config.parent_path() / path;
}

std::filesystem::path LoadDiscPath(const std::filesystem::path& config)
{
    const auto file = LoadConfig(config);
    return ResolveDiscPath(file.settings, file.path);
}
}
