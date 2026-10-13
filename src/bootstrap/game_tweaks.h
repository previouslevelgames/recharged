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
};

struct GameTweakInfo
{
    GameTweak tweak;
    const char* key;         // [gameplay] key
    const char* name;        // launcher label and log name
    const char* description; // launcher description
};

inline constexpr std::array<GameTweakInfo, 5> kGameTweaks{{
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
