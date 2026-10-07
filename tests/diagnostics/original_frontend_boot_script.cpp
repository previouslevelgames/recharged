#include "Game/AsyncLoading.h"
#include "Game/GameSceneManager.h"
#include "Game/SH/SHBootLoading.h"
#include "platform/game_allocation_ownership.h"

// Read actual callback/native-data/source VM predicates only. No instruction,
// source state, flag, callback, scene, bank or cue is requested by this observer.
extern "C" __attribute__((visibility("default"))) unsigned charged_original_boot_script_observe() {
    auto* loading = AsyncLoadingManager::Instance();
    if (!loading || !loading->mByteCode) return 0;
    unsigned flags = 1u;
    mscharged::platform::GameCompletedSpan span{};
    if (mscharged::platform::FindGameCompletedSpan(
            loading->mByteCode, sizeof(ByteCodeHeader), span)
        && span.base == loading->mByteCode && span.bytes >= sizeof(ByteCodeHeader))
        flags |= 8u;
    if (loading->m_Header != loading->mByteCode || !(flags & 8u)) return flags;
    auto* header = loading->m_Header;
    if (header->signature != 0xe11c2112u || !header->numFunctions
        || !header->m_FunctionTable.Get() || !header->m_CodeSegment.Get())
        return flags;
    flags |= 2u;
    if (loading->IsFinished()) flags |= 4u;
    if (loading->mSequenceState == ASYNC_LOADING_IDLE) flags |= 16u;
    return flags;
}

extern "C" __attribute__((visibility("default"))) int charged_original_boot_script_instruction() {
    if ((charged_original_boot_script_observe() & 2u) == 0u) return -1;
    auto* loading = AsyncLoadingManager::Instance();
    if (!loading->m_IP) return -1;
    // Original source performs the same-code-segment native pointer difference.
    return loading->GetInstructionOffset();
}

extern "C" __attribute__((visibility("default"))) int charged_original_boot_scene_phase() {
    auto* scenes = GameSceneManager::Instance();
    if (!scenes) return -1;
    auto* scene = static_cast<BootLoadingScene*>(scenes->GetScene(SCENE_BOOT_LOADING));
    return scene ? scene->mPhase : -1;
}
