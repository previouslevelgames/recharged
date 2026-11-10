// Competitive play in the game module: the rules the host received for the
// player's accepted match (charged_configure_competitive), applied to a RANKED
// match against that match's opponent through the hooks of tweak patch 0055
// (mscharged_competitive.h), and the game's WFC login for the host
// (charged_wfc_snapshot), which proves it to the profile service.
//
// A set's game (charged_configure_competitive_start: the launcher's Ready to
// play!, or a set that went while the game runs) also starts itself through
// the hooks of tweak patch 0056: the title is passed, the main menu goes
// online, the online Mii menu takes the player's own Mii, the login (or the
// online menu, when the player is already in it) goes on into the RANKED
// search, the connection check accepts the set's opponent (and nobody else),
// and popups are answered; one that is not harmless ends the set. The
// screens it passes by itself are not drawn and their menu sounds are silent
// (tweak patch 0057): the player sees the frontend's camera, the WFC login's
// Connecting screen and the draft. Once the set is over or failed, the host
// ends it (charged_competitive_end_set) and the game stays in its online
// menus; one whose popup ended it (the connection to the opponent's game was
// lost) leaves them for the main menu (tweak patch 0064). Everything else
// stays the retail online flow.
//
// The game shows its players by their Mii names (10 characters, also what
// the network carries). During a set, its two players' rows on the RANKED
// search and draft screen show their profile names instead
// (charged_configure_display_names, tweak patch 0059), made to fit the row;
// nothing sent changes.
//
// A set whose rules have Mirror match lets both players draft one captain
// (tweak patch 0061), and its games dress the away team as the offline tweak
// does: game_tweaks.cpp counts the tweak as on for them.
#if !defined(MSCHARGED_NATIVE) || !defined(MSCHARGED_GAME_MODULE)
#error Competitive play belongs to the isolated original game module
#endif
#include "platform/competitive_rules.h"
#include "platform/menu_snapshot.h"

#include "Game/FE/FrontendSceneCatalog.h"
#include "Game/FE/feText.h"
#include "Game/FE/fePopupMenu.h"
#include "Game/FE/tlTextInstance.h"
#include "Game/GameInfo.h"
#include "Game/GameSceneManager.h"
#include "Game/NetworkDebug.h"
#include "Game/NetworkDraft.h"
#include "Game/NetworkLobby.h"
#include "Game/NetworkMessages.h"
#include "Game/NetworkPeer.h"
#include "Game/NetworkSession.h"
#include "NL/nlFont.h"
#include "NL/nlString.h"
#include "NL/platvmath.h"
#include "mscharged_competitive.h"
#include "mscharged_tweaks.h"

#include <RVLFaceLib/RFL_Database.h>
#include <dwc/dwc_login.h>
#include <dwc/dwc_main.h>
#include <dwc/dwc_report.h>

#include <cstdio>
#include <cstring>
#include <iterator>

extern "C" void mscharged_tweak_set_end(void);

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
// the search (tweak patch 0058), and nobody is met until it has.
bool sLeaveSearch = false;
// The set's popup ended it (its connection to the opponent's game was lost):
// back in the online menu, the game leaves for the main menu (tweak patch
// 0064). Gone with the game's WFC login.
bool sLeaveOnline = false;
// The walk is the Recharged Game Menu's Play RANKED (charged_start_ranked),
// not a set's: it ends where the retail RANKED search begins, against
// anybody, and a popup it doesn't know is the player's to answer.
bool sRankedWalk = false;

// [advanced] log_level = debug: the DWC reports that go to the log: its
// information, errors and warnings, the login, matchmaking (NAT negotiation,
// GT2 connections, QR2 requests, server browser updates), the friend list
// update after the login and the connection test. The game sets the level to
// 0 when its network session starts, so the snapshot sets it again.
constexpr u32 kDwcReport = 0x1 | 0x2 | 0x8 | 0x20 | 0x40 | 0x80 | 0x200 | 0x400 | 0x20000 | 0x40000;
u32 sDwcReport = 0;

bool OnSet() { return sSetStage != CHARGED_SET_NONE; }

// The ranked walk is done (the search began) or stopped: the screens and the
// title are the player's again.
void EndRankedWalk()
{
    sRankedWalk = false;
    sSetStage = CHARGED_SET_NONE;
    sPlayerChoosesMii = false;
    sStartedInGame = false;
    mscharged_tweak_set_end();
}

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

