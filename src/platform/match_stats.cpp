// Match stats for replays (match_stats.h, tweak patch 0085): read from the
// game's StatsTracker and from what it is told, never written to the game.
#if !defined(MSCHARGED_NATIVE) || !defined(MSCHARGED_GAME_MODULE) || !defined(MSCHARGED_TWEAKS)
#error Match stats belong to the isolated original game module built with MSCHARGED_TWEAKS
#endif
#include "platform/match_stats.h"

#include "mscharged_replay.h"
#include "platform/online_match_snapshot.h"

#include "Game/AI/Powerups.h"
#include "Game/Ball.h"
#include "Game/DB/StatsTracker.h"
#include "Game/Field.h"
#include "Game/Game.h"
#include "Game/GameInfo.h"
#include "Game/Player.h"
#include "Game/Task/FixedUpdateTask.h"
#include "Game/Team.h"

#include <cmath>
#include <cstring>

// A set's players' profile names on the sides they play (competitive_match.cpp).
extern "C" bool mscharged_competitive_side_names(unsigned short (*names)[33]);

namespace
{
using namespace mscharged::replay;

MatchStats sStats;
bool sRecording = false;
// The totals read after the last step: the match's end keeps them when the
// game's own read less by then (the next match reset its stats).
std::uint32_t sLast[2][5][kStatKinds];
// The item a player threw last (ThrowPowerup), which its STATS_POWERUPS_USED
// that follows names.
struct Item
{
    int side = -1, player = -1, type = -1, count = 0;
    std::uint32_t frame = 0;
} sItem;

std::uint32_t Frame()
{
    FixedUpdateTask* task = GetFixedUpdateTask();
    return task ? std::uint32_t(task->GetFrame()) : 0;
}

bool Counting() { return sRecording && !mscharged_tweak_replay_playing(); }

void Utf8(const unsigned short* text, char (&out)[kMaxNameBytes])
{
    std::size_t at = 0;
    for (; *text; ++text)
    {
        unsigned code = *text;
        if (code >= 0xD800 && code <= 0xDFFF) code = '?'; // the game's names have no pairs
        char bytes[3];
        std::size_t size = 0;
        if (code < 0x80) bytes[size++] = char(code);
        else if (code < 0x800)
        {
            bytes[size++] = char(0xC0 | code >> 6);
            bytes[size++] = char(0x80 | (code & 0x3F));
        }
        else
        {
            bytes[size++] = char(0xE0 | code >> 12);
            bytes[size++] = char(0x80 | (code >> 6 & 0x3F));
            bytes[size++] = char(0x80 | (code & 0x3F));
        }
        if (at + size >= sizeof out) break;
        std::memcpy(out + at, bytes, size);
        at += size;
    }
    out[at] = 0;
}

// The names of the sides' players: a set's profile names where the host gave
// them, else the drafted Mii names of an online match.
void ReadNames()
{
    if (sStats.name_kinds[0] && sStats.name_kinds[1]) return;
    unsigned short profiles[2][33] = {};
    if (mscharged_competitive_side_names(profiles))
        for (int side = 0; side < 2; ++side)
            if (!sStats.name_kinds[side] && profiles[side][0])
            {
                Utf8(profiles[side], sStats.names[side]);
                sStats.name_kinds[side] = kNameProfile;
            }
    char miis[2][48] = {};
    if (ChargedOnlineSideMiiNames(miis))
        for (int side = 0; side < 2; ++side)
            if (!sStats.name_kinds[side] && miis[side][0])
            {
                std::strncpy(sStats.names[side], miis[side], kMaxNameBytes - 1);
                sStats.name_kinds[side] = kNameMii;
            }
}

void ReadField()
{
    if (sStats.field[0] > 0.0f) return;
    sStats.field[0] = std::fabs(cField::GetGoalLineX(1u));
    sStats.field[1] = std::fabs(cField::GetSidelineY(1u));
    sStats.field[2] = std::fabs(cField::GetPenaltyBoxX(1u));
    sStats.field[3] = std::fabs(cField::GetPenaltyBoxY());
    sStats.field[4] = std::fabs(cField::GetCornerRadius());
}

bool ReadTotals(std::uint32_t (&out)[2][5][kStatKinds])
{
    const StatsTracker* tracker = StatsTracker::s_pInstance;
    if (!tracker) return false;
    for (int side = 0; side < 2; ++side)
        for (int player = 0; player < 5; ++player)
            for (int stat = 0; stat < kStatKinds; ++stat)
            {
                const int value = GetStatValue(tracker->mCurrentPlayerStats[side][player], ePlayerStats(stat));
                out[side][player][stat] = value > 0 ? std::uint32_t(value) : 0;
            }
    return true;
}

// Every total at least what it was after the last step: the same match.
bool NotLess(const std::uint32_t (&now)[2][5][kStatKinds], const std::uint32_t (&before)[2][5][kStatKinds])
{
    for (int side = 0; side < 2; ++side)
        for (int player = 0; player < 5; ++player)
            for (int stat = 0; stat < kStatKinds; ++stat)
                if (now[side][player][stat] < before[side][player][stat]) return false;
    return true;
}

// The match clock (seconds played, overtime on) of a match against the clock.
float MatchClock()
{
    if (!g_pGame || !g_pGame->m_pGameClock) return -1.0f;
    const GameplaySettings* settings = GameInfoManager::Instance()->GetCurrentSettings();
    if (!settings || settings->GameLimitType != GAME_LIMIT_TIME) return -1.0f;
    return g_pGame->GetGameTime();
}

void PlaceOf(int side, int player, StatEvent& event)
{
    if (player < 0 || !g_pTeams[side]) return;
    const cPlayer* character = g_pTeams[side]->GetPlayer(player);
    if (!character) return;
    const nlVector3& at = character->GetPosition();
    event.x = at.x;
    event.y = at.y;
}
} // namespace

