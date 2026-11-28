#include "launcher/match_report.h"

#include <algorithm>
#include <cstdio>

namespace mscharged::launcher
{
namespace
{
using replay::MatchStats;
using replay::StatEvent;

// The game's ePlayerStats.
enum Stat
{
    kLowShots = 0x00,
    kMediumShots = 0x01,
    kHighShots = 0x02,
    kShotsOnGoal = 0x03,
    kSkillshots = 0x04,
    kAssists = 0x08,
    kMegaStrikes = 0x09,
    kGoalsFor = 0x0B,
    kPasses = 0x0D,
    kFouls = 0x11,
    kHits = 0x12,
    kSteals = 0x14,
    kDekes = 0x15,
    kPossession = 0x16,
    kItems = 0x19,
    kPerfectPasses = 0x23,
    kInterceptions = 0x24,
};
constexpr int kMegaStrikeGoal = 6; // the game's goal type
constexpr float kStepsPerSecond = 50.0f;
// A goal belongs to the shot its scorer took at most this long before.
constexpr float kShotToGoal = 5.0f;

int Total(const MatchStats& stats, int side, int stat)
{
    long long total = 0;
    for (const auto& player : stats.counts[side]) total += player[stat];
    return int(std::min<long long>(total, 0x7FFFFFFF));
}

bool IsShot(int stat)
{
    return stat == kLowShots || stat == kMediumShots || stat == kHighShots || stat == kSkillshots
        || stat == kMegaStrikes;
}

ShotKind KindOf(int stat)
{
    switch (stat)
    {
    case kMediumShots: return ShotKind::Medium;
    case kHighShots: return ShotKind::High;
    case kSkillshots: return ShotKind::Skillshot;
    case kMegaStrikes: return ShotKind::MegaStrike;
    default: return ShotKind::Low;
    }
}
} // namespace

std::string ClockText(float seconds)
{
    const int whole = seconds > 0.0f ? int(seconds) : 0;
    char text[16];
    std::snprintf(text, sizeof text, "%d:%02d", whole / 60, whole % 60);
    return text;
}

MatchReport BuildMatchReport(const replay::Setup& setup)
{
    MatchReport report;
    const MatchStats& stats = setup.stats;
    report.has_stats = stats.kept != 0;
    report.score = {setup.score[0], setup.score[1]};
    report.length = float(setup.steps) / kStepsPerSecond;
    report.sides[0].captain = setup.home_team;
    report.sides[1].captain = setup.away_team;
    for (int slot = 0; slot < 3; ++slot)
    {
        report.sides[0].sidekicks[std::size_t(slot)] = setup.home_sidekicks[slot];
        report.sides[1].sidekicks[std::size_t(slot)] = setup.away_sidekicks[slot];
    }
    for (int pad = 0; pad < 16; ++pad)
        if (setup.playing_sides[pad] == 0 || setup.playing_sides[pad] == 1)
            report.sides[std::size_t(setup.playing_sides[pad])].pads.push_back(pad);
    if (!report.has_stats) return report;

    for (int side = 0; side < 2; ++side)
    {
        auto& out = report.sides[std::size_t(side)];
        out.name = stats.names[side];
        out.name_kind = stats.name_kinds[side];
    }
    report.field_x = stats.field[0];
    report.field_y = stats.field[1];
    report.box_x = stats.field[2];
    report.box_y = stats.field[3];
    report.corner = stats.field[4];

    const auto seconds = [&](const StatEvent& event) {
        if (event.clock >= 0.0f) return event.clock;
        return event.frame >= stats.first_frame
            ? float(event.frame - stats.first_frame) / kStepsPerSecond
            : 0.0f;
    };

    // The moments in order.
    std::array<int, 2> score{setup.resumes ? setup.start_score[0] : 0, setup.resumes ? setup.start_score[1] : 0};
    std::array<int, 2> mega_strikes{};
    const int events = std::min<int>(stats.event_count, replay::kMaxStatEvents);
    for (int n = 0; n < events; ++n)
    {
        const StatEvent& event = stats.events[n];
        const int side = event.side & 1;
        const float at = seconds(event);
        if (event.stat == kGoalsFor)
        {
            GoalLine goal;
            goal.seconds = at;
            goal.side = side;
            goal.own_goal = (event.flags & replay::kOwnGoal) != 0;
            goal.player = goal.own_goal ? -1 : event.player;
            goal.assist = goal.own_goal ? -1 : event.other;
            goal.kind = event.kind;
            goal.count = std::max<int>(1, event.count);
            score[std::size_t(side)] += goal.count;
            goal.score = score;
            report.goals.push_back(goal);
            report.moments.push_back({Moment::Goal, at, side});
            if (!goal.own_goal && goal.player >= 0)
            {
                report.players[std::size_t(side)][std::size_t(goal.player)][std::size_t(PlayerColumn::Goals)]
                    += goal.count;
                if (goal.assist >= 0 && goal.assist < 5)
                    ++report.players[std::size_t(side)][std::size_t(goal.assist)][std::size_t(PlayerColumn::Assists)];
            }
            // The shot it came from: the scorer's last one shortly before (a
            // Mega Strike's comes in the same step, right after its goals).
            if (!goal.own_goal && event.kind != kMegaStrikeGoal)
                for (auto shot = report.shots.rbegin(); shot != report.shots.rend(); ++shot)
                {
                    if (at - shot->seconds > kShotToGoal) break;
                    if (shot->side != side || shot->player != event.player || shot->scored
                        || shot->kind == ShotKind::MegaStrike)
                        continue;
                    shot->scored = true;
                    break;
                }
            continue;
        }
        if (IsShot(event.stat))
        {
            ShotSpot shot;
            shot.seconds = at;
            shot.side = side;
            shot.player = event.player;
            shot.kind = KindOf(event.stat);
            shot.x = event.x;
            shot.y = event.y;
            if (shot.kind == ShotKind::MegaStrike)
            {
                ++mega_strikes[std::size_t(side)];
                // Its goals came just before it, in the same step.
                for (auto goal = report.goals.rbegin(); goal != report.goals.rend(); ++goal)
                {
                    if (goal->seconds < at - 0.1f) break;
                    if (goal->side == side && goal->kind == kMegaStrikeGoal) shot.scored = true;
                }
            }
            report.shots.push_back(shot);
            continue;
        }
        switch (event.stat)
        {
        case kItems:
        {
            ItemUse item{at, side, event.player, event.other, std::max<int>(1, event.count)};
            report.items.push_back(item);
            report.moments.push_back({Moment::Item, at, side});
            auto& counts = report.item_counts[std::size_t(side)];
            auto found = std::find_if(counts.begin(), counts.end(), [&](const ItemCount& count) {
                return count.type == item.type && count.count == item.count;
            });
            if (found == counts.end()) counts.push_back({item.type, item.count, 1});
            else ++found->uses;
            break;
        }
        case kHits: report.moments.push_back({Moment::Hit, at, side}); break;
        case kSteals: report.moments.push_back({Moment::Steal, at, side}); break;
        case kFouls: report.moments.push_back({Moment::Foul, at, side}); break;
        default: break;
        }
    }
    for (auto& counts : report.item_counts)
        std::stable_sort(counts.begin(), counts.end(),
                         [](const ItemCount& a, const ItemCount& b) { return a.uses > b.uses; });
    for (const Moment& moment : report.moments) report.length = std::max(report.length, moment.seconds);

    // The totals side by side.
    std::array<long long, 2> possession{};
    for (int side = 0; side < 2; ++side)
    {
        auto row = [&](StatRow which) -> int& { return report.rows[std::size_t(which)][std::size_t(side)]; };
        row(StatRow::Goals) = report.score[std::size_t(side)];
        row(StatRow::Shots) = Total(stats, side, kLowShots) + Total(stats, side, kMediumShots)
            + Total(stats, side, kHighShots) + Total(stats, side, kSkillshots) + mega_strikes[std::size_t(side)];
        row(StatRow::ShotsOnGoal) = Total(stats, side, kShotsOnGoal);
        row(StatRow::Skillshots) = Total(stats, side, kSkillshots);
        row(StatRow::MegaStrikes) = mega_strikes[std::size_t(side)];
        row(StatRow::Passes) = Total(stats, side, kPasses);
        row(StatRow::PerfectPasses) = Total(stats, side, kPerfectPasses);
        row(StatRow::Interceptions) = Total(stats, side, kInterceptions);
        row(StatRow::Hits) = Total(stats, side, kHits);
        row(StatRow::Steals) = Total(stats, side, kSteals);
        row(StatRow::Dekes) = Total(stats, side, kDekes);
        row(StatRow::Items) = Total(stats, side, kItems);
        row(StatRow::Fouls) = Total(stats, side, kFouls);
        possession[std::size_t(side)] = Total(stats, side, kPossession);
        for (int player = 0; player < 5; ++player)
        {
            const auto& counts = stats.counts[side][player];
            auto& line = report.players[std::size_t(side)][std::size_t(player)];
            line[std::size_t(PlayerColumn::Shots)] = int(counts[kLowShots] + counts[kMediumShots] + counts[kHighShots]
                                                         + counts[kSkillshots]);
            line[std::size_t(PlayerColumn::Passes)] = int(counts[kPasses]);
            line[std::size_t(PlayerColumn::Hits)] = int(counts[kHits]);
            line[std::size_t(PlayerColumn::Steals)] = int(counts[kSteals]);
            line[std::size_t(PlayerColumn::Items)] = int(counts[kItems]);
        }
    }
    for (const ShotSpot& shot : report.shots)
        if (shot.kind == ShotKind::MegaStrike && shot.player >= 0 && shot.player < 5)
            ++report.players[std::size_t(shot.side)][std::size_t(shot.player)][std::size_t(PlayerColumn::Shots)];
    const long long both = possession[0] + possession[1];
    if (both > 0)
    {
        const int home = int((possession[0] * 100 + both / 2) / both);
        report.rows[std::size_t(StatRow::Possession)] = {home, 100 - home};
    }
    return report;
}
} // namespace mscharged::launcher
