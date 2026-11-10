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
    // The game's cheats: eEnvironmentCheat, ePlayerCheat, ePowerupCheat (0 none).
    int stadium_cheat;
    int player_cheat;
    int powerup_cheat;
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
    // A set's game (charged_configure_competitive_start): where its start is
    // (ChargedSetStage), and the popup that ended it (ePopupMenu, -1 none).
    int set_stage;
    int set_popup;
    unsigned competitive_setups;
    int competitive_games;
    // This machine's team in the online match: 0 home, 1 away, -1 unknown.
    // Online every machine's players have a side, so the presence snapshot's
    // human_sides has both.
    int local_side;
    // A set's game passes its menus unseen (tweak patch 0057): the host holds
    // the players' buttons meanwhile, so nobody steps into a screen they
    // cannot see. The ranked walk too.
    int set_unseen;
    // The ranked walk (charged_start_ranked): where it is (ChargedSetStage,
    // from BOOT to LOGIN; NONE once the RANKED search began or without one).
    int ranked_stage;
};

// How far a set's game got: booting (title, main menu), choosing the Mii and
// logging in, searching RANKED for the opponent, playing; over (back from the
// game), failed (a popup that is not harmless; set_popup says which) or left
// (the player's Back left the search).
enum ChargedSetStage
{
    CHARGED_SET_NONE,
    CHARGED_SET_BOOT,
    CHARGED_SET_MII,
    CHARGED_SET_OVERLAY,
    CHARGED_SET_LOGIN,
    CHARGED_SET_SEARCH,
    CHARGED_SET_PLAYING,
    CHARGED_SET_OVER,
    CHARGED_SET_FAILED,
    CHARGED_SET_LEFT,
};

#ifdef __cplusplus
extern "C" {
#endif
// Game module entry, before original main or between frames: this game is a
// set's, with the player's own Mii (`mii`: its 8-byte create ID; false when
// `size` is not 8).
typedef bool (*ChargedConfigureCompetitiveStartFunction)(const unsigned char* mii, unsigned size);
// Game module entry, between frames: the set is done and the host has
// finished with it; the game's popups and menus are the player's again.
typedef void (*ChargedCompetitiveEndSetFunction)(void);
// Game module entry, between frames: the Recharged Game Menu's Play RANKED.
// The game goes by itself into a retail RANKED search against anybody, as
// a set's game does into its set's: from the title, the main menu or the
// online menu, whichever comes up next. `mii` is the Mii that plays (its
// 8-byte create ID), or null for the player to choose it on the online Mii
// menu. False while a set's game starts, or for another `size`.
typedef bool (*ChargedStartRankedFunction)(const unsigned char* mii, unsigned size);
// Game module entry, between frames: the ranked walk stops where it is; the
// screens are the player's again.
typedef void (*ChargedStopRankedFunction)(void);
// Game module entry, between frames: the set's players' profile names
// (UTF-16, ScreenName) for its RANKED screens, this player's and the
// opponent's (null or empty: the Mii name stays); both null: none.
typedef void (*ChargedConfigureDisplayNamesFunction)(const unsigned short* own, const unsigned short* opponent);
// Game module entry ([advanced] log_level = debug): DWC's reports of the WFC
// login, matchmaking and NAT negotiation go to the game's output (the log).
typedef void (*ChargedConfigureDwcReportFunction)(bool on);
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
#include <vector>

namespace mscharged::platform
{
struct MatchRules;
struct ProfilePlayer;

// The module's rules for a match against `opponent`, or nullopt with `error`
// naming a rule this build does not know (the server is newer).
std::optional<ChargedCompetitiveRules> ModuleRules(const MatchRules& rules, const ProfilePlayer& opponent,
                                                   std::string* error = nullptr);
// "3 minutes, best of 3, random stadium, No Mega Strikes, Field Tilt" (the
// skill only matters against the CPU, so it isn't said).
std::string DescribeRules(const MatchRules& rules);
// What a rule offers: the server's key and the name players see, in the
// game's order (the cheats' first is "none").
struct RuleChoice
{
    const char* key;
    const char* label;
};
const std::vector<RuleChoice>& StadiumChoices(); // "random" first
const std::vector<RuleChoice>& StadiumCheatChoices();
const std::vector<RuleChoice>& PlayerCheatChoices();
const std::vector<RuleChoice>& PowerupCheatChoices();
// The name players see for `key` among `choices` (the key itself for one not
// there).
std::string RuleChoiceLabel(const std::vector<RuleChoice>& choices, const std::string& key);
// The key `step` places from `key` among `choices`, round at the ends (the
// first for a key not there).
std::string StepRuleChoice(const std::vector<RuleChoice>& choices, const std::string& key, int step);
// The tweaks a set's rules may switch on (fast_terrain, no_mega_strikes,
// thunder_island_walls, mirror_match), with their names.
const std::vector<RuleChoice>& RuleTweakChoices();
// The server's names (lowercase, '_' between words): eTeamID and eStadiumID,
// empty for unknown values.
std::string CaptainKey(int team);
std::string StadiumKey(int stadium);
// "the_palace" as shown: "The Palace".
std::string NameLabel(std::string_view name);
// A profile name as the game's screens can show it: UTF-16 of the basic
// plane without control, private-use and invisible characters (emoji and
// other characters outside it are left out), runs of spaces as one, at most
// 32 units; empty for none (or invalid UTF-8).
std::u16string ScreenName(std::string_view name);
} // namespace mscharged::platform
#endif
