#include "runtime/frontend_music.h"
#include "Game/Audio/AudioBankSelectionSteps.h"
#include "Game/Audio/AudioResidentSteps.h"
#include "Game/Audio/AudioCuePlaybackSteps.h"
#include "Game/Audio/AudioVoiceSteps.h"
#include "Game/Audio/AudioStreamControlSteps.h"
#include "revolution/mix/MIXTableSteps.h"
#include "NL/MemAlloc.h"
#include "NL/nlFileGC.h"
#include "NL/nlMath.h"
#include <SDL3/SDL.h>
#include <algorithm>
#include <bit>
#include <exception>
#include <limits>
#include <optional>

namespace mscharged
{
namespace
{
using resources::Require;
void Supported(bool good,const char* why){if(!good)throw resources::UnsupportedResource(why);}
std::runtime_error AudioError(const char* why){return std::runtime_error(std::string(why)+": "+SDL_GetError());}
constexpr std::uint16_t volume_table[]={
#include "revolution/mix/MIXVolumeTable.inc"
};
constexpr std::uint32_t pan_table[]={
#include "revolution/mix/MIXPanTable.inc"
};
struct FreeBuffer {void operator()(std::uint8_t* p)const noexcept{if(p)VirtualAllocator.Free(p);}};
struct Read
{
    nlFile* file=nullptr;
    std::unique_ptr<std::uint8_t,FreeBuffer> data;
    unsigned size=0;
    bool issued=false,complete=false;
    std::exception_ptr error;
    std::uint64_t requested=0,completed=0;
    static void Done(nlFile* file,void* bytes,unsigned count,nlFileAsyncParam context)
    {
        auto& r=*reinterpret_cast<Read*>(context);
        try {Require(r.issued&&!r.complete&&file==r.file&&bytes==r.data.get()&&count==r.size,"Music read callback identity differs");r.complete=true;++r.completed;}
        catch(...){r.error=std::current_exception();}
    }
    void Issue(nlFile* f,std::uint32_t offset,unsigned count)
    {
        Require(!issued||complete,"Music read destination is still in use");
        auto next=std::unique_ptr<std::uint8_t,FreeBuffer>(static_cast<std::uint8_t*>(VirtualAllocator.Allocate(count,32,false)));
        nlSeek(f,offset,0);data=std::move(next);file=f;size=count;issued=true;complete=false;error=nullptr;++requested;
        auto* token=nlReadAsync(f,data.get(),count,Done,reinterpret_cast<nlFileAsyncParam>(this),count);
        if(!token&&!complete)throw std::runtime_error("Music read was not queued");
    }
    bool Ready()
    {
        if(error)std::rethrow_exception(error);
        if(issued&&!complete&&!nlAsyncReadsPending(file))throw std::runtime_error("Music read failed or was cancelled");
        return complete;
    }
    resources::Bytes Bytes()const{return{data.get(),size};}
    void Drain()
    {
        if(file)nlCancelPendingAsyncReads(file,nullptr);
        issued=false;complete=false;file=nullptr;data.reset();
    }
};
struct Selection
{
    std::uint32_t cue,index,source,play_count;
    float input_gain,left_gain,right_gain;
    float prepared_db;std::uint32_t voice,sequence;
};
Selection Select(resources::AudioStreamBank::Handle bank,resources::AudioCalculationInitial::Handle calc,
                 std::uint32_t hash,unsigned seed,AudioCategoryVolumes::Handle volumes)
{
    const auto ci=bank->CuesByKey().Find({hash,0,0,0});Require(ci!=0xffff,"Frontend music cue is absent");
    const auto& cue=bank->Cues().at(ci);
    Supported(cue.voices.size()==1&&cue.maximum_count>0,"Music requires one admitted authored voice");
    const auto vi=cue.voices[0].voice;const auto& voice=bank->Voices().at(vi);
    Supported(voice.sequences.size()==1&&voice.pitch==0,"Music sequence/pitch profile is unsupported");
    const auto& sequence=bank->Sequences().at(voice.sequences[0]);
    Supported(sequence.events.size()==1,"Music supports one authored sound event");
    const auto& event=bank->Sounds().at(sequence.events[0]);
    Supported(event.choices.size()==1&&!event.random_pitch&&!event.random_volume&&event.delay_min==0&&event.delay_range==0,
        "Music delay/random/multiple-source profile is unsupported");
    Supported(event.sound_id==1||event.sound_id==0xffffffff,"Music repeat count is unsupported");
    struct Voice {unsigned index;};struct Entry{Voice* voice;float minimumValue,weight;unsigned selectionCount=0;bool eligible=false;};
    struct Cue{unsigned voiceCount;Entry* voices;int selectionMode;unsigned selectedVoiceIndex;};
    Voice v{vi};Entry entry{&v,cue.voices[0].minimum,cue.voices[0].weight};Cue c{1,&entry,cue.selection_mode,0xffff};
    const unsigned original_seed=seed;
    Require(AudioSelectCueVoiceSteps<Voice,Entry>(&c,[&](unsigned range){return nlRandom(range,&seed);})==&v,"Music voice selection differs");
    struct Definition{unsigned choiceCount;const resources::AudioBankChoice* choices;float delayMinimum,delayRange;};
    Definition d{1,event.choices.data(),event.delay_min,event.delay_range};
    Require(AudioEventStartTimeSteps(&d,[&](float a,float b){return nlRandomf(a,b,&seed);})==0,"Music start time differs");
    const auto selected=AudioSelectSourceSteps(&d,[&](unsigned range){return nlRandom(range,&seed);});
    Require(seed==original_seed,"Single-source music unexpectedly consumed random state");
    const float db=AudioResidentPlaybackVolume(0,AudioResidentInstanceVolume(0,volumes?volumes->Value(voice.slider):calc->Value(voice.slider),std::clamp(voice.volume,-96.0f,6.0f)),0,sequence.volume);
    Require(std::isfinite(db),"Music gain is nonfinite");
    const float gain=float(MIXTableVolume(AudioResidentInputTenths(db),volume_table))/32768.0f;
    // Owned original stereo SetPan(0) gives positions0/127. MIX retains its
    // endpoint table attenuation; this is not mono duplicated at unity gain.
    const auto pan=[&](unsigned p){return float(MIXTableVolume(std::bit_cast<std::int32_t>(pan_table[p]),volume_table))/32768.0f;};
    return{hash,ci,selected,event.sound_id,gain,pan(0),pan(127),db,vi,voice.sequences[0]};
}
}
struct FrontendMusic::Implementation
{
    struct Stream
    {
        resources::AudioStreamBank::Handle bank;
        resources::AudioStreamFormat::Handle format;
        resources::AudioCalculationInitial::Handle calculation;
        AudioCategoryVolumes::Handle volumes;
        Selection selection{};
        resources::AudioStreamDecoder decoder;
        std::unique_ptr<nlFile> file;
        Read read;
        SDL_AudioStream* output=nullptr;
        bool audio=false,header=false,prepared=false,paused=false,ended=false;
        int m_Unknown10=1,m_Unknown28=3,m_Unknown04=0;
        struct Event
        {
            Stream* source;
            struct Clock {float previousTime=-1,currentTime=0;} clock;
            struct Owner {Clock* soundInstance;} sequence{&clock};
            Owner* owner=&sequence;
            int state=0;unsigned flags=0;float startTime=0;
            void StartPlayback(){UpdatePlaybackParameters(true);source->Play(source->selection.play_count);state=4;}
            void UpdatePlaybackParameters(bool)
            {
                const auto& voice=source->bank->Voices().at(source->selection.voice);
                const auto& seq=source->bank->Sequences().at(source->selection.sequence);
                const float db=AudioResidentPlaybackVolume(0,AudioResidentInstanceVolume(0,source->volumes?source->volumes->Value(voice.slider):source->calculation->Value(voice.slider),std::clamp(voice.volume,-96.0f,6.0f)),0,seq.volume);
                Require((source->volumes||db==source->selection.prepared_db)&&voice.pitch==0,"Stream parameters left qualified mix profile");
                if(source->volumes&&source->output)
                {
                    const float gain=float(MIXTableVolume(AudioResidentInputTenths(db),volume_table))/32768.0f;
                    if(!SDL_SetAudioStreamGain(source->output,gain))throw AudioError("Music category gain failed");
                    source->selection.input_gain=gain;source->selection.prepared_db=db;
                }
            }
            int Update(float){return AudioPlaybackUpdateStep(*this);}
        } event{this};
        unsigned m_PlayCount:12=0;
        std::uint64_t decoded=0,submitted=0,cycles=0;
        int queued=0,available=0;
        ~Stream(){Close();}
        void Close()noexcept
        {
            try{read.Drain();}catch(...){std::terminate();}file.reset();
            if(output){SDL_PauseAudioStreamDevice(output);SDL_ClearAudioStream(output);SDL_DestroyAudioStream(output);output=nullptr;}
            if(audio){SDL_QuitSubSystem(SDL_INIT_AUDIO);audio=false;}
        }
        void Begin(resources::AudioStreamBank::Handle b,resources::AudioCalculationInitial::Handle c,
                   std::unique_ptr<nlFile> f,Selection s,AudioCategoryVolumes::Handle v)
        {
            bank=std::move(b);calculation=std::move(c);file=std::move(f);selection=s;volumes=std::move(v);
            // Original sound event preparation waits for both real stream
            // channel blocks; actual Play happens only after preparation.
            AudioPlaybackPrepareStep(event);
        }
        void Prepare(){read.Issue(file.get(),bank->Tracks().at(selection.source).offset,204);m_Unknown10=2;}
        bool Play(unsigned value){return AudioStreamPlaySteps(*this,value,[&]{Prepare();},[&]{Start();});}
        void PlayEvent(){AudioPlaybackPlayStep(event);event.clock.previousTime=0;}
        int GetState()const{return m_Unknown04;}
        bool HasVoice()const{return output!=nullptr;}
        bool WasVoiceDropped()const{return false;} // This SDL owner has no AX voice stealing.
        void ReleaseVoice(bool)
        {
            if(output){SDL_PauseAudioStreamDevice(output);SDL_ClearAudioStream(output);SDL_DestroyAudioStream(output);output=nullptr;}
            queued=available=0;
        }
        void UpdateState(){AudioSourceStateStep(*this);}
        void Start()
        {
            Require(output&&prepared,"Music has no prepared output");
            if(!SDL_ResumeAudioStreamDevice(output))throw AudioError("Music start failed");
            m_Unknown10=4;paused=false;
        }
        void Queue()
        {
            auto next=decoder;
            const auto bytes=read.Bytes();const auto n=format->BlockBytes();
            auto pcm=resources::DecodeAudioStreamBlock(format,next,bytes.first(n),bytes.subspan(n,n));
            std::vector<float> mixed; mixed.reserve(pcm.stereo.size());
            for(std::size_t i=0;i<pcm.stereo.size();i+=2)
            {
                const float l=float(pcm.stereo[i])/32768.0f*(volumes?1.0f:selection.input_gain);
                const float r=float(pcm.stereo[i+1])/32768.0f*(volumes?1.0f:selection.input_gain);
                mixed.push_back(l*selection.left_gain+r*selection.right_gain);
                mixed.push_back(r*selection.left_gain+l*selection.right_gain);
            }
            if(!SDL_PutAudioStreamData(output,mixed.data(),static_cast<int>(mixed.size()*sizeof(float))))throw AudioError("Music refill queue failed");
            decoder=next;decoded+=pcm.frames;submitted+=pcm.frames;
            read.Drain();
            if(decoder.next_block==0)
            {
                ++cycles;
                // Original source stores the play count in12bits and starts its
                // loop counter at1. ffffffff therefore means4095 traversals in
                // this selected reconstruction, not an invented infinite loop.
                if(cycles>=(selection.play_count&0xfff)){ended=true;if(!SDL_FlushAudioStream(output))throw AudioError("Music final flush failed");}
            }
        }
        bool PreparePoll(std::uint32_t device)
        {
            if(!read.Ready())return false;
            if(!header)
            {
                format=resources::ReadAudioStreamFormat(bank,selection.source,read.Bytes());decoder=resources::BeginAudioStream(format);header=true;
                read.Drain();read.Issue(file.get(),format->BlockOffset(0,0),format->BlockBytes()*2);return false;
            }
            if(!audio){if(!SDL_InitSubSystem(SDL_INIT_AUDIO))throw AudioError("Music audio initialization failed");audio=true;}
            const SDL_AudioSpec spec{SDL_AUDIO_F32,2,static_cast<int>(format->Channels()[0].rate)};
            output=SDL_OpenAudioDeviceStream(device?device:SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK,&spec,nullptr,nullptr);
            if(!output)throw AudioError("Music stream creation failed");
            if(volumes)event.UpdatePlaybackParameters(true);
            Queue();prepared=true;m_Unknown10=3;event.Update(0);return true;
        }
        void Query()
        {
            if(!output){queued=available=0;return;}
            queued=SDL_GetAudioStreamQueued(output);available=SDL_GetAudioStreamAvailable(output);
            if(queued<0||available<0)throw AudioError("Music stream query failed");
        }
        void Poll()
        {
            Query();if(m_Unknown10==6){m_Unknown10=1;event.Update(0);return;}
            if(m_Unknown10!=4&&m_Unknown10!=7)return;
            if(read.issued&&read.Ready())Queue();
            Query();
            if(ended){if(!queued&&!available&&m_Unknown10==4){m_Unknown10=6;}event.Update(0);return;}
            event.Update(0);
            // Bound SDL queued input to at most two decoded authored blocks.
            // Polling stalls produce a genuine host underrun, never fake progress.
            const auto block_bytes=std::uint64_t(format->BlockBytes()/8)*14*2*sizeof(float);
            if(!paused&&!read.issued&&std::uint64_t(queued)+available<block_bytes)
                read.Issue(file.get(),format->BlockOffset(decoder.next_block,0),format->BlockBytes()*2);
        }
        void Stop()
        {
            std::exception_ptr failure;
            try {AudioStreamStopSteps(*this,[&]{
                if(output&&!SDL_PauseAudioStreamDevice(output))throw AudioError("Music pause on stop failed");
            },[&]{return read.issued&&!read.complete;},[&]{read.Drain();});}
            catch(...){failure=std::current_exception();}
            // Native cancellation retires queued host output even for original
            // pending-stop cases; no AX voice or original readiness is fabricated.
            Close();m_Unknown10=6;paused=false;queued=available=0;
            if(failure)std::rethrow_exception(failure);
        }
    };
    const std::thread::id thread=std::this_thread::get_id();
    resources::AudioCalculationInitial::Handle calculation;
    FrontendMusicOptions options;
    std::string base;
    FrontendMusicLoadState state=FrontendMusicLoadState::Idle;
    std::unique_ptr<Stream> current,pending;
    std::unique_ptr<nlFile> metadata,wave;
    Read read;
    unsigned pending_seed=0;
    std::uint32_t pending_hash=0;
    std::exception_ptr error;
    bool servicing=false;
    std::uint64_t retired_requested=0,retired_completed=0;
    Implementation(resources::AudioBankCatalog::Handle catalog,resources::AudioCalculationInitial::Handle calc,FrontendMusicOptions o)
        :calculation(std::move(calc)),options(o)
    {
        Mutable();Require(catalog&&calculation&&catalog->names.size()>26&&catalog->slots.size()>22,"Music requires retained catalog/calculation");
        if(options.category_volumes)options.category_volumes->RequireCompatible(calculation);
        const auto& name=catalog->names[26].name;
        Require(name=="FE_GEN_Music"&&catalog->slots[22].streaming,"Music requires original FE_GEN_Music name26/stream slot22");
        base="audio/"+name;
    }
    void Thread()const{if(thread!=std::this_thread::get_id())throw std::logic_error("Music requires its owning thread");}
    void Mutable()const{Thread();if(servicing||nlGetCurrentAsyncRead())throw std::logic_error("Music mutation during NL callback is unsupported");}
    void Retire(std::unique_ptr<Stream>& s)
    {if(s){retired_requested+=s->read.requested;retired_completed+=s->read.completed;s.reset();}}
    void Cancel()
    {read.Drain();metadata.reset();wave.reset();Retire(pending);}
    void Fail(std::exception_ptr e){if(!error)error=e;Cancel();state=FrontendMusicLoadState::Failed;}
    void Begin(unsigned index,unsigned seed)
    {
        Require(index<=1,"Only Title and Main frontend music are selected");
        if(state==FrontendMusicLoadState::Unloaded)throw std::logic_error("Music bank has been unloaded");
        if(state==FrontendMusicLoadState::Loading)throw std::logic_error("Music replacement is already pending");
        const auto hash=index?0x445abf3a:0xe326f931;
        if(current&&current->selection.cue==hash&&current->m_Unknown10!=1&&current->m_Unknown10!=6){error=nullptr;state=FrontendMusicLoadState::Ready;return;}
        Require(gMemoryInitialized&&nlFileSystemReady(),"Music requires initialized NL services");
        Cancel();error=nullptr;pending_hash=hash;pending_seed=seed;
        try
        {
            metadata.reset(nlOpen((base+".resbun").c_str()));wave.reset(nlOpen((base+".nlxwb").c_str()));
            Require(bool(metadata)&&bool(wave),"Frontend music resource is missing");
            const auto size=nlFileSize(metadata.get(),nullptr),wave_size=nlFileSize(wave.get(),nullptr);
            Require(size&&size<=resources::MaximumAssetBytes&&wave_size&&wave_size<=256*1024*1024,"Frontend music file size exceeds selected limit");
            read.Issue(metadata.get(),0,size);state=FrontendMusicLoadState::Loading;
        }
        catch(...){Fail(std::current_exception());throw;}
    }
    void Poll()
    {
        if(current)
        {
            try{current->Poll();}catch(...){const auto failure=std::current_exception();Retire(current);Fail(failure);return;}
        }
        if(state!=FrontendMusicLoadState::Loading)return;
        try
        {
            Require(nlFileSystemReady(),"Music NL services stopped");
            if(!pending)
            {
                if(!read.Ready())return;
                auto bank=resources::ReadAudioStreamBank(read.Bytes(),nlFileSize(wave.get(),nullptr));
                const auto selection=Select(bank,calculation,pending_hash,pending_seed,options.category_volumes);
                auto next=std::make_unique<Stream>();next->Begin(bank,calculation,std::move(wave),selection,options.category_volumes);
                read.Drain();metadata.reset();pending=std::move(next);return;
            }
            if(!pending->PreparePoll(options.device_id))return;
            pending->PlayEvent();
            // Everything that can allocate/read/open has completed. Publish only
            // after actual output admission; releasing old resources cannot throw.
            Retire(current);current=std::move(pending);state=FrontendMusicLoadState::Ready;
        }
        catch(...){Fail(std::current_exception());}
    }
};
FrontendMusic::FrontendMusic(resources::AudioBankCatalog::Handle catalog,resources::AudioCalculationInitial::Handle calc,FrontendMusicOptions options)
    :impl_(std::make_unique<Implementation>(std::move(catalog),std::move(calc),options)){}
FrontendMusic::~FrontendMusic(){try{Unload();}catch(...){std::terminate();}}
void FrontendMusic::BeginSelect(unsigned index,unsigned& seed){impl_->Mutable();impl_->Begin(index,seed);}
void FrontendMusic::Poll(){impl_->Mutable();impl_->Poll();}
void FrontendMusic::Service()
{
    auto& s=*impl_;s.Mutable();s.Poll();
    if(s.state==FrontendMusicLoadState::Unloaded)return;
    s.servicing=true;
    try{nlServiceFileSystem();}catch(...){s.servicing=false;s.Fail(std::current_exception());if(s.current){try{s.current->Poll();}catch(...){s.Retire(s.current);}}throw;}
    s.servicing=false;s.Poll();
}
void FrontendMusic::CancelPending(){auto& s=*impl_;s.Mutable();if(s.state!=FrontendMusicLoadState::Loading)return;s.Cancel();s.state=FrontendMusicLoadState::Cancelled;}
bool FrontendMusic::Pause()
{
    auto& s=*impl_;s.Mutable();if(!s.current)return false;auto& c=*s.current;
    return AudioStreamPauseSteps(c,[&]{if(!SDL_PauseAudioStreamDevice(c.output))throw AudioError("Music pause failed");c.paused=true;c.event.state=5;});
}
bool FrontendMusic::Resume()
{
    auto& s=*impl_;s.Mutable();if(!s.current)return false;auto& c=*s.current;
    return AudioStreamResumeSteps(c,[&]{if(!SDL_ResumeAudioStreamDevice(c.output))throw AudioError("Music resume failed");c.paused=false;c.event.state=4;});
}
void FrontendMusic::Stop(){auto& s=*impl_;s.Mutable();s.Cancel();if(s.current)AudioPlaybackStopStep(s.current->event);s.state=FrontendMusicLoadState::Idle;}
void FrontendMusic::Unload(std::uint32_t slot)
{auto& s=*impl_;s.Mutable();Require(slot==22,"Frontend music unload requires slot22");s.Cancel();s.Retire(s.current);s.calculation.reset();s.state=FrontendMusicLoadState::Unloaded;}
FrontendMusicStatus FrontendMusic::Status()const
{
    auto& s=*impl_;s.Thread();FrontendMusicStatus result;result.load=s.state;
    result.requested_reads=s.read.requested+s.retired_requested;result.completed_reads=s.read.completed+s.retired_completed;
    for(const auto* stream:{s.current.get(),s.pending.get()})if(stream){result.requested_reads+=stream->read.requested;result.completed_reads+=stream->read.completed;}
    if(s.current)
    {
        auto& c=*s.current;c.Query();result.cue=c.selection.cue;result.source=c.selection.source;
        result.source_state=c.m_Unknown10;result.event_state=c.event.state;result.maximum_cycles=c.m_PlayCount;
        result.decoded_frames=c.decoded;result.submitted_frames=c.submitted;result.completed_cycles=c.cycles;
        result.queued_input_bytes=c.queued;result.available_output_bytes=c.available;result.paused=c.paused;
        result.volume_db=c.selection.prepared_db;result.input_gain=c.volumes?c.selection.input_gain:1.0f;
    }
    return result;
}
void FrontendMusic::Check()const{impl_->Thread();if(impl_->error)std::rethrow_exception(impl_->error);}
}
