// Competitive play in the game module: the rules the host received for the
// player's accepted match (charged_configure_competitive), applied to a RANKED
// match against that match's opponent through the hooks of tweak patch 0046
// (mscharged_competitive.h), and the game's WFC login for the host
// (charged_wfc_snapshot), which proves it to the profile service.
#if !defined(MSCHARGED_NATIVE) || !defined(MSCHARGED_GAME_MODULE)
#error Competitive play belongs to the isolated original game module
#endif
#include "platform/competitive_rules.h"

#include "Game/GameInfo.h"
#include "Game/NetworkLobby.h"
#include "Game/NetworkMessages.h"
#include "Game/NetworkPeer.h"
#include "Game/NetworkSession.h"
#include "mscharged_competitive.h"

#include <dwc/dwc_login.h>

#include <cstring>
#include <iterator>

namespace
{
// The host's rules (sPending), and the ones of the match set up last (sMatch).
bool sPending = false, sActive = false;
ChargedCompetitiveRules sRules{}, sMatch{};
MschargedCompetitiveSettings sSettings{};
unsigned sSetups = 0;

// An online RANKED match of two machines, not the in-session tournament.
bool RankedOnline()
{
    const GameInfoManager* info = nlSingleton<GameInfoManager>::Instance();
    return info && info->mIsOnlineMode && info->mOnlineRankedMatch
        && info->mCurrentMode != GameInfoManager::GM_ONLINE_TOURNAMENT;
}

// The other machine of this online match is one of the opponent's WFC
// profiles.
bool AgainstOpponent(const ChargedCompetitiveRules& rules)
{
    NetworkLobby* lobby = g_pNetworkSession ? g_pNetworkSession->mLobby : nullptr;
    if (!lobby || lobby->GetMachineCount() != 2) return false;
    const int local = lobby->GetLocalMachineIndex();
    for (int machine = 0; machine < 2; ++machine)
    {
        if (machine == local) continue;
        const NetworkDraftMachineInfo* other = lobby->GetMachineInfo(machine);
        if (!other) return false;
        for (int n = 0; n < rules.opponent_count && n < int(std::size(rules.opponents)); ++n)
            if (other->mProfileId == rules.opponents[n]) return true;
    }
    return false;
}

bool Applies() { return sPending && RankedOnline() && AgainstOpponent(sRules); }
} // namespace

extern "C" __attribute__((visibility("default"))) bool charged_configure_competitive(const ChargedCompetitiveRules* rules,
                                                                                     unsigned size)
{
    if (rules && size != sizeof(ChargedCompetitiveRules)) return false;
    sPending = rules != nullptr;
    sRules = rules ? *rules : ChargedCompetitiveRules{};
    return true;
}

extern "C" __attribute__((visibility("default"))) bool charged_wfc_snapshot(ChargedWfcSnapshot* snapshot, unsigned size)
{
    if (!snapshot || size != sizeof(ChargedWfcSnapshot)) return false;
    ChargedWfcSnapshot s{};
    // GameSpy's ticket buffer (GP_LOGIN_TICKET_LEN) fits the snapshot's.
    static_assert(GP_LOGIN_TICKET_LEN < sizeof s.login_ticket);
    if (DWCi_CheckLogin())
        if (const DWCAccUserData* user = DWCi_GetUserData())
            if (user->gs_profile_id > 0 && DWCi_GetLoginTicket(s.login_ticket))
            {
                s.login_ticket[GP_LOGIN_TICKET_LEN - 1] = '\0';
                s.logged_in = s.login_ticket[0] != '\0';
                s.profile_id = unsigned(user->gs_profile_id);
            }
    if (!s.logged_in) std::memset(s.login_ticket, 0, sizeof s.login_ticket);
    s.competitive = sActive;
    s.competitive_setups = sSetups;
    s.competitive_games = sActive ? sMatch.games : 0;
    // StartNetworkedGame gives each machine's players a side by their network
    // player id (machine * 4 + player): this machine's first player's.
    s.local_side = -1;
    const GameInfoManager* info = nlSingleton<GameInfoManager>::Instance();
    if (info && info->mIsOnlineMode && g_pNetworkSession && g_pNetworkSession->mLocalMachineId >= 0
        && g_pNetworkSession->mLocalMachineId < 4)
    {
        const auto player = GetNetworkPlayerId(0, s8(g_pNetworkSession->mLocalMachineId));
        const short side = info->GetPlayingSide((unsigned short)player);
        if (side == 0 || side == 1) s.local_side = side;
    }
    *snapshot = s;
    return true;
}

extern "C" const MschargedCompetitiveSettings* mscharged_competitive_begin(void)
{
    sActive = Applies();
    if (!sActive) return nullptr;
    sMatch = sRules;
    sSettings = {sMatch.skill, sMatch.limit, sMatch.time, sMatch.goals, sMatch.games};
    ++sSetups;
    return &sSettings;
}

extern "C" bool mscharged_competitive_tweak(int tweak)
{
    return sActive && tweak >= 0 && tweak < 32 && ((sMatch.tweaks >> tweak) & 1u);
}

extern "C" int mscharged_competitive_stadium(int stadium)
{
    return Applies() && sRules.stadium >= 0 && sRules.stadium <= STAD_SAND_TOMB ? sRules.stadium : stadium;
}
