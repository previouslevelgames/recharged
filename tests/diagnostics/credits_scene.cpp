// Bounded diagnostic entry into genuine source scene services. Every loading,
// completion, authored component, text/visibility/update and draw decision
// below is made by the original reconstructed TUs. No main/task-loop claim.
#include "Game/BaseGameSceneManager.h"
#include "Game/PadActions.h"
#include "Game/FE/feInput.h"
#include "NL/globalpad.h"
#include "NL/gl/gl.h"
#include "Game/Render/RLViewLayers.h"
#include "NL/gl/glStruct.h"
#include "NL/glx/glxTarget.h"
#include "Game/FE/fePackage.h"
#include "Game/FE/fePresentation.h"
#include "Game/FE/feResourceManager.h"
#include "Game/FE/feSceneManager.h"
#include "Game/FE/tlSlide.h"
#include "Game/FE/tlTextInstance.h"
#include "Game/Font/fontmanager.h"
#include "Game/SH/SHCredits.h"
#include "Game/Task/GameRenderTask.h"
#include "Game/Sys/movie.h"
#include "NL/glx/glxSwap.h"
#include "NL/glx/GXMovieMaterialProgram.h"
#include "NL/gl/glMaterialProgram.h"
#include "NL/gl/glMatrix.h"
#include "NL/gl/glMemory.h"
#include "NL/gl/glState.h"
#include "NL/gl/glView.h"
#include "NL/glx/glxMemory.h"
#include "NL/glx/glxSend.h"
#include "NL/glx/glxGX.h"
#include "NL/glx/GXMaterialProgramRegistry.h"
#include "NL/gl/glPlat.h"
#include "NL/glx/GXVertexColourTextureMaterialProgram.h"
#include "NL/glx/GXFloatTexturedColourMaterialProgram.h"
#include "NL/glx/GXScissoredVertexColourTextureMaterialProgram.h"
#include "NL/glx/GXUnlitTextureMaterialProgram.h"
#include "NL/nlFileGC.h"
#include "NL/nlMemory.h"
#include "platform/game_allocation_ownership.h"
#include <dolphin/gx.h>
#include <array>
#include <cmath>
#include <chrono>
#include <cstdio>
#include <stdexcept>
#include <thread>

#if !defined(MSCHARGED_DIAGNOSTIC_CREDITS_SCENE)
#error This is an explicitly temporary source scene diagnostic
#endif
#if defined(MSCHARGED_DIAGNOSTIC_VIEWS) || defined(MSCHARGED_DIAGNOSTIC_GL_STATE) || defined(MSCHARGED_DIAGNOSTIC_MATRICES)
#error Legacy replacement render policies are not scene providers
#endif
namespace {
unsigned checks, packets, vertices;
bool permanentComplete;
GLView* retainedSceneView;
FESceneManager* retainedSceneManager;
FEResourceManager* retainedResourceManager;
CreditScene* retainedCredits;
unsigned liveFrames;
float firstLineBeforeLive;
void Check(bool value, const char* reason) { ++checks; if (!value) throw std::runtime_error(reason); }
void PermanentComplete() { permanentComplete = true; }
void Observe(GLView* view, unsigned long flags, const glModelPacket* packet)
{
    if (packet && (flags & 0x80)) {
        ++packets;
        vertices += packet->numUniqueVertices;
        for (unsigned i = 0; i < packet->numStreams; ++i) {
            const auto range = mscharged::platform::ResolveGameGraphicsArray(packet->streams[i].address);
            Check(range.bytes == packet->numUniqueVertices * packet->streams[i].stride,
                  "Source scene procedural writer extent differs");
        }
    }
    // Metadata-only source packet traversal. Issuing actual GX here inside a
    // saved SDK display-list context would mutate original game caches without
    // executing those commands; first real GX draw belongs to the host frame.
    (void)view;
}
}

