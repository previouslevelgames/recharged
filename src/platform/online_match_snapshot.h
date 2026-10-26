#pragma once

// The online series this machine plays, for the WFC server's match reports
// (src/platform/online_reports.h). The game module fills it
// (charged_online_match_snapshot); the host reads it after a game frame, on
// the thread that runs the game's tasks. Numbers are the game's own enums.

// One game of the series, as this machine saw it end.
struct ChargedOnlineGame
{
    int scores[2];   // goals of home and away
    int captains[2]; // eTeamID of home and away
    int stadium;     // eStadiumID, -1 unknown
    // How it ended (NetworkStatsManager::CalculateAndReportGameResult): 0
    // normally, 2 out of sync, 4 the connection lost.
    int ending;
    float seconds; // the match clock at the end
};

struct ChargedOnlineMatchSnapshot
{
    // Counts the series since the game started: a new value is another
    // series. 0: none yet.
    unsigned series;
    // 0 under way, 1 decided (a side won enough games), 2 stopped before.
    int ended;
    unsigned profile_id; // the WFC profile of the Mii online, 0 unknown
    int ranked;          // a ranked series; else one between friends
    int best_of;         // games in the series
    int side;            // this machine's team: 0 home, 1 away
    char mii[2][48];     // the Mii names of home and away, UTF-8
    int captains[2];     // eTeamID of home and away, as drafted
    int sidekicks[2][3]; // eSidekickID of their lineups, -1 unknown
    int games;           // games ended so far
    ChargedOnlineGame game[9];
};

#ifdef __cplusplus
// One result the game settles (NetworkStatsManager::
// CalculateAndReportGameResult) applied to a series under way: the game
// module's rule, which its test runs on the host. A result for a game already
// settled (`reported`: a lost connection on the screens after it) or for one
// that never began (`game` null) adds no game; then a lost connection or a
// sync error only stops the series. Otherwise the game joins the series,
// which is decided once a side has won more than half of best_of (the game's
// own rule, IsCurrentSeriesComplete), and stopped with a game that did not
// end normally.
inline void ChargedOnlineSeriesResult(ChargedOnlineMatchSnapshot& series, int result, bool reported,
                                      const ChargedOnlineGame* game)
{
    if (!series.series || series.ended) return;
    if (reported || !game)
    {
        if (result != 0) series.ended = 2;
        return;
    }
    if (series.games < int(sizeof series.game / sizeof series.game[0])) series.game[series.games++] = *game;
    int won[2] = {};
    for (int n = 0; n < series.games; ++n)
        if (series.game[n].scores[0] != series.game[n].scores[1])
            ++won[series.game[n].scores[0] > series.game[n].scores[1] ? 0 : 1];
    if (result != 0)
        series.ended = 2;
    else if (series.best_of > 0 && (won[0] > series.best_of / 2 || won[1] > series.best_of / 2))
        series.ended = 1;
}
#endif

#ifdef __cplusplus
extern "C" {
#endif
// Game module entry: fills `snapshot` when `size` matches this build's
// struct; false otherwise.
typedef bool (*ChargedOnlineMatchSnapshotFunction)(ChargedOnlineMatchSnapshot* snapshot, unsigned size);
#ifdef __cplusplus
}
#endif
