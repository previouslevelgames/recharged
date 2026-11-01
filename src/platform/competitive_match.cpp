// Competitive play in the game module: the rules the host received for the
// player's accepted match (charged_configure_competitive), applied to a RANKED
// match against that match's opponent through the hooks of tweak patch 0046
// (mscharged_competitive.h), and the game's WFC login for the host
// (charged_wfc_snapshot), which proves it to the profile service.
//
// A set's game (charged_configure_competitive_start: the launcher's Ready to
// play!, or a set that went while the game runs) also starts itself through
// the hooks of tweak patch 0047: the title is passed, the main menu goes
// online, the online Mii menu takes the player's own Mii, the login (or the
// online menu, when the player is already in it) goes on into the RANKED
// search, the connection check accepts the set's opponent (and nobody else),
// and popups are answered; one that is not harmless ends the set. The
// screens it passes by itself are not drawn and their menu sounds are silent
// (tweak patch 0048): the player sees the frontend's camera, the WFC login's
// Connecting screen and the draft. Once the set is over or failed, the host
// ends it (charged_competitive_end_set) and the game stays in its online
// menus. Everything else stays the retail online flow.
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
#include "NL/nlString.h"
#include "mscharged_competitive.h"

#include <RVLFaceLib/RFL_Database.h>
#include <dwc/dwc_login.h>
#include <dwc/dwc_main.h>
#include <dwc/dwc_report.h>

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
// The set's Mii is not on this computer: the player chooses one, and every
// screen stays visible.
bool sPlayerChoosesMii = false;
// The set started in a running game (not before its main): until the walk
// takes the main menu, the player is somewhere in the menus.
bool sStartedInGame = false;
// The set ended while its game still searched RANKED: the search screen leaves
// the search (tweak patch 0049), and nobody is met until it has.
bool sLeaveSearch = false;

// [advanced] log_level = debug: the DWC reports that go to the log: its
// information, errors and warnings, the login, matchmaking (NAT negotiation,
// GT2 connections, QR2 requests, server browser updates), the friend list
// update after the login and the connection test. The game sets the level to
// 0 when its network session starts, so the snapshot sets it again.
constexpr u32 kDwcReport = 0x1 | 0x2 | 0x8 | 0x20 | 0x40 | 0x80 | 0x200 | 0x400 | 0x20000 | 0x40000;
u32 sDwcReport = 0;

bool OnSet() { return sSetStage != CHARGED_SET_NONE; }

// The walk drives the screens unseen: once the main menu went online, or from
// the start in a game the launcher started for the set, until the search.
bool Unseen()
{
    return sSetStage >= CHARGED_SET_BOOT && sSetStage <= CHARGED_SET_LOGIN && !sPlayerChoosesMii
        && (sSetStage >= CHARGED_SET_MII || !sStartedInGame);
}

// A popup is on top: a step must not push past it (Push with popfirst would
// pop the popup).
int TopScene()
{
    GameSceneManager* manager = GameSceneManager::Instance();
    return manager && manager->GetCurrentScene() ? manager->GetSceneType(manager->GetCurrentScene()) : -1;
}

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
    if (sDwcReport) DWC_SetReportLevel(sDwcReport);
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
    // The search screen is gone (the player left it first): nothing to leave.
    if (sLeaveSearch)
        if (GameSceneManager* manager = GameSceneManager::Instance();
            !manager || !manager->IsOnStack(SCENE_ONLINE_MATCHMAKING_DRAFT))
            sLeaveSearch = false;
    s.competitive = sActive;
    s.set_stage = sSetStage;
    s.set_popup = sSetPopup;
    s.set_unseen = Unseen() ? 1 : 0;
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

// Host entry, before original main or between frames: this game is a set's
// (`mii`: the player's own Mii's create ID, 8 bytes): it starts itself into
// the set's RANKED search from the title, the main menu or the online menu,
// whichever comes up next.
extern "C" void mscharged_tweak_set_start(void);
extern "C" __attribute__((visibility("default"))) bool charged_configure_competitive_start(const unsigned char* mii,
                                                                                           unsigned size)
{
    if (!mii || size != sizeof sSetMii) return false;
    std::memcpy(&sSetMii, mii, sizeof sSetMii);
    sSetStage = CHARGED_SET_BOOT;
    sSetPopup = -1;
    sPlayerChoosesMii = false;
    sStartedInGame = GameSceneManager::Instance() != nullptr;
    sLeaveSearch = false;
    mscharged_tweak_set_start();
    return true;
}

// Host entry, at log level debug: DWC's reports go to the log.
extern "C" __attribute__((visibility("default"))) void charged_configure_dwc_report(bool on)
{
    sDwcReport = on ? kDwcReport : 0;
    DWC_SetReportLevel(sDwcReport);
}

