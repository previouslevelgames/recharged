#pragma once
#include "resources/audio_calculation.h"
#include "resources/audio_dsp.h"
#include "runtime/audio_bank_selection.h"
#include <memory>
#include <span>
#include <thread>

namespace mscharged
{
struct ResidentAudioMix
{
    float volume_db;
    std::uint16_t input,left,right;
    unsigned pan;
};
class ResidentAudioBuffer
{
public:
    using Handle=std::shared_ptr<const ResidentAudioBuffer>;
    std::span<const float> Stereo()const{return stereo_;}
    std::uint32_t Rate()const{return pcm_->Rate();}
    std::uint32_t Sample()const{return pcm_->SourceSample();}
    ResidentAudioMix Mix()const{return mix_;}
private:
    ResidentAudioBuffer()=default;
    resources::AudioResidentBank::Handle bank_;
    resources::AudioCalculationInitial::Handle calculation_;
    resources::AudioPcmSample::Handle pcm_;
    ResidentAudioMix mix_{};
    std::vector<float> stereo_;
    friend Handle PrepareResidentAudio(const AudioBankSelectionResult&,
        resources::AudioCalculationInitial::Handle);
};
// One authored resident sound event with no delay, pitch or random playback
// modifiers. Original bank selection already consumed its source-choice RNG.
// SDL resampling/mixing is native host output, not bit-exact AX emulation.
ResidentAudioBuffer::Handle PrepareResidentAudio(const AudioBankSelectionResult&,
    resources::AudioCalculationInitial::Handle);

enum class ResidentAudioState { Prepared,Playing,InputConsumed,Stopped };
struct ResidentAudioStatus
{
    ResidentAudioState state;
    int queued_input_bytes;
    int available_output_bytes;
};
// Thread-affine logical SDL device/stream owner. Initialization queues retained
// PCM into a paused device; Start admits real output. InputConsumed means SDL
// consumed its stream data, not that the physical device finished playing it.
// Stop destroys the logical device and discards pending data. Hardware already
// submitted before Stop cannot be recalled. Destroy before the application's SDL_Quit.
class ResidentAudioOutput
{
public:
    explicit ResidentAudioOutput(ResidentAudioBuffer::Handle,std::uint32_t device_id=0);
    ~ResidentAudioOutput();
    ResidentAudioOutput(const ResidentAudioOutput&)=delete;
    ResidentAudioOutput& operator=(const ResidentAudioOutput&)=delete;
    void Start();
    ResidentAudioStatus Status();
    void Stop();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    std::thread::id thread_=std::this_thread::get_id();
    void Thread()const;
};
}
