// The WFC server's match reports: each online series this machine plays, as
// it saw it. Tweak patch 0037 calls mscharged_tweak_online_game_result where
// the game settles a game's result (NetworkStatsManager::
// CalculateAndReportGameResult: a normal end, a sync error or a lost
// connection), tweak patch 0045 mscharged_tweak_online_error where it shows
// an online error code (GetOnlineErrorPopup); charged_online_match_snapshot
// hands both to the host, which reports them (src/platform/online_reports.h).
// All run on the thread of the game's tasks. Read only: the game is unchanged.
#if !defined(MSCHARGED_NATIVE) || !defined(MSCHARGED_GAME_MODULE) || !defined(MSCHARGED_TWEAKS)
#error The online match snapshot belongs to the isolated original game module built with MSCHARGED_TWEAKS
#endif
#include "mscharged_online_reports.h"
#include "platform/online_match_snapshot.h"

#include "Game/Game.h"
#include "Game/GameInfo.h"
#include "Game/NetTournManager.h"
#include "Game/NetworkDebug.h"
#include "Game/NetworkDraft.h"
#include "Game/NetworkSession.h"
#include "Game/NetworkStatsManager.h"
#include "Game/NetworkSync.h"
#include "Game/Sys/clock.h"
#include "Game/Team.h"
#include <dwc/dwc_account.h>

#include <cstring>

namespace
{
ChargedOnlineMatchSnapshot sSeries{}; // the series under way, or the last one
unsigned sSeriesCount = 0;
// A series ended and the game that ended it may still be on screen: the next
// series waits for another game.
bool sAwaitingGame = false;
ChargedOnlineError sError{}; // the last error code shown

bool UnderWay() { return sSeries.series && !sSeries.ended; }

// The game this machine plays has begun (its kickoff or later).
bool Begun()
{
    return g_pGame && g_pGame->m_eGameState != GS_NONE && g_pGame->m_eGameState != GS_PRE_GAME;
}

// A Mii name (UTF-16, at most 11 units) as UTF-8.
void Utf8(const u16* name, char* out, std::size_t size)
{
    std::size_t n = 0;
    for (int i = 0; i < 11 && name[i]; ++i)
    {
        unsigned c = name[i];
        if (c >= 0xD800 && c < 0xDC00 && i + 1 < 11 && name[i + 1] >= 0xDC00 && name[i + 1] < 0xE000)
            c = 0x10000 + ((c - 0xD800) << 10) + (name[++i] - 0xDC00);
        else if (c >= 0xD800 && c < 0xE000)
            c = 0xFFFD;
        char bytes[4];
        std::size_t length = 0;
        if (c < 0x80)
            bytes[length++] = char(c);
        else if (c < 0x800)
        {
            bytes[length++] = char(0xC0 | c >> 6);
            bytes[length++] = char(0x80 | (c & 0x3F));
        }
        else if (c < 0x10000)
        {
            bytes[length++] = char(0xE0 | c >> 12);
            bytes[length++] = char(0x80 | (c >> 6 & 0x3F));
            bytes[length++] = char(0x80 | (c & 0x3F));
        }
        else
        {
            bytes[length++] = char(0xF0 | c >> 18);
            bytes[length++] = char(0x80 | (c >> 12 & 0x3F));
            bytes[length++] = char(0x80 | (c >> 6 & 0x3F));
            bytes[length++] = char(0x80 | (c & 0x3F));
        }
        if (n + length >= size) break;
        std::memcpy(out + n, bytes, length);
        n += length;
    }
    out[n] = 0;
}

bool Online()
{
    const GameInfoManager* info = nlSingleton<GameInfoManager>::Instance();
    return info && info->IsOnline() && g_pNetworkSessionBase
        && g_pNetworkSessionBase->GetSessionMode() == NET_MODE_ONLINE;
}

// The WFC profile of the Mii online: its DWCAccUserData in the save slot it
// plays from. 0 before its first login, or with no Mii chosen.
unsigned ProfileId()
{
    GameInfoManager* info = nlSingleton<GameInfoManager>::Instance();
    if (!info || gNetworkSaveSlotIndex < 0 || gNetworkSaveSlotIndex >= 10) return 0;
    const auto* user = reinterpret_cast<const DWCAccUserData*>(info->GetSaveSlot(gNetworkSaveSlotIndex));
    return user->gs_profile_id > 0 ? unsigned(user->gs_profile_id) : 0;
}

// The drafted teams of home and away, as the game reports them
// (CalculateAndReportGameResult): by tournament seat in an online tournament.
NetworkDraftTeam* DraftTeam(int side)
{
    NetworkDraft* draft = NetworkDraft::Instance();
    if (!draft) return nullptr;
    NetTournManager* tournament = NetTournManager::Instance();
    if (tournament && tournament->mState != NET_TOURN_INACTIVE)
        return draft->FindDraftTeamByPeerIndex(tournament->MachineIdxToTournamentIdx(side));
    return draft->GetDraftTeam(side);
}

// A new series: who plays it, with which lineups, over how many games.
void Begin()
{
    sSeries = ChargedOnlineMatchSnapshot{};
    sSeries.series = ++sSeriesCount;
    sAwaitingGame = false;
    GameInfoManager* info = nlSingleton<GameInfoManager>::Instance();
    sSeries.profile_id = ProfileId();
    sSeries.ranked = IsOnlineRankedMatch() ? 1 : 0;
    sSeries.best_of = info && info->GetCurrentSettings() ? info->GetCurrentSettings()->NumGames : 0;
    sSeries.side = GetLocalNetworkPlayingSide() == 1 ? 1 : 0;
    for (int side = 0; side < 2; ++side)
    {
        sSeries.captains[side] = TEAM_INVALID;
        for (int& sidekick : sSeries.sidekicks[side]) sidekick = -1;
        if (const NetworkDraftTeam* team = DraftTeam(side))
        {
            Utf8(team->mPlayers[0].mName, sSeries.mii[side], sizeof sSeries.mii[side]);
            sSeries.captains[side] = team->mCaptain;
            for (int slot = 0; slot < 3; ++slot) sSeries.sidekicks[side][slot] = team->mSidekicks[slot];
        }
    }
}
} // namespace

