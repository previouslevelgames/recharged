// The trophy room (see trophy_room.h). The game's cut online tournament
// (NetTournManager) entered it from the online menu with its fe_presentation
// function TransitionOnlineMenuToTournament: the cup trophies hidden (native
// 7), the camera trophycentreofgold in its loop (23, 35), the tournament's
// trophy loaded on the stadium's marker 4 (13, 51).
// TransitionOnlineTournamentToMenu undid it: the trophy destroyed (3), the cup
// trophies shown (36), the camera back to fechoosecaptains, the online menu's
// (23). Neither pushes a scene, so the online menu stays on top. The trophy
// room does the same calls itself, which lets it choose how long the camera
// takes.
#if !defined(MSCHARGED_NATIVE) || !defined(MSCHARGED_GAME_MODULE)
#error The trophy room belongs to the isolated original game module
#endif
#include "platform/trophy_room.h"

#include "platform/menu_snapshot.h"

#include "Game/BaseGameSceneManager.h"
#include "Game/DB/CupInterface.h"
#include "Game/Drawable/DrawableObj.h"
#include "Game/Camera/CameraMan.h"
#include "Game/Camera/animcam.h"
#include "Game/FE/feCamera.h"
#include "Game/FE/feCupFlow.h"
#include "Game/GameSceneManager.h"
#include "Game/NetTournManager.h"
#include "Game/Render/FrontEndPresentation.h"
#include "Game/Render/StadiumLoading.h"
#include "Game/SH/SHNavigation.h"
#include "Game/SH/SHOnlineHub.h"
#include "NL/gl/glMaterialParameters.h"
#include "NL/gl/glMemory.h"
#include "NL/gl/glModel.h"
#include "NL/nlMemory.h"
#include "NL/nlString.h"
#include "mscharged_tweaks.h"

#include <cstdio>

// The game being played (null in the front end).
class cGame;
extern cGame* g_pGame;
// The tournament trophy's texture bundle and model, loading (StadiumLoading.cpp).
extern StadiumLoadResult gTournamentTrophyLoadResults[2];

namespace {
// The cut tournament's cameras: the trophy, and the online menu's.
constexpr char kTrophyCamera[] = "trophycentreofgold";
constexpr char kOnlineMenuCamera[] = "fechoosecaptains";
// How long the camera takes, in seconds: as the Road to Strikers Cup hub's
// (TransitionToStrikerCupHub), where the cut tournament cut (0).
constexpr float kCameraTime = 2.0f;

int sState = CHARGED_TROPHY_ROOM_OUT;
// The host asked to leave on the way in.
bool sLeaveWanted = false;
// The cut tournament's own persona, given back on the way out (it only
// chooses the trophy while the tournament is inactive).
int sSavedPersona = 0;
// The camera got where it was pushed; the trophy is loaded and in the world.
bool sCameraArrived = false;
bool sTrophyLoaded = false;
// A trophy still loading when the online menu left: its data is freed once
// it arrives, without putting it in the world.
bool sDiscardLoad = false;
// The set's search going back to the room is logged once a set.
bool sSearchBackSaid = false;
// The set's search screen goes back to the room (tweak patch 0075): it
// stays in the online menu's place a moment after the set ended (it leaves
// at its next update, tweak patch 0058).
bool sSearchFromRoom = false;

void OnCameraArrived(eCameraMessage) { sCameraArrived = true; }

// The trophy room's log line, with the resource pool's free memory: in and
// out ten times over, "out" must show the same figure each time.
void Log(const char* what)
{
    GLResourcePool* pool = glGetCurrentResourcePool();
    std::fprintf(stderr, "Trophy room: %s (pool free %lu)\n", what, pool ? pool->GetFreeMemory() : 0ul);
}

// As the script's native 23 (and 35 when `loop`): the presentation camera
// `name`, in place of the current one.
void PushCamera(const char* name, bool loop)
{
    sCameraArrived = kCameraTime == 0.0f;
    PushPresentationCamera(name, OnCameraArrived, kCameraTime, true);
    if (auto* camera = static_cast<cAnimCamera*>(cCameraManager::PeekCamera())) camera->m_bCyclic = loop;
}

SHOnlineHub* TopOnlineHub()
{
    GameSceneManager* manager = GameSceneManager::Instance();
    if (!manager) return nullptr;
    const unsigned long depth = manager->mCurrentStackDepth;
    if (depth == 0 || depth > BaseGameSceneManager::MAX_SCENE_DEPTH) return nullptr;
    if (manager->m_sceneStack[depth - 1] != SCENE_ONLINE_MENU) return nullptr;
    return static_cast<SHOnlineHub*>(manager->mBaseSceneHandlerStack[depth - 1]);
}

void Out()
{
    if (NetTournManager* tournament = NetTournManager::Instance()) tournament->mCupPersona = sSavedPersona;
    sState = CHARGED_TROPHY_ROOM_OUT;
    sLeaveWanted = false;
}

// The tournament trophies are skinned models (each has its skeleton and
// animation on the disc: art/animation/trophy*.shier and .cph), yet the
// tournament draws one as a static PlanarShadowDrawable that nothing poses.
// Their packets carry per-vertex matrix indices (stream 7); with no
// SkinMatrices bound their material loads no bone matrices, the vertices
// take whatever the last draw left in the matrix slots, and the trophy
// lands off the picture. Bind the rest pose instead, as ShaderSkinMesh
// binds a character's bones: identity bone matrices in the nine slots
// glx_LoadSkinMatrices fills (GX_PNMTX1 to GX_PNMTX9).
const float kRestPose[9][3][4] = {
    {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}}, {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}},
    {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}}, {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}},
    {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}}, {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}},
    {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}}, {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}},
    {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}},
};