// This machine's side in the online match: 0 home, 1 away; -1 while the
// sides are not set (the draft resets them). StartNetworkedGame gives each
// machine's players a side by their network player id (machine * 4 +
// player): this machine's first player's. The main menu's Online sets only
// mIsOnlineMode, so the mode stays -1 after a fresh start until
// InitializeOnlineMatch: no game info to read yet.
short LocalSide()
{
    const GameInfoManager* info = nlSingleton<GameInfoManager>::Instance();
    if (!info || !info->mIsOnlineMode || !g_pNetworkSession || g_pNetworkSession->mLocalMachineId < 0
        || g_pNetworkSession->mLocalMachineId >= 4 || info->mCurrentMode < 0
        || info->mCurrentMode >= GameInfoManager::GM_NUM_MODES || !info->mGameInfo[info->mCurrentMode])
        return -1;
    const auto player = GetNetworkPlayerId(0, s8(g_pNetworkSession->mLocalMachineId));
    const short side = info->GetPlayingSide((unsigned short)player);
    return side == 0 || side == 1 ? side : -1;
}

// The set's players by their profile names (charged_configure_display_names):
// this player's (0) and the opponent's (1), empty for none. A new pair gets a
// new version.
constexpr int kNameUnits = 32;
unsigned short sNames[2][kNameUnits + 1]{};
unsigned sNamesVersion = 1;

// A name as a text shows it, made for its font (the characters the font has)
// and its room (cut with an ellipsis where even the smallest scale would not
// fit it). The texts point at these buffers until their screen sets them
// again: a buffer is taken again only for another name.
constexpr float kSmallestScale = 0.6f;
constexpr unsigned kMiiDataBytes = 0x4C; // RFLStoreData
struct ShownName
{
    const nlFont* font;
    int who;
    unsigned version;
    unsigned short text[kNameUnits + 4];
};
ShownName sShown[8]{};
unsigned sNextShown = 0;

// A row's name text and the rank text right of it (feOnlinePlayerRow's NAME
// and RANK), as last drawn: where the rank begins and where the name's origin
// is, in the screen's units, and the name's unit there. The name's room ends a
// little before the rank. Only compared with the texts drawn, never read.
constexpr float kRankGap = 10.0f; // in the name's units
struct NameRoom
{
    const void* name;
    const void* rank;
    float rank_left, name_left, name_unit;
    bool rank_seen, name_seen;
};
NameRoom sRooms[16]{};
unsigned sNextRoom = 0;

bool SameMii(const unsigned short* name, const unsigned char* mii, const unsigned short* other_name,
             const void* other_mii)
{
    for (int n = 0; n < 11; ++n)
    {
        if (name[n] != other_name[n]) return false;
        if (!name[n]) break;
    }
    return std::memcmp(mii, other_mii, kMiiDataBytes) == 0;
}

// Which of the set's players a row's Mii (`name`, `mii`: its record) is: 0
// this player, 1 the opponent, -1 none (not in a set's RANKED search, or the
// same Mii on both machines). This player's is the Mii it plays with; the
// opponent's comes from the draft (both machines' Miis and WFC profiles).
int SetPlayer(const unsigned short* name, const unsigned char* mii)
{
    if (sSetStage != CHARGED_SET_SEARCH && sSetStage != CHARGED_SET_PLAYING) return -1;
    if (!RankedOnline()) return -1;
    const bool own = SameMii(name, mii, gNetworkMiiNameWide, &gNetworkMiiData);
    bool opponent = false;
    NetworkDraft* draft = NetworkDraft::Instance();
    if (draft && draft->mState != NET_DRAFT_IDLE)
    {
        const NetMessageDraft& message = draft->GetDraftMessage();
        if (message.mMachineCount == 2)
            for (int machine = 0; machine < 2; ++machine)
            {
                const NetworkDraftMachineInfo& entry = message.mEntries[machine];
                if (entry.mMachineIndex != message.mMachineIndex && IsOpponent(entry.mProfileId)
                    && SameMii(name, mii, entry.mName, entry.mMiiData))
                    opponent = true;
            }
    }
    if (own == opponent) return -1;
    const int who = own ? 0 : 1;
    return sNames[who][0] ? who : -1;
}

const nlFont* FontOf(const TLTextInstance* text)
{
    const auto* component = static_cast<const FEText*>(text->m_component);
    if (!component || !component->m_pFeFontResource || !component->m_pFeFontResource->IsValid()) return nullptr;
    return component->m_pFeFontResource->GetFontReference();
}

