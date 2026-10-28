#pragma once

// The online series this machine plays, for the WFC server's match reports
// (src/platform/online_reports.h), and the error codes its online menus show.
// The game module fills it (charged_online_match_snapshot); the host reads it
// after a game frame, on the thread that runs the game's tasks. Numbers are
// the game's own enums.

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
    // Out of sync: the first of the game's sync checks that failed, 1 the
    // frames, 2 the checksums, 3 the random seeds (the order the game checks
    // them in), 0 unknown; and the frame it failed on.
    int sync_check;
    int sync_frame;
};

// An error code the game showed its player (GetOnlineErrorPopup: the popups
// of the online login, matchmaking, friends and menus).
struct ChargedOnlineError
{
    // Counts the codes shown since the game started: a new value is another
    // one. 0: none yet.
    unsigned shown;
    int code;            // the number the popup showed
    int phase;           // eNetworkSessionState then, -1 unknown
    unsigned profile_id; // the WFC profile of the Mii online then, 0 unknown
};

struct ChargedOnlineMatchSnapshot
{
    // Counts the series since the game started: a new value is another
    // series. 0: none yet.
    unsigned series;
    // 0 under way, 1 decided (a side won enough games), 2 stopped before.
    int ended;
    // What stopped it (ended 2): the result that did, 2 out of sync or 4 the
    // connection lost (as a game's ending); 0 the online session ended
    // without one.
    int stopped_by;
    unsigned profile_id; // the WFC profile of the Mii online, 0 unknown
    int ranked;          // a ranked series; else one between friends
    int best_of;         // games in the series
    int side;            // this machine's team: 0 home, 1 away
    char mii[2][48];     // the Mii names of home and away, UTF-8
    int captains[2];     // eTeamID of home and away, as drafted
    int sidekicks[2][3]; // eSidekickID of their lineups, -1 unknown
    int games;           // games ended so far
    ChargedOnlineGame game[9];
    // The last error code shown, in a series or not.
    ChargedOnlineError error;
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
// end normally. A stop keeps the result that made it (stopped_by).
inline void ChargedOnlineSeriesResult(ChargedOnlineMatchSnapshot& series, int result, bool reported,
                                      const ChargedOnlineGame* game)
{
    if (!series.series || series.ended) return;
    if (!reported && game)
    {
        if (series.games < int(sizeof series.game / sizeof series.game[0])) series.game[series.games++] = *game;
        int won[2] = {};
        for (int n = 0; n < series.games; ++n)
            if (series.game[n].scores[0] != series.game[n].scores[1])
                ++won[series.game[n].scores[0] > series.game[n].scores[1] ? 0 : 1];
        if (result == 0 && series.best_of > 0 && (won[0] > series.best_of / 2 || won[1] > series.best_of / 2))
            series.ended = 1;
    }
    if (result != 0)
    {
        series.ended = 2;
        series.stopped_by = result;
    }
}

// Which of the game's sync checks a game out of sync failed, from the frames
// the game's sync state (NetworkSyncState) holds for a frame, a checksum and
// a random seed mismatch (-1 none): the first that failed, and on one frame
// the first the game checks.
inline void ChargedOnlineSyncCheck(ChargedOnlineGame& game, int frameMismatch, int checksumMismatch,
                                   int seedMismatch)
{
    const int frames[] = {frameMismatch, checksumMismatch, seedMismatch};
    for (int check = 0; check < 3; ++check)
        if (frames[check] >= 0 && (!game.sync_check || frames[check] < game.sync_frame))
        {
            game.sync_check = check + 1;
            game.sync_frame = frames[check];
        }
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