void PoseTrophy()
{
    static const unsigned long skinMatrices = nlStringLowerHash("SkinMatrices");
    NetTournManager* tournament = NetTournManager::Instance();
    auto* trophy = tournament ? static_cast<DrawableObject*>(tournament->mTrophyPresentation) : nullptr;
    if (!trophy || !trophy->m_pModel) return;
    for (unsigned long n = 0; n < trophy->m_pModel->numPackets; ++n) {
        glModelPacket* packet = &trophy->m_pModel->packets[n];
        if (!glModelPacketGetStream(packet, 7) || !glHasMaterialParameter(packet, skinMatrices)) continue;
        glSetMaterialBufferParameter(packet, skinMatrices, reinterpret_cast<glMaterialBufferAddress>(kRestPose),
                                     sizeof kRestPose);
    }
}

void StartLeaving()
{
    // The trophy stays in sight while the camera moves away (ChargedTrophyRoomFrame).
    PushCamera(kOnlineMenuCamera, false);
    sState = CHARGED_TROPHY_ROOM_LEAVING;
    sLeaveWanted = false;
    Log("leaving");
}
} // namespace

bool ChargedTrophyRoomCanEnter()
{
    if (sState != CHARGED_TROPHY_ROOM_OUT || sDiscardLoad || ChargedCompetitiveWalking()) return false;
    SHOnlineHub* hub = TopOnlineHub();
    FrontEndPresentation* presentation = FrontEndPresentation::GetInstance();
    NetTournManager* tournament = NetTournManager::Instance();
    return hub && hub->mState == ONLINE_HUB_READY && presentation && !presentation->IsActive() && tournament
        && tournament->mState == NET_TOURN_INACTIVE;
}

bool ChargedTrophyRoomEnter(int persona)
{
    if (!ChargedTrophyRoomCanEnter()) return false;
    NetTournManager* tournament = NetTournManager::Instance();
    if (persona < CUP_PERSONA_MUSHROOM || persona > CUP_PERSONA_NINTENDO) persona = CUP_PERSONA_MUSHROOM;
    sSavedPersona = tournament->mCupPersona;
    tournament->mCupPersona = persona;
    // The navigation's Log Out goes with the online menu.
    if (SHNavigation* navigation = GetNavigationScene()) navigation->HideButtons();
    SetCupTrophiesVisible(false);
    PushCamera(kTrophyCamera, true);
    sTrophyLoaded = false;
    BeginLoadTournamentTrophy();
    sState = CHARGED_TROPHY_ROOM_ENTERING;
    std::fprintf(stderr, "Trophy room: entering (trophy %d)\n", persona);
    return true;
}

void ChargedTrophyRoomLeave()
{
    if (sState == CHARGED_TROPHY_ROOM_ENTERING) sLeaveWanted = true;
    else if (sState == CHARGED_TROPHY_ROOM_IN) StartLeaving();
}

