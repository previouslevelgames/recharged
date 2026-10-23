#pragma once

// The rules of a competitive match as the game module applies them
// (charged_configure_competitive), made from a rule set of the profile
// service (platform/profile_service.h), and the names both sides use for
// captains and stadiums in reports. The game's own enums: eSkillLevel,
// eGameLimitType, eStadiumID, eTeamID.

struct ChargedCompetitiveRules
{
    unsigned tweaks; // gameplay tweak bits (GameTweak) both teams play with
    int skill;       // GameplaySettings::eSkillLevel
    int limit;       // GAME_LIMIT_TIME 0, GAME_LIMIT_GOALS 1
    int time;        // seconds
    int goals;
    int games;   // in the series
    int stadium; // eStadiumID, or -1: the host picks one, as in the game
    // The opponent's WFC profiles: the rules apply only to a RANKED match
    // against one of them.
    unsigned opponents[8];
    int opponent_count;
};

// The game's WFC login and competitive state, read by the host after a game
// frame (charged_wfc_snapshot).
struct ChargedWfcSnapshot
{
    int logged_in;         // logged in to WFC (GPCM)
    unsigned profile_id;   // the game's own WFC profile then
    char login_ticket[32]; // GPCM's login ticket (lt) of that login, NUL-terminated
    // The match set up last plays by competitive rules, and how many matches
    // did so since the game started (a new value: another match), and the
    // games of its series.
    int competitive;
    unsigned competitive_setups;
    int competitive_games;
    // This machine's team in the online match: 0 home, 1 away, -1 unknown.
    // Online every machine's players have a side, so the presence snapshot's
    // human_sides has both.
    int local_side;
};

#ifdef __cplusplus
extern "C" {
#endif
// Game module entry: fills `snapshot` when `size` matches this build's
// struct; false otherwise.
typedef bool (*ChargedWfcSnapshotFunction)(ChargedWfcSnapshot* snapshot, unsigned size);
// Game module entry: `rules` for the next RANKED matches against the
// opponent, or null for none (retail RANKED); false when `size` is not this
// build's struct. A match keeps the rules it started with.
typedef bool (*ChargedConfigureCompetitiveFunction)(const ChargedCompetitiveRules* rules, unsigned size);
#ifdef __cplusplus
}

#include <optional>
#include <string>
#include <string_view>

namespace mscharged::platform
{
struct MatchRules;
struct ProfilePlayer;

// The module's rules for a match against `opponent`, or nullopt with `error`
// naming a rule this build does not know (the server is newer).
std::optional<ChargedCompetitiveRules> ModuleRules(const MatchRules& rules, const ProfilePlayer& opponent,
                                                   std::string* error = nullptr);
// "Rookie, 3 minutes, best of 3, random stadium, No Mega Strikes".
std::string DescribeRules(const MatchRules& rules);
// The server's names (lowercase, '_' between words): eTeamID and eStadiumID,
// empty for unknown values.
std::string CaptainKey(int team);
std::string StadiumKey(int stadium);
// "the_palace" as shown: "The Palace".
std::string NameLabel(std::string_view name);
} // namespace mscharged::platform
#endif
