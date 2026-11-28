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
    OverrideRetailMenus = 15,
};

struct GameTweakInfo
{
    GameTweak tweak;
    const char* key;         // [gameplay] key
    const char* name;        // launcher label and log name
    const char* description; // launcher description
    bool on_by_default = false; // recommended to every player: on without its [gameplay] key
};

inline constexpr std::array<GameTweakInfo, 15> kGameTweaks{{
    {GameTweak::SkipIntro, "skip_intro", "Skip intro", "Starts at the main menu, without the intro screens and movie."},
    {GameTweak::FastMenus, "fast_menus", "Fast menus", "Skips the transitions between menu screens."},
    {GameTweak::NoMegaStrikes, "no_mega_strikes", "No Mega Strikes", "Mega Strikes are off for both teams."},
    {GameTweak::UnlockAll, "unlock_all", "Unlock everything",
     "Unlocks all characters, stadiums and cheats without changing your save."},
    {GameTweak::CpuVsCpu, "cpu_vs_cpu", "CPU vs CPU",
     "The CPU plays both teams when nobody picks a side in Domination."},
    {GameTweak::FastTerrain, "fast_terrain", "Fast terrain",
     "Every stadium plays like the fast, dry field of Bowser Stadium."},
    {GameTweak::MirrorMatch, "mirror_match", "Mirror match",
     "Both teams can pick the same captain, the away team in an away kit."},
    {GameTweak::ThunderIslandWalls, "thunder_island_walls", "Thunder Island walls",
     "Thunder Island's open sides get the electric fence of every other stadium."},
    {GameTweak::BluePeach, "blue_peach", "Blue Peach", "Peach's team wears her unused blue kit on the field."},
    {GameTweak::FairBallContests, "fair_ball_contests", "Defence priority",
     "When both teams reach a loose ball at once, the defending team gets it. Based on Feder's Gecko code for the "
     "Wii.",
     true},
    {GameTweak::RobotGoalies, "robot_goalies", "Robot goalies",
     "Both goalies wear the unused robot Kritter skin from the game's disc."},
    {GameTweak::SuperTeam, "super_team", "Super Team (Work in Progress)",
     "A team's players become Super Team robots from a Super Mario Strikers (GameCube) disc image."},
    {GameTweak::SecureStadiums, "secure_stadiums", "Secure stadiums",
     "No stadium hazards such as lightning, wind or Thwomps in offline matches."},
    {GameTweak::MatchPoint, "match_point", "Match point",
     "Sudden death music plays when both teams are one goal from winning.",
     true},
    {GameTweak::OverrideRetailMenus, "override_retail_menus", "Override retail menus (Dev)",
     "Recharged's own menus replace the game's.",
     true},
}};

// Training ([gameplay] training_item) and the captains and stadiums the
// launcher names (its replays): the config's names; kTrainingItemKinds gives
// each item's ePowerUpType, and the captains and stadiums are in the order
// of eTeamID and eStadiumID.
inline constexpr std::array<const char*, 28> kTrainingItems{
    "none",
    "green_shell", "green_shell_3", "green_shell_5", "giant_green_shell",
    "red_shell", "red_shell_3", "giant_red_shell",
    "spiny_shell", "spiny_shell_3", "spiny_shell_5", "giant_spiny_shell",
    "freeze_shell", "freeze_shell_3", "freeze_shell_5", "giant_freeze_shell",
    "banana", "banana_3", "banana_5", "giant_banana",
    "bobomb", "bobomb_3", "bobomb_5", "giant_bobomb",
    "chain_chomp", "mushroom", "star", "captain"};
// What a Training item gives: the game's ePowerUpType (-1 none; 9 stands for
// the captain's own power-up, whichever captain trains), how many of it (3 or
// 5 where the game awards that many: every shell but the red one also 5, the
// banana and the Bob-omb) and whether a single one is always thrown giant (the
// game makes it medium or giant at random otherwise).
struct TrainingItemKind {
    int type;
    int count;
    bool giant;
};
inline constexpr int kTrainingCaptainItem = 9;
inline constexpr std::array<TrainingItemKind, kTrainingItems.size()> kTrainingItemKinds{{
    {-1, 1, false},
    {0, 1, false}, {0, 3, false}, {0, 5, false}, {0, 1, true},
    {1, 1, false}, {1, 3, false}, {1, 1, true},
    {2, 1, false}, {2, 3, false}, {2, 5, false}, {2, 1, true},
    {3, 1, false}, {3, 3, false}, {3, 5, false}, {3, 1, true},
    {4, 1, false}, {4, 3, false}, {4, 5, false}, {4, 1, true},
    {5, 1, false}, {5, 3, false}, {5, 5, false}, {5, 1, true},
    {6, 1, false}, {7, 1, false}, {8, 1, false}, {kTrainingCaptainItem, 1, false}}};
inline constexpr std::array<const char*, 12> kTrainingCaptains{
    "mario", "bowser", "daisy", "donkeykong", "luigi", "peach", "waluigi", "wario", "yoshi", "bowserjr", "diddykong",
    "petey"};
inline constexpr std::array<const char*, 17> kTrainingStadiums{
    "battledome", "bowserstadium", "craterfield", "crystalcanyon", "dump", "galacticstadium", "kongacoliseum",
    "lavapit", "pipelinecentral", "stormship", "thepalace", "thunderisland", "underground", "vice", "wastelands",
    "sandtomb", "tutorialfield"};
// Training's opponent skill ([gameplay] training_away_skill): the game's skill
// levels in order, 0 (the Strikers 101 lessons') to 5, as its Gameplay options number them.
inline constexpr std::array<const char*, 6> kTrainingSkills{
    "training", "rookie", "professional", "superstar", "legend", "megastriker"};

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