void ChargedTrophyRoomFrame()
{
    if (!ChargedCompetitiveWalking()) sSearchBackSaid = false;
    if (GameSceneManager* manager = GameSceneManager::Instance();
        !ChargedCompetitiveWalking() && (!manager || !manager->IsOnStack(SCENE_ONLINE_MATCHMAKING_DRAFT)))
        sSearchFromRoom = false;
    if (sDiscardLoad && IsTournamentTrophyLoaded()) {
        for (StadiumLoadResult& result : gTournamentTrophyLoadResults) {
            nlFree(result.mData);
            result.mData = nullptr;
        }
        sDiscardLoad = false;
    }
    if (sState == CHARGED_TROPHY_ROOM_OUT) return;
    GameSceneManager* manager = GameSceneManager::Instance();
    // The front end went into a game (DestroyFrontEndState frees its scenes,
    // stadium and models at once, no frame between): the stadium freed the
    // trophy with its other drawables, and the cup trophies went with the
    // world. As the cut tournament (its native 0), the trophy is only let go.
    if (!manager || g_pGame) {
        if (NetTournManager* tournament = NetTournManager::Instance()) tournament->DetachTournamentTrophy();
        sDiscardLoad = sState == CHARGED_TROPHY_ROOM_ENTERING && !sTrophyLoaded;
        Out();
        Log("the front end went into a game; the trophy went with its stadium");
        return;
    }
    // The online menu left the stack meanwhile (its own buttons still work
    // in the trophy room): its next screen has its own camera, the trophy
    // and the hidden cup trophies are put right here. A popup over it is
    // fine, and so is a set's walk: its RANKED search takes the online
    // menu's place (tweak patch 0056) and is played from the room, until the
    // set ends (back in the online menu after a search left, also the
    // search of a set called off meanwhile).
    if (!manager->IsOnStack(SCENE_ONLINE_MENU) && !ChargedCompetitiveWalking() && !sSearchFromRoom) {
        if (NetTournManager* tournament = NetTournManager::Instance()) tournament->DestroyTournamentTrophy();
        sDiscardLoad = sState == CHARGED_TROPHY_ROOM_ENTERING && !sTrophyLoaded;
        SetCupTrophiesVisible(true);
        Out();
        char line[96];
        const unsigned long depth = manager->mCurrentStackDepth;
        std::snprintf(line, sizeof line, "the online menu left (top scene %d); trophy removed",
                      depth > 0 && depth <= BaseGameSceneManager::MAX_SCENE_DEPTH ? int(manager->m_sceneStack[depth - 1])
                                                                                  : -1);
        Log(line);
        return;
    }
    if (sState == CHARGED_TROPHY_ROOM_ENTERING) {
        if (!sTrophyLoaded && IsTournamentTrophyLoaded()) {
            FinishLoadTournamentTrophy();
            PoseTrophy();
            sTrophyLoaded = true;
        }
        if (!sTrophyLoaded || !sCameraArrived) return;
        sState = CHARGED_TROPHY_ROOM_IN;
        Log("in");
        if (sLeaveWanted) StartLeaving();
    } else if (sState == CHARGED_TROPHY_ROOM_LEAVING && sCameraArrived) {
        if (NetTournManager* tournament = NetTournManager::Instance()) tournament->DestroyTournamentTrophy();
        SetCupTrophiesVisible(true);
        // The online menu's navigation again, as SHOnlineHub sets it.
        if (SHNavigation* navigation = GetNavigationScene()) {
            navigation->SetButtons(NAVIGATION_BUTTON_BACK, true);
            navigation->SetBackButtonText(NAV_LOG_OUT);
        }
        Out();
        Log("out");
    }
}

int ChargedTrophyRoomState() { return sState; }

#if defined(MSCHARGED_TWEAKS)
// Tweak patch 0073: the game's online menu waits while the room shows.
bool mscharged_tweak_online_menu_held(void) { return sState != CHARGED_TROPHY_ROOM_OUT; }

// Tweak patch 0074: a set's match goes into its game from the trophy, as the
// cut tournament's did (its loading scene plays TransitionOnlineTournamentToGame,
// whose last call lets the trophy go: the stadium frees it with the world).
bool mscharged_tweak_trophy_room_to_game(void)
{
    if (sState != CHARGED_TROPHY_ROOM_IN || !ChargedCompetitiveWalking()) return false;
    Log("the set's match goes in from the trophy");
    return true;
}

// Tweak patch 0075: a set's search from the room goes back to the online
// menu, whose room is still there (not to the RANKED screen, out of it).
int mscharged_tweak_search_back_scene(int scene)
{
    if (sState != CHARGED_TROPHY_ROOM_IN || !ChargedCompetitiveWalking()) return scene;
    // (The search's screen asks on each of its makings: said once a set.)
    if (!sSearchBackSaid) Log("the set's search goes back to the room");
    sSearchBackSaid = true;
    sSearchFromRoom = true;
    return SCENE_ONLINE_MENU;
}
#endif
