#pragma once
#include "resources/audio_stream.h"
#include "resources/audio_calculation.h"
#include <thread>

namespace mscharged
{
enum class FrontendMusicLoadState { Idle,Loading,Ready,Failed,Cancelled,Unloaded };
struct FrontendMusicStatus
{
    FrontendMusicLoadState load=FrontendMusicLoadState::Idle;
    std::uint32_t cue=0,source=0;
    int source_state=0,event_state=0;
    std::uint32_t maximum_cycles=0;
    std::uint64_t requested_reads=0,completed_reads=0,decoded_frames=0,submitted_frames=0,completed_cycles=0;
    int queued_input_bytes=0,available_output_bytes=0;
    bool paused=false;
};
struct FrontendMusicOptions { std::uint32_t device_id=0; };
// Explicit FE_GEN_Music name26/slot22 native stream owner. Caller pumps real NL
// services and Service/Poll on the creating thread before NL/arena/SDL teardown.
// A failed replacement retains the current stream; failed initial load is not
// music readiness. Only original Title(0)/Main(1) one-event stereo profiles are
// selected. PCM/refill storage is bounded by two authored channel blocks.
// Queued/consumed SDL input does not report physical device audibility.
class FrontendMusic
{
public:
    FrontendMusic(resources::AudioBankCatalog::Handle,resources::AudioCalculationInitial::Handle,
                  FrontendMusicOptions={});
    ~FrontendMusic();
    FrontendMusic(const FrontendMusic&)=delete;
    FrontendMusic& operator=(const FrontendMusic&)=delete;
    // Selection takes no random draws in this qualified single-voice/single-source
    // profile. The passed seed is verified/preserved; no private RNG is created.
    void BeginSelect(unsigned index,unsigned& seed);
    void Poll();
    void Service();
    void CancelPending();
    bool Pause();
    bool Resume();
    void Stop();
    void Unload(std::uint32_t slot=22);
    FrontendMusicStatus Status()const;
    // Throws the retained failure from the most recent replacement, if any.
    void Check()const;
private:
    struct Implementation;
    std::unique_ptr<Implementation> impl_;
};
}