// Host entry, between frames: the set is over (or failed) and the host has
// done with it; the game is no set's any more, and its popups and menus are
// the player's again.
extern "C" void mscharged_tweak_set_end(void);
extern "C" __attribute__((visibility("default"))) void charged_competitive_end_set(void)
{
    if (sSetStage == CHARGED_SET_SEARCH)
    {
        GameSceneManager* manager = GameSceneManager::Instance();
        sLeaveSearch = manager && manager->IsOnStack(SCENE_ONLINE_MATCHMAKING_DRAFT);
        if (sLeaveSearch) std::fputs("Competitive: the set ended during its search; the game leaves it\n", stderr);
    }
    sSetStage = CHARGED_SET_NONE;
    sSetPopup = -1;
    sPlayerChoosesMii = false;
    sStartedInGame = false;
    mscharged_tweak_set_end();
}

extern "C" bool mscharged_competitive_leave_search(void)
{
    if (!sLeaveSearch) return false;
    sLeaveSearch = false;
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
    // Not on this computer (a game the launcher did not start for the set):
    // the player chooses the Mii, and the login still goes on into RANKED.
    std::fputs("Competitive: the set's Mii is not in the Mii database; the player chooses one\n", stderr);
    sSetStage = CHARGED_SET_LOGIN;
    sPlayerChoosesMii = true;
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

// A set that went while the player is in the online menu: already logged in,
// it goes on into the RANKED search with the Mii in use.
extern "C" bool mscharged_competitive_take_online_menu(void)
{
    if (sSetStage != CHARGED_SET_BOOT || PopupOnTop()) return false;
    sSetStage = CHARGED_SET_SEARCH;
    return true;
}

extern "C" int mscharged_competitive_connection(unsigned first, unsigned second)
{
    // A set's search meets nobody but the opponent: not before the host's
    // rules are there, nor after the set ended until the search is left.
    if (sLeaveSearch) return 1;
    if (sSetStage != CHARGED_SET_SEARCH) return -1;
    if (!sPending) return 1;
    return IsOpponent(first) || IsOpponent(second) ? 0 : 1;
}

extern "C" int mscharged_competitive_popup(int type)
{
    // Only while the walk drives the screens: not once the set is over or
    // failed, not while the player chooses the Mii, and in a running game not
    // before the walk took the main menu. Never a popup that deletes what the
    // player keeps (the Mii's online profile, a friend).
    if (!OnSet() || sSetStage == CHARGED_SET_OVER || sSetStage == CHARGED_SET_FAILED || sPlayerChoosesMii
        || (sSetStage == CHARGED_SET_BOOT && sStartedInGame) || type == POPUP_PROFILE_UNLINK_WARNING
        || type == POPUP_NETWORK_DELETE_FRIEND)
        return -1;
    switch (type)
    {
    case POPUP_LOW_BATTERY:
    case POPUP_PROFILE_MII_DELETION_WARNING: // a Mii's first time online
    case POPUP_ONLINE_NO_COPY:               // the online save is new
        return 0;
    default:
        std::fprintf(stderr, "Competitive: popup %d ended the set\n", type);
        sSetPopup = type;
        sSetStage = CHARGED_SET_FAILED;
        return 0;
    }
}

// The screens a set's game passes by itself (tweak patch 0048): the title and
// the main menu once the set is on its way; once the main menu went online
// also the Mii list, the Mii's card, the navigation bar and the popups over
// the Mii screens. The Connecting screen and the draft stay. While the walk
// is on one of them, the menu sounds are silent.
extern "C" void mscharged_tweak_set_menus_quiet(bool quiet);
extern "C" bool mscharged_tweak_title_pending(void);
extern "C" bool mscharged_competitive_hides_scene(unsigned long scene)
{
    static const unsigned long kTitle = nlStringLowerHash("art/fe/sms2_start.fen");
    static const unsigned long kMainMenu = nlStringLowerHash("art/fe/main_menu_v3.fen");
    static const unsigned long kMiiSelect = nlStringLowerHash("art/fe/online_mii_select.fen");
    static const unsigned long kMiiCard = nlStringLowerHash("art/fe/online_mii_select_overlay.fen");
    static const unsigned long kNavigation = nlStringLowerHash("art/fe/fe_overlay.fen");
    static const unsigned long kPopup = nlStringLowerHash("art/fe/popup_menu.fen");
    const bool walking = sSetStage >= CHARGED_SET_BOOT && sSetStage <= CHARGED_SET_LOGIN && !sPlayerChoosesMii;
    if (!walking)
    {
        mscharged_tweak_set_menus_quiet(false);
        return false;
    }
    const bool online = sSetStage >= CHARGED_SET_MII; // the main menu went online
    const int top = TopScene();
    GameSceneManager* manager = GameSceneManager::Instance();
    const bool overMii = manager
        && (manager->IsOnStack(SCENE_ONLINE_MII_SELECT) || manager->IsOnStack(SCENE_ONLINE_MII_SELECT_OVERLAY));
    mscharged_tweak_set_menus_quiet(top == SCENE_TITLE || top == SCENE_MAIN_MENU
                                    || (online && top != SCENE_ONLINE_LOGIN && (top != SCENE_POPUP_MENU || overMii)));
    // The title only while the set passes it (a title already passed stays
    // the player's).
    if (scene == kTitle) return mscharged_tweak_title_pending();
    if (scene == kMainMenu) return true;
    if (!online) return false;
    return scene == kMiiSelect || scene == kMiiCard || scene == kNavigation
        || (scene == kPopup && top == SCENE_POPUP_MENU && overMii);
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
