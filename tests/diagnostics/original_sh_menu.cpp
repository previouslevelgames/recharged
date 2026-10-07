#include "runtime/original_sh_menu_diagnostic.h"
#include "Game/GameSceneManager.h"
#include "Game/FE/feDPD.h"
#include "Game/FE/feInput.h"
#include "Game/FE/feOptionsSubMenus.h"
#include "Game/FE/fePackage.h"
#include "Game/FE/fePopupMenu.h"
#include "Game/FE/fePresentation.h"
#include "Game/FE/feResourceManager.h"
#include "Game/FE/feSceneManager.h"
#include "Game/FE/tlSlide.h"
#include "Game/SH/SHOptions.h"
#include "Game/GameInfo.h"
#include "Game/Sys/audio.h"
#include "Game/Audio/AudioBundleManager.h"
#include "Game/Audio/AudioResourceRuntime.h"

// Sole definition remains the original Game/main.cpp source owner.
extern GameAudio* g_pGameAudio;
#include <cstddef>
#include <cstdint>

#if !defined(MSCHARGED_NATIVE) || !defined(MSCHARGED_GAME_MODULE) || \
    !defined(MSCHARGED_DIAGNOSTIC_FRONTEND_SEQUENCE) || \
    !defined(MSCHARGED_DIAGNOSTIC_FRONTEND_SH_MENUS)
#error This fixture belongs only to the named original SH menu diagnostic
#endif

namespace
{
bool requested;
constexpr std::int32_t OptionsID = 13;
constexpr std::int32_t AudioID = 14;
constexpr std::int32_t VisualID = 15;
constexpr std::int32_t NavigationID = 29;

std::uint64_t Address(const void* p)
{
    return static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(p));
}

BaseSceneHandler* Navigation(GameSceneManager* scenes)
{
    if (!scenes || scenes->mCurrentStackDepth > BaseGameSceneManager::MAX_SCENE_DEPTH)
        return nullptr;
    for (unsigned long i = 0; i < scenes->mCurrentStackDepth; ++i)
        if (static_cast<std::int32_t>(scenes->m_sceneStack[i]) == NavigationID)
            return scenes->mBaseSceneHandlerStack[i];
    return nullptr;
}

bool HasOriginalAudioRuntimeOwner()
{
    // main assigns its GameAudio pointer only after the actual constructor
    // returns. Do not inspect transient/uninitialized script or config fields.
    if (!g_pGameAudio || static_cast<AudioSystem*>(g_pGameAudio) != g_pAudioSystem)
        return false;
    auto* bundle = g_pAudioSystem->GetBundleManager();
    if (!bundle || bundle->GetResourceRuntime() != g_pAudioResourceRuntime)
        return false;
    return g_pAudioResourceRuntime->m_Script != nullptr;
}

void ObserveButton(OriginalSHSnapshot& out, unsigned slot, FEPointerButton& button)
{
    out.buttons[slot].min_x = button.GetMinX();
    out.buttons[slot].max_x = button.GetMaxX();
    out.buttons[slot].min_y = button.GetMinY();
    out.buttons[slot].max_y = button.GetMaxY();
    out.buttons[slot].disabled = button.mDisabled;
    for (unsigned i = 0; i < 4; ++i)
        out.buttons[slot].pointer_state[i] = button.GetPointerState(i);
    out.buttons_valid |= 1u << slot;
}
}

