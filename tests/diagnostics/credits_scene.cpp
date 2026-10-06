// Bounded diagnostic entry into genuine source scene services. Every loading,
// completion, authored component, text/visibility/update and draw decision
// below is made by the original reconstructed TUs. No main/task-loop claim.
#include "Game/BaseGameSceneManager.h"
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
    // Actual source gxInit owns the hardware/cache initialization normally
    // requested by whole glPlat::glx_InitGX. No hand-tuned renderer state.
    GXAdjustForOverscan(&GXNtsc480IntDf, &glx_rmode, 0, 16);
    gxInit();
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

    // Named platform-frame diagnostic omits source glPlat/VI startup. These
    // exact EFB logical dimensions are the original glPlat default target
    // values, recorded in the actual source screen object, not fake getters.
    glGetScreenInfo()->ScreenWidth = 640;
    glGetScreenInfo()->ScreenHeight = 448;
    glxInitTargets();
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
    for (unsigned frame=0; frame<120; ++frame)
        credits->Update(1.0f/60.0f);
    Check(credits->m_pTextLines[0]->GetString() == credits->mStrings[0] && credits->mStrings[0][0],
          "Original parser/handler did not publish genuine first Credits string");
    scenes->RenderActiveScenes();
    Check(!view.m_Enabled, "Original Anark alpha-write permission changed during packet attachment");
    retainedSceneView = &view;
    view.Iterate(Observe);
    Check(packets && vertices, "Actual original Credits renderer omitted source packets");
    Check(gxGetNumTexGens() == 0, "GPU fixture issued an unintended source GX request before first frame");
    std::printf("231 owned original Credits: %u checks, %u source packets, %u vertices. Temporary phase2/movie/audio/world/input gates; genuine Anark/source projection; metadata-only packet observation before first GPU frame.\n", checks,packets,vertices);
    // Keep live source scene/font/pool owners until terminal diagnostic exit.
    // No missing FESceneManager destructor or CRT cleanup is invented.
    (void)fontPool;
    return checks;
}

extern "C" __attribute__((visibility("default"))) void charged_scene_render_frame()
{
    if (!retainedSceneView) throw std::logic_error("Original owned Credits scene not loaded");
    // Repeat the unchanged original source packet list while the real host
    // asynchronously prepares pipelines. This is a static scene diagnostic,
    // not an original task/frame loop or artificial resource readiness.
    retainedSceneView->Iterate(glx_SendFrame_cb);
    glx_SendEnd();
}
