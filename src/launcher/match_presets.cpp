#include "launcher/match_presets.h"

#include "platform/path.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <system_error>

namespace mscharged::launcher
{
namespace
{
namespace fs = std::filesystem;
using platform::MatchSettings;

std::string Lower(std::string text)
{
    for (auto& c : text) c = char(std::tolower(static_cast<unsigned char>(c)));
    return text;
}

std::string Trim(std::string_view text)
{
    const auto begin = text.find_first_not_of(" \t\r");
    if (begin == std::string_view::npos) return {};
    return std::string(text.substr(begin, text.find_last_not_of(" \t\r") - begin + 1));
}

// The values' names in a preset file: the enums' in lower case
// (ENV_CHEAT_WHITE_BALL white_ball; the skills as the settings screen's
// slides). They stay when the page's labels change.
const std::vector<const char*> kSkillTokens{"rookie", "professional", "superstar", "legend", "megastriker"};
const std::vector<const char*> kStadiumTokens{"none", "secure", "power", "voltage", "tilt", "white_ball"};
const std::vector<const char*> kPlayerTokens{"none", "devastating", "safe", "skill_shot", "glass_jaw"};
const std::vector<const char*> kPowerupTokens{"none",       "explosives", "freezing",  "shells",
                                              "giant",      "accelerator", "peelin_out", "heat_seekers",
                                              "bombs_away", "super",       "infinite",   "butterfingers"};

std::optional<int> FindToken(const std::vector<const char*>& tokens, const std::string& value)
{
    for (std::size_t n = 0; n < tokens.size(); ++n)
        if (value == tokens[n]) return int(n);
    return std::nullopt;
}

std::optional<int> Number(const std::string& value)
{
    if (value.empty() || value.size() > 3
        || !std::all_of(value.begin(), value.end(), [](unsigned char c) { return std::isdigit(c); }))
        return std::nullopt;
    return std::stoi(value);
}

const char* NameOr(const std::vector<const char*>& names, int index)
{
    return index >= 0 && std::size_t(index) < names.size() ? names[std::size_t(index)] : nullptr;
}

std::string Quoted(const std::string& text) { return '"' + text + '"'; }
} // namespace

const std::vector<MatchPreset>& BuiltInMatchPresets()
{
    static const std::vector<MatchPreset> presets = [] {
        MatchSettings competitive;
        competitive.skill = 3;
        competitive.best_of = 3;
        competitive.goal_limit = true;
        competitive.goals = 10;
        competitive.stadium_cheat = 1;
        return std::vector<MatchPreset>{{"Default", MatchSettings{}, true, {}}, {"Competitive", competitive, true, {}}};
    }();
    return presets;
}

bool SameMatchPresetName(const std::string& a, const std::string& b) { return Lower(a) == Lower(b); }

bool ValidMatchPresetName(const std::string& name)
{
    if (name.empty() || name.size() > kMaxMatchPresetName || name != Trim(name)) return false;
    for (const auto& preset : BuiltInMatchPresets())
        if (SameMatchPresetName(name, preset.name)) return false;
    return std::none_of(name.begin(), name.end(), [](unsigned char c) {
        return c < 0x20 || c == 0x7f || c == '"' || c == '[' || c == ']' || c == '=' || c == '|';
    });
}

const std::vector<const char*>& SkillNames()
{
    static const std::vector<const char*> names{"Rookie", "Professional", "Superstar", "Legend", "Megastriker"};
    return names;
}

const std::vector<const char*>& StadiumCheatNames()
{
    static const std::vector<const char*> names{"None",       "Secure Stadiums", "Power Shortage",
                                                "High Voltage", "Field Tilt",    "Always White Ball"};
    return names;
}

const std::vector<const char*>& PlayerCheatNames()
{
    static const std::vector<const char*> names{"None", "Devastating Hits", "Safe Megastrike",
                                                "Always Sidekick Skillshot", "Classic Mode"};
    return names;
}

const std::vector<const char*>& PowerupCheatNames()
{
    static const std::vector<const char*> names{"None",         "Explosives",     "Freezing",   "Shells",
                                                "Giant",        "Accelerator",    "Peelin' Out", "Seek and Destroy",
                                                "Bombs Away",   "Super Captains", "Infinite",   "No Power-Ups"};
    return names;
}

std::string MatchSummary(const MatchSettings& s)
{
    std::ostringstream text;
    const char* skill = NameOr(SkillNames(), s.skill - 1);
    if (skill) text << skill;
    else text << "skill " << s.skill;
    text << ", best of " << s.best_of << ", ";
    if (s.goal_limit) text << "first to " << s.goals << " goals";
    else if (s.time % 60 == 0) text << s.time / 60 << " minutes";
    else text << s.time << " seconds";
    using Cheat = std::pair<const std::vector<const char*>*, int>;
    for (const auto& [names, cheat] : {Cheat{&StadiumCheatNames(), s.stadium_cheat},
                                       Cheat{&PlayerCheatNames(), s.player_cheat},
                                       Cheat{&PowerupCheatNames(), s.powerup_cheat}})
    {
        if (cheat == 0) continue;
        const char* name = NameOr(*names, cheat);
        text << ", " << (name ? name : "cheat " + std::to_string(cheat));
    }
    return text.str();
}

std::string MatchPresetText(const MatchPreset& preset)
{
    const auto& s = preset.settings;
    std::ostringstream text;
    text << "# Recharged match preset, from the launcher's Matches page. Settings left out\n"
         << "# are a new save's.\n"
         << "name = " << Quoted(preset.name) << '\n'
         << "skill = " << kSkillTokens.at(std::size_t(s.skill - 1)) << '\n'
         << "best_of = " << s.best_of << '\n'
         << "win_by = " << (s.goal_limit ? "goals" : "time") << '\n'
         << "minutes = " << s.time / 60 << '\n'
         << "goals = " << s.goals << '\n'
         << "stadium_cheat = " << kStadiumTokens.at(std::size_t(s.stadium_cheat)) << '\n'
         << "player_cheat = " << kPlayerTokens.at(std::size_t(s.player_cheat)) << '\n'
         << "powerup_cheat = " << kPowerupTokens.at(std::size_t(s.powerup_cheat)) << '\n';
    return text.str();
}

std::optional<MatchPreset> ParseMatchPreset(std::string_view text, std::string* error)
{
    MatchPreset preset;
    bool named = false;
    auto fail = [&](int line, const std::string& why) -> std::optional<MatchPreset> {
        if (error) *error = (line ? "line " + std::to_string(line) + ": " : std::string()) + why;
        return std::nullopt;
    };
    std::istringstream input{std::string(text)};
    std::string raw;
    for (int line = 1; std::getline(input, raw); ++line)
    {
        const auto content = Trim(raw);
        if (content.empty() || content[0] == '#' || content[0] == ';') continue;
        const auto equals = content.find('=');
        if (equals == std::string::npos) return fail(line, "no \"key = value\"");
        const auto key = Lower(Trim(std::string_view(content).substr(0, equals)));
        auto value = Trim(std::string_view(content).substr(equals + 1));
        // A quoted value ends at its quote; any other at a # or ; comment.
        if (!value.empty() && value.front() == '"')
        {
            const auto close = value.find('"', 1);
            if (close == std::string::npos) return fail(line, "no closing quote");
            value = value.substr(1, close - 1);
        }
        else value = Trim(value.substr(0, value.find_first_of("#;")));
        if (key == "name")
        {
            if (!ValidMatchPresetName(value)) return fail(line, "\"" + value + "\" cannot be a preset's name");
            preset.name = value;
            named = true;
            continue;
        }
        value = Lower(value);
        auto& s = preset.settings;
        std::optional<int> number;
        if (key == "skill" && (number = FindToken(kSkillTokens, value))) s.skill = *number + 1;
        else if (key == "best_of" && (number = Number(value))) s.best_of = *number;
        else if (key == "win_by" && (value == "time" || value == "goals")) s.goal_limit = value == "goals";
        else if (key == "minutes" && (number = Number(value))) s.time = *number * 60;
        else if (key == "goals" && (number = Number(value))) s.goals = *number;
        else if (key == "stadium_cheat" && (number = FindToken(kStadiumTokens, value))) s.stadium_cheat = *number;
        else if (key == "player_cheat" && (number = FindToken(kPlayerTokens, value))) s.player_cheat = *number;
        else if (key == "powerup_cheat" && (number = FindToken(kPowerupTokens, value))) s.powerup_cheat = *number;
        else if (key == "skill" || key == "best_of" || key == "win_by" || key == "minutes" || key == "goals"
                 || key == "stadium_cheat" || key == "player_cheat" || key == "powerup_cheat")
            return fail(line, key + " cannot be " + value);
    }
    if (!named) return fail(0, "no name");
    if (!platform::ValidMatchSettings(preset.settings)) return fail(0, "a setting the game does not offer");
    return preset;
}

std::string MatchPresetFileName(const std::string& name)
{
    std::string stem;
    for (const unsigned char c : name)
    {
        if (std::isalnum(c) && c < 0x80) stem += char(std::tolower(c));
        else if (!stem.empty() && stem.back() != '-') stem += '-';
    }
    while (!stem.empty() && stem.back() == '-') stem.pop_back();
    return (stem.empty() ? "preset" : stem) + ".ini";
}

MatchPresetList LoadMatchPresets(const fs::path& folder)
{
    MatchPresetList list;
    list.presets = BuiltInMatchPresets();
    std::vector<MatchPreset> found;
    std::error_code error;
    for (fs::directory_iterator it(folder, error), end; !error && it != end; it.increment(error))
    {
        const auto& path = it->path();
        if (Lower(PathUtf8(path.extension())) != ".ini" || !it->is_regular_file(error)) continue;
        const auto file = PathUtf8(path.filename());
        std::ifstream input(path, std::ios::binary);
        std::ostringstream bytes;
        bytes << input.rdbuf();
        std::string why;
        auto preset = input ? ParseMatchPreset(bytes.str(), &why) : std::nullopt;
        if (!input) why = "it cannot be read";
        else if (preset && std::any_of(found.begin(), found.end(), [&](const MatchPreset& other) {
                     return SameMatchPresetName(other.name, preset->name);
                 }))
        {
            why = "another file has the preset " + preset->name;
            preset.reset();
        }
        if (!preset)
        {
            list.skipped.push_back(file + ": " + why);
            continue;
        }
        preset->file = path;
        found.push_back(std::move(*preset));
    }
    std::sort(found.begin(), found.end(),
              [](const MatchPreset& a, const MatchPreset& b) { return Lower(a.name) < Lower(b.name); });
    list.presets.insert(list.presets.end(), found.begin(), found.end());
    return list;
}

fs::path SaveMatchPreset(const fs::path& folder, const MatchPreset& preset, const std::vector<MatchPreset>& others)
{
    if (preset.built_in) throw std::invalid_argument("The built-in presets cannot change.");
    if (!ValidMatchPresetName(preset.name)) throw std::invalid_argument("\"" + preset.name + "\" cannot be a preset's name.");
    if (!platform::ValidMatchSettings(preset.settings)) throw std::invalid_argument("A setting the game does not offer.");
    for (const auto& other : others)
        if (!other.built_in && other.file != preset.file && SameMatchPresetName(other.name, preset.name))
            throw std::invalid_argument("Another preset is called " + other.name + ".");

    const auto target = folder / PathFromUtf8(MatchPresetFileName(preset.name));
    std::error_code error;
    const bool renamed = !preset.file.empty() && !fs::equivalent(preset.file, target, error);
    if ((preset.file.empty() || renamed) && fs::exists(target, error))
        throw std::invalid_argument("Another file is called " + PathUtf8(target.filename()) + ": rename the preset.");

    try
    {
        fs::create_directories(folder);
        // Through a staging file, so a failed write leaves the old file whole.
        auto staging = target;
        staging += ".new";
        {
            std::ofstream output(staging, std::ios::binary | std::ios::trunc);
            output << MatchPresetText(preset);
            output.close();
            if (!output) throw std::runtime_error("Cannot write " + PathUtf8(staging));
        }
        fs::rename(staging, target);
        if (renamed) fs::remove(preset.file);
    }
    catch (const fs::filesystem_error& e)
    {
        throw std::runtime_error("Cannot save the preset: " + std::string(e.what()));
    }
    return target;
}

void DeleteMatchPreset(const MatchPreset& preset)
{
    if (preset.built_in || preset.file.empty()) throw std::invalid_argument("The built-in presets cannot be removed.");
    std::error_code error;
    if (!fs::remove(preset.file, error) && error)
        throw std::runtime_error("Cannot remove " + PathUtf8(preset.file) + ": " + error.message());
}

const MatchPreset* FindMatchPreset(const std::vector<MatchPreset>& presets, const MatchSettings& settings)
{
    for (const auto& preset : presets)
        if (preset.settings == settings) return &preset;
    return nullptr;
}
} // namespace mscharged::launcher
