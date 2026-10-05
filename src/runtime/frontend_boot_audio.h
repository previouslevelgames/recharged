#pragma once
#include "runtime/audio_bank_load.h"
#include "runtime/audio_output.h"
#include <optional>

namespace mscharged
{
enum class FrontendBootAudioState { Loaded,Playing,InputConsumed,Unloaded,Failed };
struct FrontendBootAudioStatus
{
    FrontendBootAudioState state;
    std::optional<std::uint32_t> sample;
    int queued_input_bytes=0,available_output_bytes=0;
};
// Bounded logo-cue admission, not an original AudioResourceLoadOwner/AX slot.
// The checked loaded asset must identify name25/slot23 FE_GEN_Splash. The caller
// owns the shared RNG and must retain it until this owner is destroyed.
// PlayLogo is transactional through all preparation/device failures: selection
// and RNG publish only after a real SDL stream starts. No further allocation
// or fallible service occurs during that publication.
// All methods require the creating thread. Destroy before SDL shutdown.
class FrontendBootAudio
{
    struct Implementation;
    std::unique_ptr<Implementation> impl_;
public:
    FrontendBootAudio(LoadedAudioBank::Handle,resources::AudioCalculationInitial::Handle,
        unsigned& caller_seed,std::uint32_t device_id=0);
    ~FrontendBootAudio();
    FrontendBootAudio(const FrontendBootAudio&)=delete;
    FrontendBootAudio& operator=(const FrontendBootAudio&)=delete;
    void PlayLogo();
    // Discard pending output before releasing the bank. Already submitted
    // physical audio cannot be recalled. Same-slot repeated unload is harmless.
    void Unload(std::uint32_t slot=23);
    FrontendBootAudioStatus Status();
    AudioCueSelectionState SelectionState()const;
};
}
