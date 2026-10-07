#include "NL/nlDLListContainer.inl"
#include "Game/Audio/AudioSystem.inl"
#include "Game/Audio/AudioBundleManager.h"
#include "Game/Audio/AudioBankTable.h"
#include "Game/Sys/audio.h"
#include "Game/Audio/AudioGlobals.h"
#include "platform/game_allocation_ownership.h"
#include "platform/dsp_control_abi.h"
#include "platform/dsp_mailbox_abi.h"
#include <stdexcept>

// Read actual source predicates only. Backend-initialized and completed bundle
// loading are separate; source Shutdown deliberately does not clear either.
extern "C" __attribute__((visibility("default"))) unsigned charged_original_audio_observe() {
    if (!g_pAudioSystem) return 0;
    return 1u | (g_pAudioSystem->IsInitialized() ? 2u : 0u) |
        (g_pAudioSystem->GetBundleManager() && g_pAudioSystem->GetBundleManager()->IsLoaded() ? 4u : 0u);
}

// Diagnostic observation only: source handles/events decide when they are idle.
extern "C" __attribute__((visibility("default"))) bool charged_original_audio_idle() {
    if (charged_original_audio_observe() != 7u)
        throw std::logic_error("Original audio idle observation requires its loaded source owner");
    return g_pAudioSystem->IsIdle();
}

// Separate read-only terminal observation: source IsIdle covers handles/pools,
// not the real NL resource/sample callbacks. Do not change its meaning or cancel
// pending bank loads merely to retire the host diagnostic.
extern "C" __attribute__((visibility("default"))) bool charged_original_audio_bank_reads_idle() {
    if (charged_original_audio_observe() != 7u)
        throw std::logic_error("Original bank observation requires its loaded source owner");
    AudioBankTable* table = g_pAudioSystem->GetBundleManager()->GetSoundMap();
    if (!table)
        throw std::logic_error("Original initialized source lacks its sound bank table");
    for (u32 i = 0; i < table->count_08; ++i) {
        const auto& source = table->records_0C[i];
        const auto* owner = source.field_10;
        if (source.field_14 && !owner)
            return false;
        if (owner && owner->m_Loader && !owner->m_Completed)
            return false;
    }
    return true;
}

// Called only after genuine AI/job drain and device HALT/close, while source
// owners and their allocation endpoints remain live. No forced Stop/update loop.
extern "C" __attribute__((visibility("default"))) void charged_original_audio_unload_idle_banks() {
    if (!charged_original_audio_bank_reads_idle())
        throw std::logic_error("Pending original bank callbacks must complete before host retirement");
    if (!charged_original_audio_idle())
        throw std::logic_error("Active original sounds require the original teardown flow before bank retirement");
    const auto csr = ChargedDSPControlRead();
    if (!(csr & 4u) || (csr & 0x80u) ||
            (ChargedDSPMailToHigh() & 0x8000u) ||
            (ChargedDSPMailFromHigh() & 0x8000u))
        throw std::logic_error("Original bank retirement requires actual device HALT and drained mail/IRQ");
    UnloadSoundBanks(static_cast<GameAudio*>(g_pAudioSystem));
    AudioBankTable* soundMap = g_pAudioSystem->GetBundleManager()->GetSoundMap();
    if (soundMap != 0)
        soundMap->ClearSelectedGroups();
}

extern "C" __attribute__((visibility("default"))) void charged_original_audio_shutdown() {
    if (charged_original_audio_observe() != 7u)
        throw std::logic_error("Original audio shutdown requires its genuinely loaded source owner");
    g_pAudioSystem->Shutdown();
}

// Only the native device lease is retired. The original allocation, source
// pointer and initialized flags remain untouched. Called after AI/HALT/Close.
extern "C" __attribute__((visibility("default"))) void charged_original_audio_retire_silence_pin() {
    mscharged::platform::RetireGameAllocationDevicePins(g_pAudioSilenceBuffer);
}