bool FontHas(const nlFont* font, unsigned short c)
{
    if (c < 0x20 || c == 0x7F) return false;
    if (c < 0x7F) return font->m_GlyphLookup[c - 0x20].IsValid();
    return font->GetExtendedFontChar(c) != '?';
}

// The width of `text` in one row, in the font's units (as the text box
// measures it); 0 for a text longer than 63 units.
unsigned Width(const nlFont* font, const unsigned short* text)
{
    unsigned short chars[64];
    for (unsigned n = 0; text[n]; ++n)
        if (n + 1 >= std::size(chars)) return 0;
    const FontCharString string(text, font, chars);
    return font->GetStringWidth(string, true, 0, false);
}

bool Equal(const unsigned short* a, const unsigned short* b)
{
    for (; *a == *b; ++a, ++b)
        if (!*a) return true;
    return false;
}

// Player `who`'s name for a text of `font` with `room` for it (its units).
void MakeName(int who, const nlFont* font, float room, unsigned short (&made)[kNameUnits + 4])
{
    // The characters of the font: '{' begins the game's text escapes; what
    // the font lacks (it would draw '?') is left out.
    unsigned length = 0;
    for (const unsigned short* c = sNames[who]; *c && length < kNameUnits; ++c)
    {
        const unsigned short ch = *c == '{' ? '(' : *c == '}' ? ')' : *c;
        if (!FontHas(font, ch)) continue;
        if (ch == ' ' && (length == 0 || made[length - 1] == ' ')) continue;
        made[length++] = ch;
    }
    while (length && made[length - 1] == ' ') --length;
    made[length] = 0;
    // Wider than the smallest scale fits: as much as fits, then the ellipsis.
    if (!length || room <= 0.0f || float(Width(font, made)) <= room / kSmallestScale) return;
    const bool ellipsis = FontHas(font, 0x2026);
    unsigned short cut[kNameUnits + 4];
    for (unsigned kept = length - 1; kept > 0; --kept)
    {
        unsigned n = kept;
        while (n && made[n - 1] == ' ') --n;
        if (!n) break;
        std::memcpy(cut, made, n * sizeof *cut);
        if (ellipsis) cut[n++] = 0x2026;
        else
            for (int dot = 0; dot < 3; ++dot) cut[n++] = '.';
        cut[n] = 0;
        if (float(Width(font, cut)) <= room / kSmallestScale)
        {
            std::memcpy(made, cut, (n + 1) * sizeof *cut);
            return;
        }
    }
}

// The buffer showing player `who`'s name made for `font` and `room`, or null.
const unsigned short* ShownFor(int who, const nlFont* font, float room)
{
    unsigned short made[kNameUnits + 4];
    MakeName(who, font, room, made);
    if (!made[0]) return nullptr;
    for (const ShownName& shown : sShown)
        if (shown.version == sNamesVersion && shown.who == who && shown.font == font && Equal(shown.text, made))
            return shown.text;
    ShownName& shown = sShown[sNextShown++ % std::size(sShown)];
    shown = ShownName{font, who, sNamesVersion, {}};
    std::memcpy(shown.text, made, sizeof made);
    return shown.text;
}

NameRoom* RoomOf(const void* name)
{
    for (NameRoom& room : sRooms)
        if (room.name == name) return &room;
    return nullptr;
}

// The room of a name text: its box, and for a name from the left (the rows')
// at most to the rank beside it, as last drawn.
float RoomFor(const TLTextInstance* name, const NameRoom* room)
{
    const float box = name->m_OverloadedAttributes.BoxSize.x;
    if (!room || !room->rank || !room->rank_seen || !room->name_seen || room->name_unit <= 0.0f
        || (name->m_DrawOptions & 0x3) != nlTextBox::AlignLeft)
        return box;
    const float beside = (room->rank_left - room->name_left) / room->name_unit - kRankGap;
    return beside > 0.0f && beside < box ? beside : box;
}