extern "C" __attribute__((visibility("default")))
std::uint32_t charged_original_sh_observe(OriginalSHSnapshot* result, std::uint32_t capacity)
{
    if (!result || capacity < sizeof(OriginalSHSnapshot))
        return 0;
    // Writes only caller-owned output. No source update/input accessor is called.
    OriginalSHSnapshot out{};
    out.version = 1;
    out.bytes = sizeof(out);
    out.options_state = -1;
    out.next_scene = SCENE_INVALID;
    out.popup_type = INVALID_TYPE;
    out.popup_highlight = -1;
    auto* scenes = GameSceneManager::Instance();
    out.owners = (scenes ? 1u : 0u) |
        (FESceneManager::Instance() ? 2u : 0u) |
        (FEResourceManager::Instance() ? 4u : 0u) |
        (g_pFEInput ? 8u : 0u) |
        (GameInfoManager::Instance() ? 16u : 0u) |
        (HasOriginalAudioRuntimeOwner() ? 32u : 0u);
    auto* navigation = Navigation(scenes);
    out.navigation_ready = navigation && navigation->IsSceneReady();
    for (unsigned i = 0; i < 4; ++i)
    {
        if (gFEPointerInstances[i])
            out.pointer_instances |= 1u << i;
        out.pointers[i][0] = gFEPointerPositions[i].x;
        out.pointers[i][1] = gFEPointerPositions[i].y;
    }
    out.controller_index = gFEControllerIndex;
    if (g_pFEInput)
    {
        out.input_lock_depth = g_pFEInput->m_InputLockDepth;
        out.input_allowed = g_pFEInput->m_bInputAllowed;
        for (unsigned i = 0; i < 4; ++i)
            if (g_pFEInput->m_bEnableInput[i])
                out.input_enabled |= 1u << i;
    }
    if (scenes && scenes->mCurrentStackDepth <= BaseGameSceneManager::MAX_SCENE_DEPTH)
    {
        out.stack_depth = static_cast<std::uint32_t>(scenes->mCurrentStackDepth);
        for (unsigned i = 0; i < out.stack_depth; ++i)
        {
            auto* handler = scenes->mBaseSceneHandlerStack[i];
            auto& item = out.stack[i];
            item.scene_id = static_cast<std::int32_t>(scenes->m_sceneStack[i]);
            item.resource_state = -1;
            item.handler = Address(handler);
            if (!handler)
                continue;
            item.hash = handler->mHashID;
            item.visible = handler->mVisible;
            auto* scene = handler->mFEScene;
            item.scene = Address(scene);
            if (!scene)
                continue;
            item.resource_state = scene->mState;
            // Package fields and handler-specific fields are only observed once
            // the original resource callbacks have made this actual scene ready.
            if (!handler->IsSceneReady())
                continue;
            auto* package = scene->m_pFEPackage;
            auto* presentation = handler->mPresentation;
            item.package = Address(package);
            item.presentation = Address(presentation);
            if (package)
                item.resources = static_cast<std::uint32_t>(package->m_uResourceCount);
            if (presentation && presentation->m_currentSlide)
            {
                auto* slide = presentation->m_currentSlide;
                item.slide_hash = slide->m_hash;
                item.slide_start = slide->GetStartTime();
                item.slide_duration = slide->GetDuration();
                item.slide_time = slide->GetCurrentTime();
            }
            if (i + 1 != out.stack_depth)
                continue;
            if (item.scene_id == OptionsID)
            {
                auto* options = static_cast<OptionsScene*>(handler);
                out.options_state = options->mScenePhase;
                out.options_initialized = options->mPointerButtonsInitialized;
                out.next_scene = static_cast<std::int32_t>(options->mNextScene);
                if (options->mPointerButtonsInitialized)
                    for (unsigned j = 0; j < 3; ++j)
                        ObserveButton(out, j, options->mOptionButtons[j]);
            }
            else if (item.scene_id == AudioID)
            {
                auto* audio = static_cast<OptionsAudioMenuV2*>(handler);
                out.options_state = audio->mState;
                out.submenu_initialized = audio->mPointerButtonsInitialized;
                out.submenu_saving = audio->mSaveStarted;
                for (unsigned j = 0; j < 3; ++j)
                    out.settings[j] = audio->mSettings[j];
                if (audio->mPointerButtonsInitialized)
                    for (unsigned j = 0; j < 6; ++j)
                        ObserveButton(out, j, audio->mButtonComponents[j]);
            }
            else if (item.scene_id == VisualID)
            {
                auto* visual = static_cast<OptionsVisualMenuV2*>(handler);
                out.options_state = visual->mState;
                out.submenu_initialized = visual->mPointerButtonsInitialized;
                out.submenu_saving = visual->mSaveStarted;
                for (unsigned j = 0; j < 2; ++j)
                    out.settings[j] = visual->mSettings[j];
                if (visual->mPointerButtonsInitialized)
                {
                    for (unsigned j = 0; j < 5; ++j)
                        ObserveButton(out, j, visual->mButtonComponents[j]);
                    for (unsigned j = 0; j < 2; ++j)
                        ObserveButton(out, j + 5, visual->mZoomButtonComponents[j]);
                }
            }
            else if (item.scene_id == SCENE_POPUP_MENU)
            {
                auto* popup = static_cast<FEPopupMenu*>(handler);
                out.popup_type = popup->GetType();
                out.popup_created = popup->mMenuCreated;
                out.popup_displayed = popup->mMenuDisplayed;
                out.popup_pressed = popup->mOptionPressed;
                out.popup_updates = popup->mUpdateCount;
                out.popup_highlight = popup->mHighlightedOption;
            }
        }
    }
    *result = out;
    return 1;
}

extern "C" __attribute__((visibility("default")))
std::uint32_t charged_original_sh_request_options()
{
    if (requested)
        return ORIGINAL_SH_ALREADY_REQUESTED;
    auto* scenes = GameSceneManager::Instance();
    auto* fe = FESceneManager::Instance();
    if (!scenes || !fe || !FEResourceManager::Instance() || !g_pFEInput ||
        !GameInfoManager::Instance())
        return ORIGINAL_SH_WAIT_OWNERS;
    if (!HasOriginalAudioRuntimeOwner())
        return ORIGINAL_SH_WAIT_AUDIO_RUNTIME_OWNER;
    auto* navigation = Navigation(scenes);
    if (!navigation || !navigation->IsSceneReady())
        return ORIGINAL_SH_WAIT_NAVIGATION;
    for (unsigned i = 0; i < 4; ++i)
        if (!gFEPointerInstances[i])
            return ORIGINAL_SH_WAIT_NAVIGATION;
    if (!fe->AreAllScenesValid())
        return ORIGINAL_SH_WAIT_RESOURCES;
    if (!scenes->mCurrentStackDepth ||
        scenes->mCurrentStackDepth >= BaseGameSceneManager::MAX_SCENE_DEPTH)
        return ORIGINAL_SH_WAIT_SOURCE_SCENE;
    const auto current = scenes->m_sceneStack[scenes->mCurrentStackDepth - 1];
    // Boot naturally creates Navigation and Intro before this selected gate.
    // Never select a scene from a fabricated/empty stack or replace a menu.
    if (current != SCENE_INTRO_MOVIE && current != SCENE_TITLE)
        return ORIGINAL_SH_WAIT_SOURCE_SCENE;
    scenes->Push(static_cast<SceneList>(OptionsID), SCREEN_NOTHING, true);
    requested = true;
    return ORIGINAL_SH_REQUESTED;
}