void mscharged_tweak_online_game_result(int result, int homeScore, int awayScore, int reported)
{
    // LAN games have no WFC server to report to.
    if (!Online()) return;
    ChargedOnlineGame game{};
    const bool played = !reported && Begun();
    if (played)
    {
        // A result for a game that had no series yet (the host had not looked
        // since its kickoff) begins one.
        if (!UnderWay()) Begin();
        game.scores[0] = homeScore;
        game.scores[1] = awayScore;
        game.captains[0] = game.captains[1] = TEAM_INVALID;
        game.stadium = STAD_INVALID;
        if (const GameInfoManager* info = nlSingleton<GameInfoManager>::Instance();
            info && info->mCurrentMode >= 0 && info->mCurrentMode < GameInfoManager::GM_NUM_MODES)
            if (const BasicGameInfo* current = info->mGameInfo[info->mCurrentMode])
            {
                game.captains[0] = current->mTeamIndex[0];
                game.captains[1] = current->mTeamIndex[1];
                game.stadium = current->mStadiumIndex;
            }
        game.ending = result;
        if (g_pGame->m_pGameClock) game.seconds = g_pGame->m_pGameClock->m_fTimer;
        // Out of sync: the check that failed has its frame already (each
        // NetworkSyncState::On*Mismatch sets it before the sync error that
        // brings this result); the state is reset for each game.
        if (result == 2 && gNetworkSyncState)
            ChargedOnlineSyncCheck(game, gNetworkSyncState->mFrameMismatchFrame,
                                   gNetworkSyncState->mChecksumMismatchFrame,
                                   gNetworkSyncState->mRandomSeedMismatchFrame);
    }
    const bool underWay = UnderWay();
    ChargedOnlineSeriesResult(sSeries, result, reported != 0, played ? &game : nullptr);
    if (underWay && !UnderWay()) sAwaitingGame = true;
}

void mscharged_tweak_online_error(int code)
{
    ++sError.shown;
    sError.code = code;
    sError.phase = g_pNetworkSession ? int(g_pNetworkSession->mSessionState) : -1;
    sError.profile_id = ProfileId();
}

extern "C" __attribute__((visibility("default"))) bool charged_online_match_snapshot(
    ChargedOnlineMatchSnapshot* snapshot, unsigned size)
{
    if (!snapshot || size != sizeof(ChargedOnlineMatchSnapshot)) return false;
    // A series begins with the kickoff of its first online game (the
    // screens after the last one's end are no new game); leaving the online
    // session before it is decided stops it.
    const bool online = Online();
    if (!g_pGame || g_pGame->m_eGameState == GS_NONE || g_pGame->m_eGameState == GS_PRE_GAME
        || g_pGame->m_eGameState == GS_UNLOADING)
        sAwaitingGame = false;
    if (!UnderWay() && !sAwaitingGame && online && g_pGame
        && (g_pGame->m_eGameState == GS_KICKOFF || g_pGame->m_eGameState == GS_GAMEPLAY))
        Begin();
    else if (UnderWay() && !online)
    {
        sSeries.ended = 2;
        sSeries.stopped_by = 0; // with no result
        sAwaitingGame = true;
    }
    *snapshot = sSeries;
    snapshot->error = sError;
    return true;
}