// A matrix the front end draws flat text with: no perspective.
bool Flat(const nlMatrix4& m) { return m.m14 == 0.0f && m.m24 == 0.0f && m.m34 == 0.0f; }
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
    if (!s.logged_in)
    {
        std::memset(s.login_ticket, 0, sizeof s.login_ticket);
        sLeaveOnline = false;
    }
    // The search screen is gone (the player left it first): nothing to leave.
    if (sLeaveSearch)
        if (GameSceneManager* manager = GameSceneManager::Instance();
            !manager || !manager->IsOnStack(SCENE_ONLINE_MATCHMAKING_DRAFT))
            sLeaveSearch = false;
    s.competitive = sActive;
    s.set_stage = sRankedWalk ? CHARGED_SET_NONE : sSetStage;
    s.ranked_stage = sRankedWalk ? sSetStage : CHARGED_SET_NONE;
    s.set_popup = sSetPopup;
    s.set_unseen = Unseen() ? 1 : 0;
    s.competitive_setups = sSetups;
    s.competitive_games = sActive ? sMatch.games : 0;
    s.local_side = LocalSide();
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
    sRankedWalk = false;
    sSetStage = CHARGED_SET_BOOT;
    sSetPopup = -1;
    sPlayerChoosesMii = false;
    sStartedInGame = GameSceneManager::Instance() != nullptr;
    sLeaveSearch = false;
    sLeaveOnline = false;
    mscharged_tweak_set_start();
    return true;
}

// Host entry, between frames: the Recharged Game Menu's Play RANKED. The set's
// walk without a set: the game goes by itself into the retail RANKED search
// for one player, against anybody, with `mii` (null: the player chooses it).
extern "C" __attribute__((visibility("default"))) bool charged_start_ranked(const unsigned char* mii, unsigned size)
{
    if (OnSet() && !sRankedWalk) return false; // a set's game is on its way
    if (mii && size != sizeof sSetMii) return false;
    if (mii) std::memcpy(&sSetMii, mii, sizeof sSetMii);
    else std::memset(&sSetMii, 0, sizeof sSetMii);
    sRankedWalk = true;
    sSetStage = CHARGED_SET_BOOT;
    sSetPopup = -1;
    sPlayerChoosesMii = false;
    sStartedInGame = GameSceneManager::Instance() != nullptr;
    sLeaveSearch = false;
    sLeaveOnline = false;
    mscharged_tweak_set_start();
    return true;
}

extern "C" __attribute__((visibility("default"))) void charged_stop_ranked(void)
{
    if (sRankedWalk) EndRankedWalk();
}

// Host entry, at log level debug: DWC's reports go to the log.
extern "C" __attribute__((visibility("default"))) void charged_configure_dwc_report(bool on)
{
    sDwcReport = on ? kDwcReport : 0;
    DWC_SetReportLevel(sDwcReport);
}

// Host entry, between frames: the set's players' profile names (UTF-16, at
// most 32 units, the characters the game's screens can show): this player's
// and the opponent's; null or empty for a player who keeps the Mii name. Both
// null: none (the set ended).
extern "C" __attribute__((visibility("default"))) void charged_configure_display_names(const unsigned short* own,
                                                                                       const unsigned short* opponent)
{
    const unsigned short* names[2] = {own, opponent};
    for (int who = 0; who < 2; ++who)
    {
        int n = 0;
        if (names[who])
            for (; n < kNameUnits && names[who][n]; ++n) sNames[who][n] = names[who][n];
        sNames[who][n] = 0;
    }
    ++sNamesVersion;
    for (NameRoom& room : sRooms) room = NameRoom{};
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
    sLeaveOnline = sSetStage == CHARGED_SET_FAILED && !sRankedWalk;
    sSetStage = CHARGED_SET_NONE;
    sSetPopup = -1;
    sPlayerChoosesMii = false;
    sStartedInGame = false;
    charged_configure_display_names(nullptr, nullptr);
    mscharged_tweak_set_end();
}

extern "C" void mscharged_competitive_show_name(void* name, void* rank, const unsigned short* mii_name,
                                                const unsigned char* mii_data)
{
    auto* text = static_cast<TLTextInstance*>(name);
    if (!text || !mii_name || !mii_data) return;
    const int who = SetPlayer(mii_name, mii_data);
    if (who < 0) return;
    const nlFont* font = FontOf(text);
    if (!font) return;
    NameRoom* room = RoomOf(text);
    if (!room)
    {
        room = &sRooms[sNextRoom++ % std::size(sRooms)];
        *room = NameRoom{text};
    }
    // The rank beside it, while shown (a guest's row has none).
    const auto* rankText = static_cast<const TLTextInstance*>(rank);
    const void* beside = rankText && rankText->IsVisible() ? rank : nullptr;
    if (room->rank != beside)
    {
        room->rank = beside;
        room->rank_seen = false;
    }
    if (const unsigned short* shown = ShownFor(who, font, RoomFor(text, room))) text->SetString(shown);
}