unsigned DrawActualCredits231(FontManager& fonts, GLResourcePool* fontPool)
{
    auto* first = *fonts.m_fonts.Begin();
    Check(first != nullptr, "Actual source fonts not completed");
    Check(glxInitMemory(256*1024, 256*1024), "Actual original source frame memory unavailable");
    // Actual original registry owns persistent nlMalloc/placement-new program
    // objects. Stack provider objects would die before the native host frames.
    // The named diagnostic retains the four original constructor requests;
    // normal startup retains all43 and this is not full startup acceptance.
    glInitMaterialPrograms();
    // The actual whole glPlat startup owns VI/FIFO/XFB/GX/cache initialization
    // and installs source vi_post_cb: this is the movie's genuine frame clock.
    // The named host diagnostic still brackets EFB rendering without source
    // task scheduling or VI display-copy/swap presentation.
    Check(glplatStartup(glGetScreenInfo()), "Actual source glPlat startup failed");
    Check(glplatPostStartup(), "Actual source glPlat post-startup failed");
    gl_StateStartup();
    gl_MatrixStartup();
    glplatInitializeMaterialPrograms(); // Actual source registry traversal.
    Check(GXVertexColourTextureMaterialProgram::Initialized &&
          GXFloatTexturedColourMaterialProgram::Initialized,
          "Original source registry did not initialize material providers");
    Check(mscharged::platform::FindGameAllocationOwner(GXVertexColourTextureMaterialProgram::Instance) &&
          mscharged::platform::FindGameAllocationOwner(GXFloatTexturedColourMaterialProgram::Instance),
          "Original source registry material providers lack persistent game allocations");

    auto* resources = new (8, false) FEResourceManager;
    FEResourceManager::s_pInstance = resources;
    // Exact original large FE pool, not a larger guessed budget or asset owner.
    CreateLargeFEResourcePool();
    Check(resources->GetResourcePool() == GetFEResourcePool(), "Source FE pool selection differs");
    resources->LoadPermanentResourceBundle("art/fe/MainUI.Dmn", PermanentComplete);
    auto end = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (!permanentComplete) {
        nlServiceFileSystem();
        if (std::chrono::steady_clock::now() > end)
            throw std::runtime_error("Actual original MainUI callback stalled");
        std::this_thread::yield();
    }
    Check(permanentComplete, "Original permanent bundle never completed");
    std::puts("231 genuine MainUI permanent source callback complete");

    Check(glGetScreenInfo()->ScreenWidth == 640 && glGetScreenInfo()->ScreenHeight == 448,
          "Actual source glPlat screen object differs from source defaults");
    gl_TargetStartup();
    SetupViews(); // Named diagnostic creates the genuine source Anark only.
    nlMatrix4 identity; identity.SetIdentity();
    fn_80273144(identity, identity, 4.0f/3.0f, 0.4712389f, 4.0f/3.0f, 0.4712389f);
    RLView* sourceView = GetLayerView(eCLV_Anark);
    Check(sourceView && sourceView->m_Interface == &sOrthoCenteredCamera,
          "Original SetupViews omitted actual Anark camera");
    Check(sourceView->m_Parent == &gRootView && !sourceView->m_Enabled && sourceView->m_Visible,
          "Original Anark parent/initial state/tweak visibility changed");
    Check(sourceView->m_Unknown48 == eCLV_Anark && sourceView->m_RenderPair.target == glplatGetBackBufferTarget(),
          "Original Anark layer/display selection changed");
    const auto* projection = sourceView->m_Interface->GetProjectionMatrix();
    Check(projection->m11 == 2.0f/640.0f && projection->m22 == 2.0f/480.0f,
          "Actual source Anark projection is not original centered640x480");
    GLView& view = *sourceView;
    // Actual original pre-FE logical pad initialization, without installing or
    // fabricating a physical backend. InitPads leaves all eight mBackend=0.
    // FEInput's original constructor resets the true source analog-map fields.
    InitPads();
    Check(g_pPadManager && g_pPadManager->mPadCount==4 && g_pPadManager->mPadSetCount==2,
          "Source InitPads did not create its original logical pad sets");
    for(int set=0;set<2;++set) {
        g_pPadManager->SetActivePadSet(set);
        for(int pad=0;pad<4;++pad)
            Check(g_pPadManager->GetPad(pad)->mBackend==nullptr,
                  "Logical pad prerequisite installed an unexpected physical backend");
    }
    g_pPadManager->SetActivePadSet(0);
    FEInput::Initialize();
    Check(g_pFEInput && g_pFEInput->m_InputLockDepth==0 && g_pFEInput->m_bInputAllowed,
          "Original FEInput constructor/focus initialization changed");
    auto* scenes = new (8, false) FESceneManager;
    FESceneManager::s_pInstance = scenes;
    scenes->m_uDefaultRenderView = reinterpret_cast<glViewHandle>(&view);
    auto* credits = new (8, false) CreditScene;
    Check(credits->mPhase == 2, "Temporary Credits scene scope did not select phase2");
    scenes->QueueScenePush(credits, "art/fe/credits.fen", CurrentAllocator);
    Check(!scenes->AreAllScenesValid(), "Scene queue fabricated package readiness");
    scenes->ForceImmediateStackProcessing();
    Check(credits->mFEScene && credits->mFEScene->mState == 1,
          "Source async package unexpectedly ready before callback");
    end = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (!scenes->AreAllScenesValid()) {
        nlServiceFileSystem();
        resources->Run(0);
        if (std::chrono::steady_clock::now() > end)
            throw std::runtime_error("Actual source Credits resource completion stalled");
        std::this_thread::yield();
    }
    Check(credits->mFEScene->mState == 6, "Actual source resource manager did not finish owned FEN");
    Check(credits->mCreditParser.mFileData && credits->mCreditParser.mFileSize,
          "Actual Credits SceneCreated omitted original credits.txt load");
    Check(credits->mPresentation == credits->mFEScene->m_pFEPackage->GetPresentation(),
          "Source scene callback did not initialize actual handler presentation");
    Check(credits->m_pTextLines[0] != nullptr && credits->m_pTextLines[19] != nullptr,
          "Source Credits setup did not find original20 line instances");
    Check(credits->m_pTextLines[0]->GetAssetPosition().f.y == -250.0f &&
          credits->m_pTextLines[19]->GetAssetPosition().f.y == -725.0f,
          "Original authored line setup changed");
    // Exact source renderer callback creates GXMovie packets for the authored
    // movie image. The source MovieInit entry selects real THPSimple mode0 only
    // under the explicitly named diagnostic while normal AX mode1 is held.
    InstallImageRenderCallback();
    Check(MovieInit(), "Actual original MovieInit rejected its request");
    for (unsigned frame=0; frame<120; ++frame)
        scenes->Update(1.0f/60.0f);
    Check(credits->mMovieStarted && credits->mSwappedTexture && credits->mMovieInstance,
          "Original Credits parent movie source did not start/swap its authored image");
    Check(IsMovieActive() && !IsMovieFinished(), "Actual source MovieStart/preload did not remain active");
    std::printf("Actual source Credits movie selected %s, initial retrace frame%d.\n",
                credits->mMovieFilename, glxGetFrameCount());
    Check(credits->m_pTextLines[0]->GetString() == credits->mStrings[0] && credits->mStrings[0][0],
          "Original parser/handler did not publish genuine first Credits string");
    scenes->RenderActiveScenes();
    Check(!view.m_Enabled, "Original Anark alpha-write permission changed during packet attachment");
    retainedSceneView = &view;
    retainedSceneManager = scenes;
    retainedResourceManager = resources;
    retainedCredits = credits;
    firstLineBeforeLive = credits->m_pTextLines[0]->GetAssetPosition().f.y;
    view.Iterate(Observe);
    Check(packets && vertices, "Actual original Credits renderer omitted source packets");
    Check(gxGetNumTexGens() == 0, "GPU fixture issued an unintended source GX request before first frame");
    std::printf("231 owned original Credits: %u checks, %u source packets, %u vertices. Temporary phase2/mode0/world/input gates; genuine Anark/source projection; metadata-only packet observation before first GPU frame.\n", checks,packets,vertices);
    // Keep live source scene/font/pool owners until terminal diagnostic exit.
    // No missing FESceneManager destructor or CRT cleanup is invented.
    (void)fontPool;
    return checks;
}

