#pragma once
#include "resources/audio_bank.h"

namespace mscharged
{
enum class AudioBankLoadState { Loading, Ready, Failed, Cancelled };
struct AudioBankLoadProgress
{
    unsigned requested_reads = 0, completed_reads = 0;
};
struct LoadedAudioBank
{
    using Handle = std::shared_ptr<const LoadedAudioBank>;
    std::uint32_t name_index, slot_index;
    resources::AudioBankNameRecord name;
    resources::AudioBankSlotRecord slot;
    resources::AudioResidentBank::Handle bank;
};
// Native counterpart of the resident resource read chain: resbun, then nlxwb.
// Indices follow AudioBankTable::LoadBank's array arguments, not record IDs.
// Ready means a checked, retained asset, not an initialized AX bank or a live
// AudioResourceLoadOwner. No original readiness callback is fabricated.
// Service/Cancel/destruction require the creating NL thread, before shutdown.
// Retained results survive destruction and game arena teardown.
class AudioBankLoad
{
    struct Implementation;
    std::unique_ptr<Implementation> impl_;
public:
    AudioBankLoad(resources::AudioBankCatalog::Handle, std::uint32_t name_index,
                  std::uint32_t slot_index);
    ~AudioBankLoad();
    AudioBankLoad(const AudioBankLoad&) = delete;
    AudioBankLoad& operator=(const AudioBankLoad&) = delete;
    void Poll();
    void Service();
    void Cancel();
    AudioBankLoadState State() const;
    AudioBankLoadProgress Progress() const;
    LoadedAudioBank::Handle Result() const;
};
}
