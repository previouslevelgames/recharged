// Competitive play in the game module: the rules the host received for the
// player's accepted match (charged_configure_competitive), applied to a RANKED
// match against that match's opponent through the hooks of tweak patch 0046
// (mscharged_competitive.h), and the game's WFC login for the host
// (charged_wfc_snapshot), which proves it to the profile service.
//
// A set's game (charged_configure_competitive_start, the launcher's Ready to
// play!) also starts itself through the hooks of tweak patch 0047: the title
// is passed, the main menu goes online, the online Mii menu takes the
// player's own Mii, the login goes on into the RANKED search, the connection
// check accepts the set's opponent (and nobody else), and popups are answered;
// one that is not harmless ends the set. The host closes the game once the
// set is over or failed. Everything else stays the retail online flow.
#if !defined(MSCHARGED_NATIVE) || !defined(MSCHARGED_GAME_MODULE)
#error Competitive play belongs to the isolated original game module
#endif
#include "platform/competitive_rules.h"

#include "Game/FE/FrontendSceneCatalog.h"
#include "Game/FE/fePopupMenu.h"
#include "Game/GameInfo.h"
#include "Game/GameSceneManager.h"
#include "Game/NetworkDraft.h"
#include "Game/NetworkLobby.h"
#include "Game/NetworkMessages.h"
#include "Game/NetworkPeer.h"
#include "Game/NetworkSession.h"
#include "mscharged_competitive.h"

#include <RVLFaceLib/RFL_Database.h>
#include <dwc/dwc_login.h>
#include <dwc/dwc_main.h>

#include <cstdio>
#include <cstring>
#include <iterator>

namespace
{
// The host's rules (sPending), and the ones of the match set up last (sMatch).
bool sPending = false, sActive = false;
ChargedCompetitiveRules sRules{}, sMatch{};
MschargedCompetitiveSettings sSettings{};
unsigned sSetups = 0;

// A set's game: how far its start went (ChargedSetStage), the player's own
// Mii (its create ID) and the popup that ended it.
int sSetStage = CHARGED_SET_NONE;
RFLCreateID sSetMii{};
int sSetPopup = -1;

bool OnSet() { return sSetStage != CHARGED_SET_NONE; }

// A popup is on top: a step must not push past it (Push with popfirst would
// pop the popup).
bool PopupOnTop()
{
    GameSceneManager* manager = GameSceneManager::Instance();
    return manager && manager->GetCurrentScene()
        && manager->GetSceneType(manager->GetCurrentScene()) == SCENE_POPUP_MENU;
}

bool IsOpponent(unsigned profile_id)
{
    if (!sPending || !profile_id) return false;
    for (int n = 0; n < sRules.opponent_count && n < int(std::size(sRules.opponents)); ++n)
        if (sRules.opponents[n] == profile_id) return true;
    return false;
}

// An online RANKED match of two machines, not the in-session tournament.
bool RankedOnline()
{
    const GameInfoManager* info = nlSingleton<GameInfoManager>::Instance();
    return info && info->mIsOnlineMode && info->mOnlineRankedMatch
        && info->mCurrentMode != GameInfoManager::GM_ONLINE_TOURNAMENT;
}

// The other machine of this online match is one of the opponent's WFC
// profiles. Its profile comes from the draft (BeginSortedDraft), which both
// machines have; the lobby's machine info reaches only the host.
bool AgainstOpponent()
{
    const NetworkDraft* draft = NetworkDraft::Instance();
    if (!draft) return false;
    const NetMessageDraft& message = const_cast<NetworkDraft*>(draft)->GetDraftMessage();
    if (message.mMachineCount != 2) return false;
    for (int machine = 0; machine < 2; ++machine)
        if (IsOpponent(message.mEntries[machine].mProfileId)) return true;
    return false;
}

bool Applies() { return sPending && RankedOnline() && AgainstOpponent(); }
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
    s.set_stage = sSetStage;
    s.set_popup = sSetPopup;
    s.competitive_setups = sSetups;
    s.competitive_games = sActive ? sMatch.games : 0;
    // StartNetworkedGame gives each machine's players a side by their network
    // player id (machine * 4 + player): this machine's first player's. The
    // main menu's Online sets only mIsOnlineMode, so the mode stays -1 after
    // a fresh start until InitializeOnlineMatch: no game info to read yet.
    s.local_side = -1;
    const GameInfoManager* info = nlSingleton<GameInfoManager>::Instance();
    if (info && info->mIsOnlineMode && g_pNetworkSession && g_pNetworkSession->mLocalMachineId >= 0
        && g_pNetworkSession->mLocalMachineId < 4 && info->mCurrentMode >= 0
        && info->mCurrentMode < GameInfoManager::GM_NUM_MODES && info->mGameInfo[info->mCurrentMode])
    {
        const auto player = GetNetworkPlayerId(0, s8(g_pNetworkSession->mLocalMachineId));
        const short side = info->GetPlayingSide((unsigned short)player);
        if (side == 0 || side == 1) s.local_side = side;
    }
    *snapshot = s;
    return true;
}

