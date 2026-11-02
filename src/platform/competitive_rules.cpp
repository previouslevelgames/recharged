#include "platform/competitive_rules.h"

#include "bootstrap/game_tweaks.h"
#include "platform/profile_service.h"

#include <array>
#include <cctype>

namespace mscharged::platform
{
namespace
{
// eSkillLevel order.
constexpr std::array<const char*, 5> kSkills{"training", "rookie", "professional", "superstar", "legend"};
// eStadiumID order, without the tutorial field and the low-poly stadium.
constexpr std::array<const char*, 16> kStadiums{
    "battle_dome",      "bowser_stadium", "crater_field",    "crystal_canyon", "the_dump",  "galactic_stadium",
    "konga_coliseum",   "lava_pit",       "pipeline_central", "storm_ship",    "the_palace", "thunder_island",
    "the_underground",  "the_vice",       "the_wastelands",  "sand_tomb"};
// eTeamID order.
constexpr std::array<const char*, 12> kCaptains{"mario", "bowser", "daisy", "donkey_kong", "luigi", "peach",
                                                "waluigi", "wario", "yoshi", "bowser_jr", "diddy_kong", "petey"};
// The tweaks a rule set may switch on (the server's TWEAKS).
constexpr std::array<GameTweak, 4> kRuleTweaks{GameTweak::FastTerrain, GameTweak::NoMegaStrikes,
                                               GameTweak::ThunderIslandWalls, GameTweak::MirrorMatch};

template <std::size_t N> int IndexOf(const std::array<const char*, N>& names, const std::string& name)
{
    for (std::size_t n = 0; n < N; ++n)
        if (name == names[n]) return int(n);
    return -1;
}

const GameTweakInfo* RuleTweak(const std::string& name)
{
    for (const auto tweak : kRuleTweaks)
        for (const auto& info : kGameTweaks)
            if (info.tweak == tweak && name == info.key) return &info;
    return nullptr;
}

// The cheat's place in its list (its enum value), -1 for one the list hasn't.
int CheatIndex(const std::vector<RuleChoice>& choices, const std::string& key)
{
    const std::string name = key.empty() ? "none" : key;
    for (std::size_t n = 0; n < choices.size(); ++n)
        if (name == choices[n].key) return int(n);
    return -1;
}
} // namespace

const std::vector<RuleChoice>& StadiumChoices()
{
    static const std::vector<RuleChoice> choices = [] {
        static std::vector<std::string> labels;
        std::vector<RuleChoice> list{{"random", "Random"}};
        labels.reserve(kStadiums.size());
        for (const char* key : kStadiums) labels.push_back(NameLabel(key));
        for (std::size_t n = 0; n < kStadiums.size(); ++n) list.push_back({kStadiums[n], labels[n].c_str()});
        return list;
    }();
    return choices;
}

// The names the game's settings screen shows (the launcher's presets too).
const std::vector<RuleChoice>& StadiumCheatChoices()
{
    static const std::vector<RuleChoice> choices{{"none", "None"},
                                                 {"secure_stadiums", "Secure Stadiums"},
                                                 {"power_shortage", "Power Shortage"},
                                                 {"high_voltage", "High Voltage"},
                                                 {"field_tilt", "Field Tilt"},
                                                 {"always_white_ball", "Always White Ball"}};
    return choices;
}

const std::vector<RuleChoice>& PlayerCheatChoices()
{
    static const std::vector<RuleChoice> choices{{"none", "None"},
                                                 {"devastating_hits", "Devastating Hits"},
                                                 {"safe_megastrike", "Safe Megastrike"},
                                                 {"always_sidekick_skillshot", "Always Sidekick Skillshot"},
                                                 {"classic_mode", "Classic Mode"}};
    return choices;
}

const std::vector<RuleChoice>& PowerupCheatChoices()
{
    static const std::vector<RuleChoice> choices{
        {"none", "None"},           {"explosives", "Explosives"},     {"freezing", "Freezing"},
        {"shells", "Shells"},       {"giant", "Giant"},               {"accelerator", "Accelerator"},
        {"peelin_out", "Peelin' Out"}, {"seek_and_destroy", "Seek and Destroy"}, {"bombs_away", "Bombs Away"},
        {"super_captains", "Super Captains"}, {"infinite", "Infinite"}, {"no_power_ups", "No Power-Ups"}};
    return choices;
}

const std::vector<RuleChoice>& RuleTweakChoices()
{
    static const std::vector<RuleChoice> choices = [] {
        std::vector<RuleChoice> list;
        for (const auto tweak : kRuleTweaks)
            for (const auto& info : kGameTweaks)
                if (info.tweak == tweak) list.push_back({info.key, info.name});
        return list;
    }();
    return choices;
}

std::optional<ChargedCompetitiveRules> ModuleRules(const MatchRules& rules, const ProfilePlayer& opponent,
                                                   std::string* error)
{
    auto fail = [error](const std::string& message) -> std::optional<ChargedCompetitiveRules> {
        if (error) *error = message;
        return std::nullopt;
    };
    ChargedCompetitiveRules module{};
    for (const auto& name : rules.tweaks)
    {
        const auto* tweak = RuleTweak(name);
        if (!tweak) return fail("This build does not know the rule " + name + "; update Recharged");
        module.tweaks |= GameTweakBit(tweak->tweak);
    }
    module.skill = IndexOf(kSkills, rules.skill);
    if (module.skill < 0) return fail("This build does not know the skill level " + rules.skill);
    if (rules.limit != "time" && rules.limit != "goals") return fail("This build does not know the limit " + rules.limit);
    module.limit = rules.limit == "goals" ? 1 : 0;
    if (rules.time < 60 || rules.time > 900 || rules.goals < 1 || rules.goals > 10)
        return fail("The match's length is out of range");
    module.time = rules.time;
    module.goals = rules.goals;
    if (rules.games != 1 && rules.games != 3 && rules.games != 5 && rules.games != 7)
        return fail("The match's series is out of range");
    module.games = rules.games;
    module.stadium = rules.stadium == "random" ? -1 : IndexOf(kStadiums, rules.stadium);
    if (rules.stadium != "random" && module.stadium < 0) return fail("This build does not know the stadium " + rules.stadium);
    module.stadium_cheat = CheatIndex(StadiumCheatChoices(), rules.stadium_cheat);
    module.player_cheat = CheatIndex(PlayerCheatChoices(), rules.player_cheat);
    module.powerup_cheat = CheatIndex(PowerupCheatChoices(), rules.powerup_cheat);
    if (module.stadium_cheat < 0 || module.player_cheat < 0 || module.powerup_cheat < 0)
        return fail("This build does not know one of the match's cheats; update Recharged");
    for (const auto& profile : opponent.wfc)
        if (module.opponent_count < int(std::size(module.opponents)) && profile.profile_id > 0
            && profile.profile_id <= 0xFFFFFFFFll)
            module.opponents[module.opponent_count++] = unsigned(profile.profile_id);
    return module;
}

std::string NameLabel(std::string_view name)
{
    std::string label;
    bool word = true;
    for (const char c : name)
    {
        if (c == '_')
        {
            label += ' ';
            word = true;
            continue;
        }
        label += word ? char(std::toupper(static_cast<unsigned char>(c))) : c;
        word = false;
    }
    return label;
}

std::string DescribeRules(const MatchRules& rules)
{
    std::string text;
    if (rules.limit == "goals")
        text += "First to " + std::to_string(rules.goals) + (rules.goals == 1 ? " goal" : " goals");
    else if (rules.time % 60 == 0)
        text += std::to_string(rules.time / 60) + (rules.time == 60 ? " minute" : " minutes");
    else
        text += std::to_string(rules.time / 60) + ":" + (rules.time % 60 < 10 ? "0" : "")
            + std::to_string(rules.time % 60) + " minutes";
    text += rules.games == 1 ? ", one game" : ", best of " + std::to_string(rules.games);
    text += rules.stadium == "random" ? std::string(", random stadium") : ", " + NameLabel(rules.stadium);
    for (const auto& name : rules.tweaks)
    {
        const auto* tweak = RuleTweak(name);
        text += ", ";
        text += tweak ? tweak->name : name;
    }
    for (const auto& [choices, key] : {std::pair{&StadiumCheatChoices(), &rules.stadium_cheat},
                                       std::pair{&PlayerCheatChoices(), &rules.player_cheat},
                                       std::pair{&PowerupCheatChoices(), &rules.powerup_cheat}})
        if (const int index = CheatIndex(*choices, *key); index > 0)
            text += ", " + std::string((*choices)[std::size_t(index)].label);
        else if (index < 0)
            text += ", " + NameLabel(*key);
    return text;
}

std::u16string ScreenName(std::string_view name)
{
    constexpr std::size_t kUnits = 32;
    std::u16string shown;
    for (std::size_t n = 0; n < name.size();)
    {
        const auto lead = static_cast<unsigned char>(name[n]);
        const std::size_t length = lead < 0x80 ? 1 : (lead & 0xE0) == 0xC0 ? 2 : (lead & 0xF0) == 0xE0 ? 3
                                 : (lead & 0xF8) == 0xF0                   ? 4
                                                                           : 0;
        if (!length || n + length > name.size()) return {};
        char32_t c = length == 1 ? lead : lead & (0x7F >> length);
        for (std::size_t k = 1; k < length; ++k)
        {
            const auto next = static_cast<unsigned char>(name[n + k]);
            if ((next & 0xC0) != 0x80) return {};
            c = (c << 6) | (next & 0x3F);
        }
        n += length;
        const bool space = c == U' ' || c == U'\t' || c == U'\u00A0' || c == U'\u3000';
        // The basic plane without control, surrogate, private-use and
        // invisible format characters (and variation selectors).
        const bool shows = c >= 0x20 && !(c >= 0x7F && c < 0xA0) && c <= 0xFFFF && !(c >= 0xD800 && c < 0xF900)
                        && c != 0xAD && !(c >= 0x200B && c <= 0x200F) && !(c >= 0x2028 && c <= 0x202E)
                        && !(c >= 0x2060 && c <= 0x206F) && !(c >= 0xFE00 && c <= 0xFE0F) && c != 0xFEFF
                        && c < 0xFFF0;
        if (space)
        {
            if (!shown.empty() && shown.back() != u' ') shown.push_back(u' ');
        }
        else if (shows)
            shown.push_back(char16_t(c));
        if (shown.size() >= kUnits) break;
    }
    if (shown.size() > kUnits) shown.resize(kUnits);
    while (!shown.empty() && shown.back() == u' ') shown.pop_back();
    return shown;
}

std::string CaptainKey(int team)
{
    return team >= 0 && team < int(kCaptains.size()) ? kCaptains[std::size_t(team)] : "";
}

std::string StadiumKey(int stadium)
{
    return stadium >= 0 && stadium < int(kStadiums.size()) ? kStadiums[std::size_t(stadium)] : "";
}
} // namespace mscharged::platform
