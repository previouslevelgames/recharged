#pragma once

#include "platform/presence_snapshot.h"

namespace mscharged::platform
{
// The kind of match the game is in, as the profile service's lists show it
// (the Recharged-Match header, recharged-wfc-srv docs/profiles.md,
// "Activity"); nullptr outside a match and in the title screen's demo.
inline const char* MatchKindOf(const ChargedPresenceSnapshot& s)
{
    // GameInfoManager::eGameMode, as ChargedPresenceSnapshot::mode has it.
    constexpr int kOnlineTournament = 1, kDemo = 2, kCup = 3, kChallenge = 4;
    if (!s.in_match || s.mode == kDemo) return nullptr;
    if (s.strikers101) return "training";
    if (s.online) return s.ranked ? "ranked" : s.mode == kOnlineTournament ? "tournament" : "friends";
    if (s.mode == kCup) return "cup";
    if (s.mode == kChallenge) return "challenge";
    // Domination: players on both teams at this computer, or the computer
    // against one team (or both).
    return s.human_sides == 3 ? "local" : "cpu";
}
} // namespace mscharged::platform