extern "C" bool mscharged_competitive_fit_text(void* text, void* matrix)
{
    auto* instance = static_cast<TLTextInstance*>(text);
    if (!instance || !matrix) return false;
    const nlMatrix4& drawn = *static_cast<const nlMatrix4*>(matrix);
    // A rank beside a set player's name: where it begins (from its alignment;
    // one row).
    for (NameRoom& room : sRooms)
        if (room.rank && room.rank == instance)
        {
            const nlFont* font = FontOf(instance);
            const unsigned short* string = instance->GetString();
            room.rank_seen = false;
            if (!font || !string || !Flat(drawn)) continue;
            const float width = float(Width(font, string));
            const unsigned align = unsigned(instance->m_DrawOptions & 0x3);
            const float left = align == nlTextBox::AlignRight ? -width : align == nlTextBox::AlignCenter ? -width / 2.0f
                                                                                                         : 0.0f;
            room.rank_left = left * drawn.m11 + drawn.m41;
            room.rank_seen = width > 0.0f;
        }
    if (instance->m_OverloadFlags & 0x8) return false;
    const ShownName* shown = nullptr;
    for (const ShownName& name : sShown)
        if (instance->m_wcUserString == name.text) shown = &name;
    if (!shown || !shown->text[0]) return false;
    NameRoom* room = RoomOf(instance);
    if (room)
    {
        room->name_left = drawn.m41;
        room->name_unit = drawn.m11;
        room->name_seen = Flat(drawn);
    }
    const nlFont* font = FontOf(instance);
    const float width = float(Width(font, shown->text));
    const float fits = RoomFor(instance, room);
    if (font != shown->font || fits <= 0.0f || width <= fits) return false;
    // Smaller about the text's origin, which its alignment keeps in the box
    // (left, centre or right): the name's one row then spans its room.
    const float scale = fits / width;
    nlMatrix4 smaller, scaled;
    nlMakeScaleMatrix(smaller, scale, scale, 1.0f);
    nlMultMatrices(scaled, smaller, drawn);
    *static_cast<nlMatrix4*>(matrix) = scaled;
    instance->m_DrawOptions |= 0x1000; // one row, not wrapped at the box
    return true;
}

// The draft's choice of the match's simulation profile (tweak patch 0067):
// a set's game meets in RANKED, but puts no points (the WFC server keeps them
// off the boards), so it plays by the rules of a friend match.
extern "C" int mscharged_competitive_set_draft(const NetMessageDraft* message)
{
    if (!message || message->mMachineCount != 2) return 0;
    for (int machine = 0; machine < 2; ++machine)
        if (IsOpponent(message->mEntries[machine].mProfileId)) return 1;
    return 0;
}

extern "C" bool mscharged_competitive_leave_search(void)
{
    if (!sLeaveSearch) return false;
    sLeaveSearch = false;
    return true;
}

// QR2 (tweak patch 0065): while the walk's game searches RANKED, each change
// of its matchmaking state goes to the master server at once, not 10 seconds
// after the last heartbeat: the search waits for its own listing, and the
// opponent's for its reservation. The master server is Recharged's.
extern "C" int mscharged_competitive_announce_now(void)
{
    return sSetStage == CHARGED_SET_SEARCH ? 1 : 0;
}

extern "C" bool mscharged_competitive_leave_online(void)
{
    if (!sLeaveOnline || OnSet() || PopupOnTop()) return false;
    sLeaveOnline = false;
    std::fputs("Competitive: the set lost its connection; the game leaves the online menus\n", stderr);
    return true;
}

extern "C" void mscharged_competitive_search_back(void)
{
    // A set's search the player left: the host withdraws the player's Ready
    // to play! at once, and the opponent's game leaves the set.
    if (sRankedWalk || sSetStage != CHARGED_SET_SEARCH) return;
    sSetStage = CHARGED_SET_LEFT;
    std::fputs("Competitive: the player left the set's search\n", stderr);
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
    // Not on this computer (a game the launcher did not start for the set),
    // or Play RANKED without one: the player chooses the Mii, and the login
    // still goes on into RANKED.
    if (sRankedWalk) std::fputs("Ranked: the player chooses the Mii\n", stderr);
    else std::fputs("Competitive: the set's Mii is not in the Mii database; the player chooses one\n", stderr);
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
    // The ranked walk ends where the retail search begins.
    if (sRankedWalk) EndRankedWalk();
    else sSetStage = CHARGED_SET_SEARCH;
    return true;
}

