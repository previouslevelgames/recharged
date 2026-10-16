#pragma once

// Opt-in game tweaks: [gameplay] in mscharged.ini. They change the original
// game on purpose, so each is off unless the player turns it on, and with all
// of them off the game runs unchanged apart from the series' always-on fixes.
// The hooks are the separate patch series patches/mscharged-decomp-tweaks; the
// game module receives the enabled set as a bit mask (charged_configure_tweaks)
// before original main starts.

#include <array>
#include <cstddef>
#include <string>

namespace mscharged
{
// Bit numbers; they match the hooks' tweak IDs (include/mscharged_tweaks.h in
// the tweak series). Append only.
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
};

struct GameTweakInfo
{
    GameTweak tweak;
    const char* key;         // [gameplay] key
    const char* name;        // launcher label and log name
    const char* description; // launcher description
};

inline constexpr std::array<GameTweakInfo, 9> kGameTweaks{{
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
}};

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
