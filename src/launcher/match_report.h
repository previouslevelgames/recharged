#pragma once

// What the launcher's match stats show of a replay (the Replays page): the
// sides and their players, the totals side by side, the goals in order, the
// shots on the field and the items, from the replay's setup and the stats the
// game counted (platform/match_replay_stream.h, MatchStats). Independent of
// SDL/ImGui so it can be unit tested.

#include "platform/match_replay_stream.h"

#include <array>
#include <string>
#include <vector>

namespace mscharged::launcher
{
// The comparison rows, in the order shown.
enum class StatRow
{
    Goals,
    Shots,        // charged shots, Skillshots and Mega Strikes taken
    ShotsOnGoal,  // the game's shots on target (saved, the post, scored)
    Skillshots,   // taken
    MegaStrikes,  // taken
    Passes,
    PerfectPasses,
    Interceptions,
    Hits,
    Steals,
    Dekes,
    Items,
    Fouls,
    Possession,   // percent of the time either side had the ball
    Count
};

struct SideReport
{
    int captain = -1;
    std::array<int, 3> sidekicks{-1, -1, -1};
    std::string name;  // a player's name (online), empty offline
    int name_kind = 0; // replay::kName*
    std::vector<int> pads; // the controllers that played this side (0-15), offline
};

struct GoalLine
{
    float seconds = 0.0f; // into the match
    int side = 0;         // who it counts for
    int player = -1;      // the scorer (0 captain, 1-3 sidekicks, 4 goalie) of `side`, -1 own goal/unknown
    int assist = -1;
    int kind = 0;         // the game's goal type (6 Mega Strike)
    int count = 1;        // goals it made (a Mega Strike several)
    bool own_goal = false;
    std::array<int, 2> score{}; // the score after it
};

enum class ShotKind
{
    Low,
    Medium,
    High,
    Skillshot,
    MegaStrike,
};

struct ShotSpot
{
    float seconds = 0.0f;
    int side = 0;
    int player = -1;
    ShotKind kind = ShotKind::Low;
    float x = 0.0f, y = 0.0f; // the game's field: x along it, y across
    bool scored = false;
};

struct ItemUse
{
    float seconds = 0.0f;
    int side = 0;
    int player = -1;
    int type = -1; // ePowerUpType, -1 unknown
    int count = 1;
};

// How often a side used an item of a type and count.
struct ItemCount
{
    int type = -1;
    int count = 1;
    int uses = 0;
};

// A moment of the timeline: a goal, an item, a hit, a steal or a foul.
struct Moment
{
    enum Kind
    {
        Goal,
        Item,
        Hit,
        Steal,
        Foul
    } kind = Goal;
    float seconds = 0.0f;
    int side = 0;
};

// A player's line of the players' table.
enum class PlayerColumn
{
    Goals,
    Assists,
    Shots,
    Passes,
    Hits,
    Steals,
    Items,
    Count
};

struct MatchReport
{
    bool has_stats = false;
    std::array<SideReport, 2> sides;
    std::array<int, 2> score{};
    float length = 0.0f; // seconds of play
    std::array<std::array<int, 2>, std::size_t(StatRow::Count)> rows{};
    std::vector<GoalLine> goals;
    std::vector<ShotSpot> shots;
    std::vector<ItemUse> items;
    std::array<std::vector<ItemCount>, 2> item_counts; // most used first
    std::vector<Moment> moments;
    // [side][player 0-4][column]
    std::array<std::array<std::array<int, std::size_t(PlayerColumn::Count)>, 5>, 2> players{};
    // The field's half length and half width, and its penalty box (0 unknown).
    float field_x = 0.0f, field_y = 0.0f, box_x = 0.0f, box_y = 0.0f, corner = 0.0f;
};

MatchReport BuildMatchReport(const replay::Setup& setup);

// m:ss
std::string ClockText(float seconds);
} // namespace mscharged::launcher