extern "C" __attribute__((visibility("default"))) void charged_scene_update_and_render_frame(float delta)
{
    Check(std::isfinite(delta) && delta>=0, "Invalid native diagnostic frame delta");
    if (!retainedSceneView) throw std::logic_error("Original owned Credits scene not loaded");
    // True VI retrace callbacks own the source movie clock. The native frame
    // fixture still omits source VI scanout/swap and the full task loop, calling
    // actual FE endpoints in DrawFrontEndElements order. MoviePlay is the
    // original nlTaskManager post-Run movie endpoint, selected diagnostically.
    // The preceding native frame has ended/drained before reusing frame backing.
    gl_ViewReset();
    glplatFrameAllocNextFrame();
    glBeginFrame();
    nlServiceFileSystem();
    retainedResourceManager->Run(delta);
    retainedSceneManager->Update(delta);
    Check(g_pFEInput->m_bInputAllowed,
          "Original scene-manager focus did not dispatch the active Credits handler");
    retainedSceneManager->RenderActiveScenes();
    MoviePlay();
    glEndFrame();
    retainedSceneView->Iterate(glx_SendFrame_cb);
    glx_SendEnd();
    ++liveFrames;
    const auto y=retainedCredits->m_pTextLines[0]->GetAssetPosition().f.y;
    if(liveFrames==1 || liveFrames==15)
        std::printf("Original Credits live source frame%u: first-line Y%.9g (before live%.9g), phase%d.\n",
                    liveFrames,y,firstLineBeforeLive,retainedCredits->mPhase);
    if(liveFrames==1 || liveFrames==15)
        std::printf("Actual Credits movie frame%u/source retrace%d, active%d.\n",
                    GetMovieFrame(), glxGetFrameCount(), IsMovieActive());
}

// Retain the previous bounded fixture ABI for an independent fixed-step root
// comparison; the user-facing host sends elapsed native owner time instead.
extern "C" __attribute__((visibility("default"))) void charged_scene_render_frame()
{
    charged_scene_update_and_render_frame(1.0f/60.0f);
}

// Terminal diagnostic owner boundary. No original manager/CRT teardown is
// invented. MovieStop follows original audio/read/texture/pool release order;
// the host drains its genuine AI endpoint before releasing the one SDK.
extern "C" __attribute__((visibility("default"))) void charged_scene_stop_movie()
{
    MovieStop();
    Check(MovieQuit(), "Actual original MovieQuit failed");
}
