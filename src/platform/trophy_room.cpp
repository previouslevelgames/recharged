// The trophy room (see trophy_room.h). The game's cut online tournament
// (NetTournManager) entered it from the online menu with
// TransitionOnlineMenuToTournament: the cup trophies hidden, the camera
// trophycentreofgold in its loop, the tournament's trophy loaded on the
// stadium's marker 4. TransitionOnlineTournamentToMenu undoes it: the trophy
// destroyed, the cup trophies shown, the camera back to fechoosecaptains (the
// online menu's). Neither pushes a scene, so the online menu stays on top.
#if !defined(MSCHARGED_NATIVE) || !defined(MSCHARGED_GAME_MODULE)
#error The trophy room belongs to the isolated original game module
#endif
#include "platform/trophy_room.h"

#include "platform/menu_snapshot.h"

#include "Game/BaseGameSceneManager.h"
#include "Game/DB/CupInterface.h"
#include "Game/Drawable/DrawableObj.h"
#include "Game/FE/feCupFlow.h"
#include "Game/GameSceneManager.h"
#include "Game/NetTournManager.h"
#include "Game/Render/FrontEndPresentation.h"
#include "Game/SH/SHOnlineHub.h"
#include "NL/gl/glMaterialParameters.h"
#include "NL/gl/glModel.h"
#include "NL/nlString.h"

#include <cstdio>

namespace {
int sState = CHARGED_TROPHY_ROOM_OUT;
// The host asked to leave on the way in.
bool sLeaveWanted = false;
// The cut tournament's own persona, given back on the way out (it only
// chooses the trophy while the tournament is inactive).
int sSavedPersona = 0;

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
    FrontEndPresentation::GetInstance()->Call("TransitionOnlineTournamentToMenu");
    sState = CHARGED_TROPHY_ROOM_LEAVING;
    sLeaveWanted = false;
    std::fputs("Trophy room: leaving\n", stderr);
}
} // namespace

bool ChargedTrophyRoomEnter(int persona)
{
#if defined(MSCHARGED_DIAGNOSTIC_FRONTEND_SEQUENCE) && !defined(MSCHARGED_DIAGNOSTIC_FRONTEND_TROPHY_ROOM)
    // The script's trophy natives are not admitted in this build (decomp
    // patch 0765): the frontend sequence diagnostic would throw on them.
    (void)persona;
    std::fputs("Trophy room: not admitted in this build\n", stderr);
    return false;
#else
    if (sState != CHARGED_TROPHY_ROOM_OUT) return false;
    SHOnlineHub* hub = TopOnlineHub();
    FrontEndPresentation* presentation = FrontEndPresentation::GetInstance();
    NetTournManager* tournament = NetTournManager::Instance();
    if (!hub || hub->mState != ONLINE_HUB_READY || presentation->IsActive() || !tournament
        || tournament->mState != NET_TOURN_INACTIVE)
        return false;
    if (persona < CUP_PERSONA_MUSHROOM || persona > CUP_PERSONA_NINTENDO) persona = CUP_PERSONA_MUSHROOM;
    sSavedPersona = tournament->mCupPersona;
    tournament->mCupPersona = persona;
    presentation->Call("TransitionOnlineMenuToTournament");
    sState = CHARGED_TROPHY_ROOM_ENTERING;
    std::fprintf(stderr, "Trophy room: entering (trophy %d)\n", persona);
    return true;
#endif
}

void ChargedTrophyRoomLeave()
{
    if (sState == CHARGED_TROPHY_ROOM_ENTERING) sLeaveWanted = true;
    else if (sState == CHARGED_TROPHY_ROOM_IN && !FrontEndPresentation::GetInstance()->IsActive()) StartLeaving();
}

void ChargedTrophyRoomFrame()
{
    if (sState == CHARGED_TROPHY_ROOM_OUT) return;
    // The online menu left the stack meanwhile (its own buttons still work
    // in the trophy room): its next screen has its own camera, the trophy
    // and the hidden cup trophies are put right here. A popup over it is
    // fine.
    GameSceneManager* manager = GameSceneManager::Instance();
    if (!manager || !manager->IsOnStack(SCENE_ONLINE_MENU)) {
        if (NetTournManager* tournament = NetTournManager::Instance()) tournament->DestroyTournamentTrophy();
        SetCupTrophiesVisible(true);
        Out();
        std::fputs("Trophy room: the online menu left; trophy removed\n", stderr);
        return;
    }
    if (FrontEndPresentation::GetInstance()->IsActive()) return;
    if (sState == CHARGED_TROPHY_ROOM_ENTERING) {
        sState = CHARGED_TROPHY_ROOM_IN;
        PoseTrophy();
        std::fputs("Trophy room: in\n", stderr);
        if (sLeaveWanted) StartLeaving();
    } else if (sState == CHARGED_TROPHY_ROOM_LEAVING) {
        Out();
        std::fputs("Trophy room: out\n", stderr);
    }
}

int ChargedTrophyRoomState() { return sState; }
