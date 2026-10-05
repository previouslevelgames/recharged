#include "runtime/audio_output.h"
#include "Game/Audio/AudioResidentSteps.h"
#include "revolution/mix/MIXTableSteps.h"
#include <SDL3/SDL.h>
#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <string>

namespace mscharged
{
namespace
{
constexpr u16 volume_table[]={
#include "revolution/mix/MIXVolumeTable.inc"
};
constexpr std::uint32_t pan_table[]={
#include "revolution/mix/MIXPanTable.inc"
};
static_assert(std::size(volume_table)==965&&std::size(pan_table)==128);
void Supported(bool good,const char* message){if(!good)throw resources::UnsupportedResource(message);}
std::runtime_error Error(const char* operation){return std::runtime_error(std::string(operation)+": "+SDL_GetError());}
}
ResidentAudioBuffer::Handle PrepareResidentAudio(const AudioBankSelectionResult& selection,
    resources::AudioCalculationInitial::Handle calculation,std::optional<float> slider_override)
{
    using resources::Require;
    Require(bool(selection.bank)&&bool(calculation),"Resident output requires retained bank and calculation table");
    const auto& bank=*selection.bank;
    const auto& cue=bank.Cues().at(selection.cue);
    Require(std::any_of(cue.voices.begin(),cue.voices.end(),[&](const auto& v){return v.voice==selection.voice;}),
        "Selected voice does not belong to its cue");
    const auto& voice=bank.Voices().at(selection.voice);
    Supported(voice.sequences.size()==1&&selection.events.size()==1,"Resident output supports one authored sequence/event");
    const auto& event=selection.events.front();const auto& sequence=bank.Sequences().at(event.sequence);
    Require(voice.sequences.front()==event.sequence&&sequence.events.size()==1&&sequence.events.front()==event.event,
        "Selected event is inconsistent with its voice/sequence");
    const auto& sound=bank.Sounds().at(event.event);
    Require(std::any_of(sound.choices.begin(),sound.choices.end(),[&](const auto& c){return c.index==event.source;}),
        "Selected source does not belong to its sound event");
    Require(bank.Sources().at(event.source).fields[0]==event.sample,"Selected source/sample is inconsistent");
    Supported(sound.sound_id==1,"Looped/repeated resident playback is unavailable");
    Supported(!sound.random_pitch&&!sound.random_volume&&voice.pitch==0,
        "Resident pitch and random playback modifiers are unavailable");
    Supported(sound.delay_min==0&&sound.delay_range==0&&event.start_time==0,
        "Delayed resident playback is unavailable");
    const float slider=slider_override.value_or(calculation->Value(voice.slider));
    Require(std::isfinite(slider)&&slider>=-96&&slider<=6,"Resident slider exceeds its original finite range");
    // Original zero-duration Prepare targets clamp the voice transition before Play.
    const float target=std::clamp(voice.volume,-96.0f,6.0f);
    const float instance=AudioResidentInstanceVolume(0.0f,slider,target);
    const float db=AudioResidentPlaybackVolume(0.0f,instance,0.0f,sequence.volume);
    Require(std::isfinite(db),"Resident playback volume is nonfinite");
    unsigned pan=0,span=0;int fader=0;
    AudioResidentInitializeMix([&](int,int mode,int input,int aux_a,int aux_b,int aux_c,int p,int s,int f)
    {
        Require(mode==0&&input==0&&aux_a==-960&&aux_b==-960&&aux_c==-960,"Unexpected resident mix defaults");
        pan=p;span=s;fader=f;
    },0);
    const auto pan_db=[](unsigned index){return std::bit_cast<std::int32_t>(pan_table[index]);};
    const ResidentAudioMix mix{db,MIXTableVolume(AudioResidentInputTenths(db),volume_table),
        MIXTableVolume(fader+pan_db(pan)+pan_db(127-span),volume_table),
        MIXTableVolume(fader+pan_db(127-pan)+pan_db(127-span),volume_table),pan};
    auto result=std::shared_ptr<ResidentAudioBuffer>(new ResidentAudioBuffer);
    result->bank_=selection.bank;result->calculation_=std::move(calculation);
    result->pcm_=resources::DecodeAudioDsp(result->bank_,event.sample,4*1024*1024);result->mix_=mix;
    const auto pcm=result->pcm_->Samples();
    resources::Require(pcm.size()<=4*1024*1024,"Resident stereo output exceeds its32MiB budget");
    result->stereo_.reserve(pcm.size()*2);
    const float input=float(mix.input)/32768.0f,left=float(mix.left)/32768.0f,right=float(mix.right)/32768.0f;
    for(const auto sample:pcm)
    {
        const float value=float(sample)/32768.0f*input;
        // The original MIX fixed-point gains are retained. Host float mixing and
        // resampling intentionally do not claim AX accumulator/filter parity.
        result->stereo_.push_back(value*left);result->stereo_.push_back(value*right);
    }
    return result;
}
std::vector<float> ResidentAudioBuffer::BeforeInputGain()const
{
    std::vector<float> stereo;stereo.reserve(pcm_->Samples().size()*2);
    const float left=float(mix_.left)/32768.0f,right=float(mix_.right)/32768.0f;
    for(auto sample:pcm_->Samples())
    {const float value=float(sample)/32768.0f;stereo.push_back(value*left);stereo.push_back(value*right);}
    return stereo;
}
struct ResidentAudioOutput::Impl
{
    ResidentAudioBuffer::Handle buffer;
    SDL_AudioStream* stream=nullptr;
    bool initialized=false;
    bool live_gain=false;
    ResidentAudioState state=ResidentAudioState::Prepared;
    ~Impl(){Close();}
    void Close()noexcept
    {
        if(stream){SDL_PauseAudioStreamDevice(stream);SDL_ClearAudioStream(stream);SDL_DestroyAudioStream(stream);stream=nullptr;}
        buffer.reset();state=ResidentAudioState::Stopped;
        if(initialized){SDL_QuitSubSystem(SDL_INIT_AUDIO);initialized=false;}
    }
};
ResidentAudioOutput::ResidentAudioOutput(ResidentAudioBuffer::Handle buffer,std::uint32_t device,bool live_gain):impl_(std::make_unique<Impl>())
{
    resources::Require(bool(buffer)&&!buffer->Stereo().empty(),"Audio output requires prepared resident samples");
    resources::Require(buffer->Rate()<=std::uint32_t(std::numeric_limits<int>::max())&&
        buffer->Stereo().size_bytes()<=std::size_t(std::numeric_limits<int>::max()),"Audio output exceeds SDL input limits");
    impl_->buffer=std::move(buffer);
    impl_->live_gain=live_gain;
    if(!SDL_InitSubSystem(SDL_INIT_AUDIO))throw Error("SDL audio initialization failed");
    impl_->initialized=true;
    const SDL_AudioSpec spec{SDL_AUDIO_F32,2,static_cast<int>(impl_->buffer->Rate())};
    impl_->stream=SDL_OpenAudioDeviceStream(device?device:SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK,&spec,nullptr,nullptr);
    if(!impl_->stream)throw Error("SDL audio device/stream creation failed");
    const auto before=live_gain?impl_->buffer->BeforeInputGain():std::vector<float>{};
    const auto data=live_gain?std::span<const float>(before):impl_->buffer->Stereo();
    if(live_gain)SetVolume(impl_->buffer->Mix().volume_db);
    if(!SDL_PutAudioStreamData(impl_->stream,data.data(),static_cast<int>(data.size_bytes())))throw Error("SDL audio queue failed");
    if(!SDL_FlushAudioStream(impl_->stream))throw Error("SDL audio flush failed");
}
ResidentAudioOutput::~ResidentAudioOutput()=default;
void ResidentAudioOutput::Thread()const
{if(thread_!=std::this_thread::get_id())throw std::logic_error("Resident audio output used from another thread");}
void ResidentAudioOutput::Start()
{
    Thread();if(impl_->state!=ResidentAudioState::Prepared)throw std::logic_error("Resident audio output is not prepared");
    if(!SDL_ResumeAudioStreamDevice(impl_->stream)){const auto error=Error("SDL audio start failed");impl_->Close();throw error;}
    impl_->state=ResidentAudioState::Playing;
}
ResidentAudioStatus ResidentAudioOutput::Status()
{
    Thread();if(!impl_->stream)return{ResidentAudioState::Stopped,0,0};
    // A logical device may consume between these reads. Both counters are
    // monotonic after this finite flushed queue; retry naturally on next poll.
    const int input=SDL_GetAudioStreamQueued(impl_->stream),output=SDL_GetAudioStreamAvailable(impl_->stream);
    if(input<0||output<0){const auto error=Error("SDL audio stream query failed");impl_->Close();throw error;}
    if(impl_->state==ResidentAudioState::Playing&&!input&&!output)impl_->state=ResidentAudioState::InputConsumed;
    const float gain=SDL_GetAudioStreamGain(impl_->stream);
    if(!std::isfinite(gain)||gain<0)throw Error("SDL stream gain query failed");
    return{impl_->state,input,output,gain};
}
void ResidentAudioOutput::SetVolume(float db)
{
    Thread();resources::Require(impl_->live_gain&&impl_->stream,"Resident live gain requires its retained active output");
    resources::Require(std::isfinite(db)&&db>=-96&&db<=6,"Resident live volume exceeds its original range");
    const float gain=float(MIXTableVolume(AudioResidentInputTenths(db),volume_table))/32768.0f;
    if(!SDL_SetAudioStreamGain(impl_->stream,gain))throw Error("Resident live stream gain failed");
}
void ResidentAudioOutput::Stop()
{
    Thread();if(!impl_->stream)return;
    const bool paused=SDL_PauseAudioStreamDevice(impl_->stream);
    const bool cleared=SDL_ClearAudioStream(impl_->stream);
    const auto error=(!paused||!cleared)?std::optional<std::runtime_error>(Error("SDL audio stop failed")):std::nullopt;
    impl_->Close();if(error)throw *error;
}
}
