#pragma once
#include "runtime/audio_bank_load.h"
#include "runtime/audio_voices.h"
#include <optional>

namespace mscharged
{
struct FrontendAudioHandle
{
    std::uint64_t owner=0;
    std::uint32_t slot=0,generation=0;
    friend bool operator==(const FrontendAudioHandle&,const FrontendAudioHandle&)=default;
};
struct FrontendAudioStatus
{
    std::uint32_t cue=0;
    int state=0,instance_state=0,event_state=0,source_state=0;
    bool auto_release=false,limited=false;
    std::optional<std::uint32_t> sample;
};
// A retained native resident FE_GEN_Sfx bank in original name23/slot21.
// Loaded means checked bank ownership, never full AudioBackend readiness.
// Selection uses the caller's RNG transactionally. No context-pointer registry,
// streams/music, spatial owner, rumble or controller-speaker service is supplied.
// All operations and destruction require the creating thread before SDL_Quit.
class FrontendAudio
{
    struct Implementation;
    std::unique_ptr<Implementation> impl_;
public:
    FrontendAudio(LoadedAudioBank::Handle,resources::AudioCalculationInitial::Handle,
                  AudioVoicesOptions={});
    ~FrontendAudio();
    FrontendAudio(const FrontendAudio&) = delete;
    FrontendAudio& operator=(const FrontendAudio&) = delete;
    // Disabled/sentinel/missing cue returns no handle and consumes no RNG.
    // A source maximum-count rejection creates original state6, without selecting
    // or starting audio. It remains counted until Release or Unload.
    std::optional<FrontendAudioHandle> Play(std::uint32_t cue,unsigned& seed,bool auto_release=true);
    void ServiceAudio(); // Real source starts/consumption, in source insertion order.
    void Update(float dt); // Original event/instance/cue updates; destroys released9 handles.
    void Stop(FrontendAudioHandle,bool auto_release=true);
    void Release(FrontendAudioHandle); // Mark original9; next Update removes/decrements.
    void Cancel(FrontendAudioHandle); // Native immediate stop/remove; distinct from original Stop.
    FrontendAudioStatus Status(FrontendAudioHandle) const;
    bool IsFinished(FrontendAudioHandle) const; // Retired owned handle is finished.
    std::uint32_t ActiveCount(std::uint32_t cue) const;
    AudioCueSelectionState SelectionState(std::uint32_t cue) const;
    std::vector<FrontendAudioHandle> Handles() const;
    void Enable(bool);
    bool Enabled() const;
    void Unload(std::uint32_t slot=21); // Cancel all output before releasing bank/calculation.
    bool Loaded() const;
};
}
