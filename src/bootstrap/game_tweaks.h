#pragma once

// Game tweaks: [gameplay] in recharged.ini. They change the original game on
// purpose, so each is off unless the player turns it on, except the few
// recommended to every player, which are on unless turned off. With all of
// them off the game runs unchanged apart from the series' always-on fixes.
// The hooks are the separate patch series patches/mscharged-decomp-tweaks; the
// game module receives the enabled set as a bit mask (charged_configure_tweaks)
// before original main starts.

#include <array>
#include <cstddef>
#include <string>

namespace mscharged
{
// Bit numbers; they match the hooks' tweak IDs (include/mscharged_tweaks.h in
// the tweak series). Append only. NoHud is no [gameplay] tweak: its bit is
// set by [hotkeys] show_hud = false (Settings::ModuleTweakMask).
enum class GameTweak : unsigned
{
    SkipIntro = 0,
    FastMenus = 1,
    NoMegaStrikes = 2,
    UnlockAll = 3,
    CpuVsCpu = 4,
    FastTerrain = 5,
    MirrorMatch = 6,
    ThunderIslandWalls = 7,
    BluePeach = 8,
    NoHud = 9,
    FairBallContests = 10,
    RobotGoalies = 11,
    SuperTeam = 12,
    SecureStadiums = 13,
    MatchPoint = 14,
};

struct GameTweakInfo
{
    GameTweak tweak;
    const char* key;         // [gameplay] key
    const char* name;        // launcher label and log name
    const char* description; // launcher description
    bool on_by_default = false; // recommended to every player: on without its [gameplay] key
};

inline constexpr std::array<GameTweakInfo, 14> kGameTweaks{{
    {GameTweak::SkipIntro, "skip_intro", "Skip intro",
     "Starts at the main menu: no notice screens, studio logo, intro movie or title screen. Loading still takes as long."},
    {GameTweak::FastMenus, "fast_menus", "Fast menus",
     "Skips menu transitions, also in the pause menu and on the screens after a match. Matches, movies and the "
     "credits are unchanged."},
    {GameTweak::NoMegaStrikes, "no_mega_strikes", "No Mega Strikes",
     "Mega Strikes are off for both teams, the usual competitive rule. Skillshots stay, and Strikers 101 lessons "
     "keep their own rules."},
    {GameTweak::UnlockAll, "unlock_all", "Unlock everything",
     "All characters, stadiums and cheats, through the game's own unlock switch. Your save is not changed; "
     "cup finals then no longer feature the locked captains."},
    {GameTweak::CpuVsCpu, "cpu_vs_cpu", "CPU vs CPU",
     "The CPU plays both teams in Domination if you press Done on Choose Sides without picking a side. Any "
     "controller can still pause, and Choose Sides in the pause menu can hand a match over to the CPU too."},
    {GameTweak::FastTerrain, "fast_terrain", "Fast terrain",
     "Every stadium plays like the fast, dry field of Bowser Stadium: same running speed, ball roll and bounce, "
     "no sliding on ice or mud, no slow sand in the Sand Tomb. Looks and hazards stay; online matches are unchanged."},
    {GameTweak::MirrorMatch, "mirror_match", "Mirror match",
     "Both teams can pick the same captain in Domination. The away team wears an away kit, in the menus too: the "
     "game's own, or for Mario, Luigi, Wario and Waluigi one made from their kit (blue, sky blue, blue, orange). "
     "Online matches are unchanged."},
    {GameTweak::ThunderIslandWalls, "thunder_island_walls", "Thunder Island walls",
     "Thunder Island's open sides get the electric fence of every other stadium: the ball and items bounce off, and "
     "players knocked into it are electrocuted instead of falling off the island. The fence stays invisible; online "
     "matches are unchanged."},
    {GameTweak::BluePeach, "blue_peach", "Blue Peach",
     "Peach's team wears her unused blue and yellow kit on the field: Peach, her sidekicks and her goalie. The game "
     "has no blue menu pictures, Mega Strike hand, banners or crowd for it, so those stay pink."},
    {GameTweak::FairBallContests, "fair_ball_contests", "Fair ball contests",
     "When players of both teams reach a loose ball at once, neither team wins more often. The game updates the "
     "players in an order it shuffles at kickoff, slightly in the home team's favour, and settles one-timers, "
     "first-time shots off rebounds and pass receptions with the home team always first; this shuffles the order "
     "fairly, again every step, and uses it for those too. On by default; online matches always play with it.",
     true},
    {GameTweak::RobotGoalies, "robot_goalies", "Robot goalies",
     "Both goalies wear the robot Kritter skin of the Super Team from Super Mario Strikers, which the game's disc "
     "still holds unused: red lens eyes, metal plating and the robot jersey. Only the look changes."},
    {GameTweak::SuperTeam, "super_team", "Super Team (Work in Progress)",
     "A team's captain and sidekicks become the robots of the Super Team from Super Mario Strikers, fitted to each "
     "character's size. Needs a Super Mario Strikers (GameCube) disc image. Bowser, Petey Piranha and Boo keep "
     "their own look; only the look changes."},
    {GameTweak::SecureStadiums, "secure_stadiums", "Secure stadiums",
     "The game's Secure Stadium cheat in every offline match: no lightning, wind, flying debris, solar flares, "
     "bubbling lava, Storm Ship tilt, Thwomps or sand patches. The electric fences stay. Striker Challenges keep "
     "their rules; online matches are unchanged."},
    {GameTweak::MatchPoint, "match_point", "Match point",
     "In a match to a number of goals, the kickoff after a team gets one goal from winning plays overtime's siren "
     "and sudden death music in place of the stadium's, until the match ends. With both teams one goal from winning, the HUD "
     "shows SUDDEN DEATH instead of First to. Only sound and HUD change. On by default.",
     true},
}};

// Start training ([gameplay] training_item, training_captain, training_stadium): the
// config's names, in the order of the game's ePowerUpType (after none, which is
// -1), eTeamID and eStadiumID.
inline constexpr std::array<const char*, 10> kTrainingItems{
    "none", "green_shell", "red_shell", "spiny_shell", "freeze_shell", "banana", "bobomb", "chain_chomp", "mushroom",
    "star"};
inline constexpr std::array<const char*, 12> kTrainingCaptains{
    "mario", "bowser", "daisy", "donkeykong", "luigi", "peach", "waluigi", "wario", "yoshi", "bowserjr", "diddykong",
    "petey"};
inline constexpr std::array<const char*, 17> kTrainingStadiums{
    "battledome", "bowserstadium", "craterfield", "crystalcanyon", "dump", "galacticstadium", "kongacoliseum",
    "lavapit", "pipelinecentral", "stormship", "thepalace", "thunderisland", "underground", "vice", "wastelands",
    "sandtomb", "tutorialfield"};

// The position of `name` in one of the tables above, or -1.
template <std::size_t Size>
inline int TrainingIndex(const std::array<const char*, Size>& names, const std::string& name)
{
    for (std::size_t n = 0; n < Size; ++n)
        if (name == names[n]) return int(n);
    return -1;
}

// The position of `tweak` in kGameTweaks and Settings::tweaks (kGameTweaks.size()
// for NoHud, which is not listed).
inline constexpr std::size_t GameTweakIndex(GameTweak tweak)
{
    for (std::size_t n = 0; n < kGameTweaks.size(); ++n)
        if (kGameTweaks[n].tweak == tweak) return n;
    return kGameTweaks.size();
}

// The tweaks on without a [gameplay] key.
inline constexpr std::array<bool, kGameTweaks.size()> DefaultGameTweaks()
{
    std::array<bool, kGameTweaks.size()> tweaks{};
    for (std::size_t n = 0; n < kGameTweaks.size(); ++n)
        tweaks[n] = kGameTweaks[n].on_by_default;
    return tweaks;
}

inline constexpr unsigned GameTweakBit(GameTweak tweak) { return 1u << unsigned(tweak); }

// The enabled tweaks' names, comma separated, or "none".
inline std::string DescribeGameTweaks(unsigned mask)
{
    std::string text;
    for (const auto& info : kGameTweaks)
        if (mask & GameTweakBit(info.tweak)) text += (text.empty() ? "" : ", ") + std::string(info.name);
    return text.empty() ? "none" : text;
}
}
