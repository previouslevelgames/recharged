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
constexpr std::array<GameTweak, 3> kRuleTweaks{GameTweak::FastTerrain, GameTweak::NoMegaStrikes,
                                               GameTweak::ThunderIslandWalls};

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
} // namespace

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
    std::string text = NameLabel(rules.skill);
    if (rules.limit == "goals")
        text += ", first to " + std::to_string(rules.goals) + (rules.goals == 1 ? " goal" : " goals");
    else if (rules.time % 60 == 0)
        text += ", " + std::to_string(rules.time / 60) + (rules.time == 60 ? " minute" : " minutes");
    else
        text += ", " + std::to_string(rules.time / 60) + ":" + (rules.time % 60 < 10 ? "0" : "")
            + std::to_string(rules.time % 60) + " minutes";
    text += rules.games == 1 ? ", one game" : ", best of " + std::to_string(rules.games);
    text += rules.stadium == "random" ? std::string(", random stadium") : ", " + NameLabel(rules.stadium);
    for (const auto& name : rules.tweaks)
    {
        const auto* tweak = RuleTweak(name);
        text += ", ";
        text += tweak ? tweak->name : name;
    }
    return text;
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