// A set that went while the player is in the online menu: already logged in,
// it goes on into the RANKED search with the Mii in use.
extern "C" bool mscharged_competitive_take_online_menu(void)
{
    if (sSetStage != CHARGED_SET_BOOT || PopupOnTop()) return false;
    if (sRankedWalk) EndRankedWalk();
    else sSetStage = CHARGED_SET_SEARCH;
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

namespace {
// A set's answer to the game's popup `type`, -1 for the player's own.
int SetPopupAnswer(int type)
{
    // Only while the walk drives the screens: not once the set is over,
    // failed or left, not while the player chooses the Mii, and in a running game not
    // before the walk took the main menu. Never a popup that deletes what the
    // player keeps (the Mii's online profile, a friend).
    if (!OnSet() || sSetStage == CHARGED_SET_OVER || sSetStage == CHARGED_SET_FAILED || sSetStage == CHARGED_SET_LEFT
        || sPlayerChoosesMii
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
        // Play RANKED: the walk stops, and the player answers.
        if (sRankedWalk)
        {
            std::fprintf(stderr, "Ranked: popup %d stops the walk to RANKED\n", type);
            EndRankedWalk();
            return -1;
        }
        std::fprintf(stderr, "Competitive: popup %d ended the set\n", type);
        sSetPopup = type;
        sSetStage = CHARGED_SET_FAILED;
        return 0;
    }
}
} // namespace

extern "C" int mscharged_competitive_popup(int type)
{
    const int answer = SetPopupAnswer(type);
    // Else the network popup of a match the player forfeited (Recharged's
    // match menu): answered for them, back to the menus.
    return answer < 0 && ChargedMenuAnswersPopup(type) ? 0 : answer;
}

// The screens a set's game passes by itself (tweak patch 0057): the title and
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

// The set's rules have Mirror match and this RANKED match's draft is against
// the set's opponent: the captain the other player picked stays available.
// The host's rules, as the match is not set up yet.
extern "C" bool mscharged_competitive_draft_mirror(void)
{
    return Applies() && ((sRules.tweaks >> MSCHARGED_TWEAK_MIRROR_MATCH) & 1u);
}

// Mirror match for the tweak's hooks (game_tweaks.cpp): from the set's draft
// (both teams may then have one captain) through the screens after its
// match, which keeps the rules it started with. Only in RANKED: once the
// game leaves online play, Mirror match is the player's own setting again.
extern "C" bool mscharged_competitive_mirror_match(void)
{
    if (!RankedOnline()) return false;
    return mscharged_competitive_draft_mirror()
        || (sActive && ((sMatch.tweaks >> MSCHARGED_TWEAK_MIRROR_MATCH) & 1u));
}

// The side whose team wears the away kit in a mirror match (tweak patch
// 0062). Offline, one screen shows both teams: the away team's. In a set's
// mirror match each player sees their own team in its usual kit (the
// players' choice, 2026-11-02): from the match's start the opponent's side.
// In the draft, whose screens show this player's team on side 0, side 1.
extern "C" int mscharged_competitive_mirror_dressed_side(void)
{
    if (!mscharged_competitive_mirror_match()) return 1;
    const NetworkDraft* draft = NetworkDraft::Instance();
    if (draft && draft->mState >= NET_DRAFT_CAPTAINS && draft->mState <= NET_DRAFT_FINAL_COUNTDOWN) return 1;
    const short side = LocalSide();
    return side == 0 || side == 1 ? 1 - side : 1;
}

extern "C" bool mscharged_competitive_cheats(int* stadium, int* player, int* powerup)
{
    if (!sActive) return false;
    if (stadium) *stadium = sMatch.stadium_cheat;
    if (player) *player = sMatch.player_cheat;
    if (powerup) *powerup = sMatch.powerup_cheat;
    return true;
}

extern "C" int mscharged_competitive_stadium(int stadium)
{
    return Applies() && sRules.stadium >= 0 && sRules.stadium <= STAD_SAND_TOMB ? sRules.stadium : stadium;
}