namespace mscharged::replay
{
void StatsBegin()
{
    sStats = MatchStats{};
    std::memset(sLast, 0, sizeof sLast);
    sItem = Item{};
    sStats.first_frame = Frame();
    sRecording = true;
    ReadNames();
    ReadField();
}

void StatsStep()
{
    if (!sRecording) return;
    ReadNames();
    ReadField();
    if (ReadTotals(sLast)) std::memcpy(sStats.counts, sLast, sizeof sLast);
}

void StatsRolledBack(std::uint32_t frame)
{
    if (!sRecording) return;
    while (sStats.event_count && sStats.events[sStats.event_count - 1].frame > frame) --sStats.event_count;
}

MatchStats StatsFinish()
{
    if (!sRecording) return MatchStats{};
    sRecording = false;
    // The last step's goal (a golden goal ends the match in its step, after
    // the step's record): the game's totals now, when they are this match's.
    static std::uint32_t now[2][5][kStatKinds];
    if (ReadTotals(now) && NotLess(now, sLast)) std::memcpy(sStats.counts, now, sizeof now);
    ReadNames();
    ReadField();
    sStats.kept = 1;
    return sStats;
}
} // namespace mscharged::replay

extern "C" {
void mscharged_tweak_stats_item(int side, int player, int type, int count)
{
    if (!Counting()) return;
    sItem = Item{side, player, type, count, Frame()};
}

void mscharged_tweak_stats_tracked(int stat, int side, int player, int param0, int param1, int param2, int)
{
    if (!Counting() || side < 0 || side > 1 || sStats.event_count >= kMaxStatEvents) return;
    StatEvent event;
    event.stat = std::uint8_t(stat);
    event.side = std::uint8_t(side);
    event.player = std::int8_t(player >= 0 && player < 5 ? player : -1);
    switch (stat)
    {
    case STATS_GOALS_FOR:
        // The assisting player, the goal type and how many it made (a Mega
        // Strike several); scored into the own net when the shooter plays for
        // the other side (the scorer is then of that side: no place).
        event.other = std::int8_t(param0 >= 0 && param0 < 5 ? param0 : -1);
        event.kind = std::int16_t(param1);
        event.count = std::uint8_t(param2 > 0 && param2 < 256 ? param2 : 1);
        if (g_pBall && g_pBall->m_pShooter && g_pBall->m_pShooter->m_pTeam
            && g_pBall->m_pShooter->m_pTeam->m_nSide != side)
            event.flags |= kOwnGoal;
        break;
    case STATS_LOW_CHARGE_SHOTS:
    case STATS_MEDIUM_CHARGE_SHOTS:
    case STATS_HIGH_CHARGE_SHOTS:
    case STATS_STS_ATTEMPTS:
        PlaceOf(side, event.player, event);
        break;
    case STATS_MEGASTRIKE_ATTEMPTS:
        event.count = std::uint8_t(param0 > 0 && param0 < 256 ? param0 : 0);
        PlaceOf(side, event.player, event);
        break;
    case STATS_POWERUPS_USED:
        if (sItem.side == side && sItem.player == player && sItem.frame == Frame())
        {
            event.other = std::int8_t(sItem.type);
            event.count = std::uint8_t(sItem.count > 0 && sItem.count < 256 ? sItem.count : 1);
        }
        PlaceOf(side, event.player, event);
        break;
    case STATS_HITS_MADE:
    case STATS_ATTACK_SUCCESSES:
    case STATS_FOULS:
        PlaceOf(side, event.player, event);
        break;
    default:
        return;
    }
    event.frame = Frame();
    event.clock = MatchClock();
    sStats.events[sStats.event_count++] = event;
}
}