// Host entry, before original main: this game is a set's (`mii`: the
// player's own Mii's create ID, 8 bytes): it starts itself into the set's
// RANKED search.
extern "C" void mscharged_tweak_set_start(void);
extern "C" __attribute__((visibility("default"))) bool charged_configure_competitive_start(const unsigned char* mii,
                                                                                           unsigned size)
{
    if (!mii || size != sizeof sSetMii) return false;
    std::memcpy(&sSetMii, mii, sizeof sSetMii);
    sSetStage = CHARGED_SET_BOOT;
    mscharged_tweak_set_start();
    return true;
}

extern "C" bool mscharged_competitive_take_main_menu(void)
{
    if (sSetStage != CHARGED_SET_BOOT || PopupOnTop()) return false;
    sSetStage = CHARGED_SET_MII;
    return true;
}

extern "C" int mscharged_competitive_mii_position(const int* official_indices, int count)
{
    if (sSetStage != CHARGED_SET_MII || PopupOnTop()) return -1;
    u16 index = 0;
    if (RFLSearchOfficialData(&sSetMii, &index))
        for (int position = 0; position < count; ++position)
            if (official_indices[position] == index)
            {
                sSetStage = CHARGED_SET_OVERLAY;
                return position;
            }
    std::fputs("Competitive: the set's Mii is not in the Mii database\n", stderr);
    sSetStage = CHARGED_SET_FAILED;
    return -1;
}

extern "C" bool mscharged_competitive_take_use(void)
{
    if (sSetStage != CHARGED_SET_OVERLAY || PopupOnTop()) return false;
    sSetStage = CHARGED_SET_LOGIN;
    return true;
}

// DWC refuses to start a search while it updates its servers, the friend
// list sync that follows the login, which the retail menus always outlast: the
// set's game waits for it, at most 600 frames (10 s). Korea's DWC stays
// ONLINE during the sync and starts a search anyway: nothing to wait for.
extern "C" bool mscharged_competitive_ranked_waits(void)
{
#if defined(VERSION_R4QK01)
    return false;
#else
    static int sWaitedFrames = 0;
    if (sSetStage != CHARGED_SET_LOGIN)
    {
        sWaitedFrames = 0;
        return false;
    }
    if (DWC_GetState() != DWC_STATE_UPDATE_SERVERS || sWaitedFrames >= 600) return false;
    ++sWaitedFrames;
    return true;
#endif
}

extern "C" bool mscharged_competitive_take_ranked(void)
{
    if (sSetStage != CHARGED_SET_LOGIN) return false;
    sSetStage = CHARGED_SET_SEARCH;
    return true;
}

extern "C" int mscharged_competitive_connection(unsigned first, unsigned second)
{
    if (sSetStage != CHARGED_SET_SEARCH || !sPending) return -1;
    return IsOpponent(first) || IsOpponent(second) ? 0 : 1;
}

extern "C" int mscharged_competitive_popup(int type)
{
    if (!OnSet()) return -1;
    switch (type)
    {
    case POPUP_LOW_BATTERY:
    case POPUP_PROFILE_MII_DELETION_WARNING: // a Mii's first time online
    case POPUP_ONLINE_NO_COPY:               // the online save is new
        return 0;
    default:
        if (sSetStage != CHARGED_SET_OVER && sSetStage != CHARGED_SET_FAILED)
        {
            std::fprintf(stderr, "Competitive: popup %d ended the set\n", type);
            sSetPopup = type;
            sSetStage = CHARGED_SET_FAILED;
        }
        return 0;
    }
}

extern "C" void mscharged_competitive_back_from_game(void)
{
    if (sSetStage == CHARGED_SET_SEARCH || sSetStage == CHARGED_SET_PLAYING) sSetStage = CHARGED_SET_OVER;
}

extern "C" const MschargedCompetitiveSettings* mscharged_competitive_begin(void)
{
    sActive = Applies();
    if (sActive && sSetStage == CHARGED_SET_SEARCH) sSetStage = CHARGED_SET_PLAYING;
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
