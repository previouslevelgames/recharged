#include "runtime/frontend_audio.h"
#include "Game/Audio/AudioCuePlaybackSteps.h"
#include "Game/Audio/AudioResidentSteps.h"
#include "Game/Audio/Transition.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <exception>
#include <limits>
#include <thread>

namespace mscharged
{
namespace
{
void Check(bool value,const char* why){if(!value)throw std::logic_error(why);}
std::uint64_t Identity()
{
    static std::atomic<std::uint64_t> next{1};auto value=next.load();
    for(;;){Check(value!=UINT64_MAX,"Frontend audio identity exhausted");if(next.compare_exchange_weak(value,value+1))return value;}
}
struct Definition { bool useSlider=false;std::uint32_t activeCount=0,maximumCount=0; };
struct Instance;
struct Sequence;
struct Voice
{
    AudioVoices* voices;
    AudioVoiceHandle handle;
    float prepared_volume;
    bool closed=false;
    Voice(AudioVoices& output,const AudioBankSelectionResult& selected,resources::AudioCalculationInitial::Handle calculation)
        :voices(&output)
    {
        const auto& definition=selected.bank->Voices().at(selected.voice);
        const auto& sequence=selected.bank->Sequences().at(selected.events.at(0).sequence);
        prepared_volume=AudioResidentPlaybackVolume(0.0f,
            AudioResidentInstanceVolume(0.0f,calculation->Value(definition.slider),std::clamp(definition.volume,-96.0f,6.0f)),0.0f,sequence.volume);
        handle=voices->Create(selected,std::move(calculation));
    }
    void Close(){if(!closed){closed=true;voices->Destroy(handle);}}
    ~Voice(){try{Close();}catch(...){}}
    bool Prepare(){return voices->Prepare(handle);}
    void Stop(){voices->Stop(handle);}
    bool Play(unsigned value){return voices->Play(handle,value);}
    void UpdateState(){voices->PollState(handle);}
    int GetState()const{return voices->Status(handle).state;}
};
struct Event
{
    Event* next=nullptr;
    Sequence* owner;
    std::unique_ptr<Voice> storage;
    Voice* source=nullptr;
    int state=0;
    unsigned flags=0;
    float startTime=0,currentVolume=0,currentPitch=0;
    explicit Event(Sequence* sequence):owner(sequence){}
    void Prepare(){AudioPlaybackPrepareStep(*this);}
    void Stop(){AudioPlaybackStopStep(*this);}
    void Play(){AudioPlaybackPlayStep(*this);}
    int Update(float){return AudioPlaybackUpdateStep(*this);}
    void StartPlayback();
    void UpdatePlaybackParameters(bool);
};
struct Sequence
{
    Instance* soundInstance;
    std::unique_ptr<Event> storage;
    Event* events=nullptr;
    Sequence* next=nullptr;
    bool stopped=false;
    float volumeOffset=0;
    explicit Sequence(Instance* instance):soundInstance(instance){}
    void Play(){AudioSequencePlayStep(*this);}
    void Prepare(){AudioSequencePrepareStep(*this);}
    void Stop(){AudioSequenceStopStep(*this);}
    int Update(float dt){return AudioSequenceUpdateStep(*this,dt);}
};
struct Instance
{
    Transition volume,pitch;
    std::unique_ptr<Sequence> storage;
    Sequence* voices=nullptr;
    Instance* nextInstance=nullptr;
    void* activeRpc=nullptr;
    SoundInstanceState state=SOUND_INSTANCE_STATE_INITIAL;
    float previousTime=-1,currentTime=0;
    float target_volume=0,target_pitch=0,slider=0;
    explicit Instance(const AudioBankSelectionResult& selected,resources::AudioCalculationInitial::Handle calculation,AudioVoices& output)
    {
        const auto& voice=selected.bank->Voices().at(selected.voice);
        const auto& event=selected.events.at(0);
        target_volume=voice.volume;target_pitch=voice.pitch;slider=calculation->Value(voice.slider);
        volume.Reset(0,-96,6);pitch.Reset(0,-12,12);
        storage=std::make_unique<Sequence>(this);storage->volumeOffset=selected.bank->Sequences().at(event.sequence).volume;
        storage->storage=std::make_unique<Event>(storage.get());storage->events=storage->storage.get();
        storage->events->startTime=event.start_time;
        storage->events->storage=std::make_unique<Voice>(output,selected,std::move(calculation));
        storage->events->source=storage->events->storage.get();voices=storage.get();
    }
    void Prepare()
    {
        // Original static, zero-RPC profile: targets precede sequence preparation.
        // RPC lists and later release/slider automation are rejected by the readers.
        volume.SetTarget(target_volume,0);pitch.SetTarget(target_pitch,0);
        if(voices){voices->Prepare();state=SOUND_INSTANCE_STATE_PREPARING;}
        else state=SOUND_INSTANCE_STATE_PREPARED;
    }
    void ReleaseSequences()
    {
        std::exception_ptr error;
        try{if(storage&&storage->events&&storage->events->storage)storage->events->storage->Close();}catch(...){error=std::current_exception();}
        storage.reset();voices=nullptr;
        if(error)std::rethrow_exception(error);
    }
    void Play(float){AudioInstancePlayStep(*this);}
    void Stop(void* force){AudioInstanceStopStep(*this,force);}
    void Update(float dt)
    {
        Check(!activeRpc&&!nextInstance,"Frontend audio static instance has unsupported RPC/slider state");
        AudioInstanceClockStep(*this,dt);
        const int next=voices?voices->Update(dt):SOUND_INSTANCE_STATE_STOPPED;
        AudioInstanceStateStep(*this,next,[&]{ReleaseSequences();});
    }
};
void Event::UpdatePlaybackParameters(bool force)
{
    const auto& instance=*owner->soundInstance;
    const float db=AudioResidentPlaybackVolume(0.0f,AudioResidentInstanceVolume(0.0f,instance.slider,instance.volume.value),0.0f,owner->volumeOffset);
    // This profile has immutable calculation/sequence gain and no modifiers.
    // PCM was prepared with exactly this gain before publication. Re-evaluate
    // the original parameter equation; never accept a silent dynamic change.
    Check(std::isfinite(db)&&db==source->prepared_volume&&instance.pitch.value==0,
          "Frontend audio parameters changed outside the static output profile");
    if(force||currentVolume!=db)currentVolume=db;
    currentPitch=0;
}
void Event::StartPlayback()
{
    // Random pitch/volume and non-one-shot events were rejected at Voice creation.
    currentVolume=-96;currentPitch=0;UpdatePlaybackParameters(true);
    source->Play(1);state=4;
}
struct Cue
{
    FrontendAudioHandle handle;
    std::uint32_t index=0,hash=0;
    Definition* definition=nullptr;
    std::unique_ptr<Instance> storage;
    Instance* instance=nullptr;
    int m_State=1;
    bool m_CallbackEnabled=false,counted=false,limited=false;
    float m_PreviousTime=0,m_CurrentTime=0;
    struct {bool flag8000=true,playWhenPrepared=false;}bits;
    std::optional<std::uint32_t> sample;
    ~Cue(){if(counted)AudioCueReleaseCount(*definition);}
    bool Prepare(bool callback){return AudioCuePrepareStep(*this,callback);}
    bool Play(bool callback){return AudioCuePlayStep(*this,callback);}
    void Stop(bool callback){AudioCueStopStep(*this,callback,nullptr);}
    void Update(float dt){AudioCueUpdateStep(*this,dt);}
    void Release(){AudioCueReleaseStep(*this);}
    void UpdateSlider(float){throw resources::UnsupportedResource("Frontend audio slider cues are unavailable");}
};
}
struct FrontendAudio::Implementation
{
    struct Slot {std::uint32_t generation=0;std::unique_ptr<Cue> cue;};
    const std::thread::id thread=std::this_thread::get_id();
    const std::uint64_t identity=Identity();
    LoadedAudioBank::Handle bank;
    resources::AudioCalculationInitial::Handle calculation;
    std::unique_ptr<AudioBankSelection> selection;
    AudioVoices voices;
    std::vector<Definition> definitions;
    std::vector<Slot> slots;
    std::vector<std::uint32_t> order;
    bool loaded=true,enabled=true,busy=false;
    Implementation(LoadedAudioBank::Handle b,resources::AudioCalculationInitial::Handle c,AudioVoicesOptions options)
        :bank(std::move(b)),calculation(std::move(c)),voices(options)
    {
        resources::Require(bank&&bank->bank&&calculation,"Frontend audio needs retained checked assets");
        resources::Require(bank->name_index==23&&bank->slot_index==21&&bank->name.name=="FE_GEN_Sfx"&&!bank->slot.streaming,
                           "Frontend audio requires resident FE_GEN_Sfx name23/slot21");
        // Copy identity records because a caller can retain a mutable alias.
        bank=std::make_shared<const LoadedAudioBank>(*bank);
        selection=std::make_unique<AudioBankSelection>(bank->bank);
        for(const auto& cue:bank->bank->Cues())definitions.push_back({false,0,cue.maximum_count});
        slots.resize(options.capacity);order.reserve(options.capacity);
    }
    void Thread()const{Check(thread==std::this_thread::get_id(),"Frontend audio requires its creating thread");}
    void Ready()const{Thread();Check(loaded,"Frontend audio bank is unloaded");}
    void Mutable()const{Ready();Check(!busy,"Frontend audio cannot mutate during update");}
    void IdentityCheck(FrontendAudioHandle h)const
    {Thread();Check(h.owner==identity&&h.slot<slots.size()&&h.generation&&h.generation<=slots[h.slot].generation,"Frontend audio handle is foreign or unissued");}
    Cue* Find(FrontendAudioHandle h)const
    {IdentityCheck(h);const auto& s=slots[h.slot];return s.generation==h.generation?s.cue.get():nullptr;}
    Cue& Get(FrontendAudioHandle h)const
    {Ready();auto* cue=Find(h);Check(cue!=nullptr,"Frontend audio handle is retired");return *cue;}
    std::uint32_t Index(std::uint32_t hash)const
    {Ready();const auto index=bank->bank->CuesByKey().Find({hash,0,0,0});Check(index!=0xffff,"Frontend audio cue is absent");return index;}
    void Erase(std::uint32_t slot)
    {
        const auto at=std::find(order.begin(),order.end(),slot);Check(at!=order.end(),"Frontend cue is outside its list");
        order.erase(at);auto cue=std::move(slots[slot].cue);
        // Original cue destruction decrements the count before instance/source teardown.
        if(cue->counted){AudioCueReleaseCount(*cue->definition);cue->counted=false;}
        if(cue->instance)cue->instance->ReleaseSequences();
    }
    void Drain()
    {
        loaded=false;
        std::exception_ptr error;
        while(!order.empty())try{Erase(order.front());}catch(...){if(!error)error=std::current_exception();}
        try{voices.Release();}catch(...){if(!error)error=std::current_exception();}
        selection.reset();bank.reset();calculation.reset();
        if(error)std::rethrow_exception(error);
    }
};
FrontendAudio::FrontendAudio(LoadedAudioBank::Handle bank,resources::AudioCalculationInitial::Handle calculation,AudioVoicesOptions options)
    :impl_(std::make_unique<Implementation>(std::move(bank),std::move(calculation),options)){}
FrontendAudio::~FrontendAudio()
{
    if(impl_->thread!=std::this_thread::get_id()||impl_->busy)std::terminate();
    try{if(impl_->loaded)impl_->Drain();}catch(...){}
}
std::optional<FrontendAudioHandle> FrontendAudio::Play(std::uint32_t hash,unsigned& seed,bool auto_release)
{
    auto& p=*impl_;p.Mutable();if(!p.enabled||hash==UINT32_MAX)return{};
    const auto index=p.bank->bank->CuesByKey().Find({hash,0,0,0});if(index==0xffff)return{};
    auto free=std::find_if(p.slots.begin(),p.slots.end(),[](const auto& slot){return !slot.cue&&slot.generation!=UINT32_MAX;});
    Check(free!=p.slots.end(),"Frontend cue capacity/generation space exhausted");
    auto definition=p.definitions.at(index);Check(definition.activeCount!=UINT32_MAX,"Frontend cue active count exhausted");
    AudioCueRetainCount(definition);
    auto cue=std::make_unique<Cue>();cue->definition=&p.definitions[index];cue->index=index;cue->hash=hash;
    cue->handle={p.identity,std::uint32_t(free-p.slots.begin()),free->generation+1};
    std::unique_ptr<AudioBankSelection> next_selection;auto next_seed=seed;
    if(definition.activeCount>definition.maximumCount){cue->m_State=6;cue->limited=true;}
    else
    {
        next_selection=std::make_unique<AudioBankSelection>(*p.selection);
        auto selected=next_selection->Select({hash,0,0,0},next_seed);Check(bool(selected),"Frontend cue selection disappeared");
        cue->sample=selected->events.at(0).sample;
        cue->storage=std::make_unique<Instance>(*selected,p.calculation,p.voices);cue->instance=cue->storage.get();
    }
    cue->Play(auto_release); // All allocations/output admission finish before publication.
    const auto handle=cue->handle;
    free->generation=handle.generation;free->cue=std::move(cue);p.order.push_back(handle.slot);
    p.definitions[index].activeCount=definition.activeCount;free->cue->counted=true;
    if(next_selection){p.selection.swap(next_selection);seed=next_seed;}
    return handle;
}
void FrontendAudio::ServiceAudio()
{
    auto& p=*impl_;p.Mutable();p.busy=true;
    try{p.voices.ServiceAudio();p.busy=false;}catch(...){p.busy=false;const auto failure=std::current_exception();try{p.Drain();}catch(...){}std::rethrow_exception(failure);}
}
void FrontendAudio::Update(float dt)
{
    auto& p=*impl_;p.Mutable();resources::Require(std::isfinite(dt)&&dt>=0&&dt<=1,"Frontend audio delta must be finite0..1");
    for(auto slot:p.order)
    {
        const auto& cue=*p.slots[slot].cue;
        Check(cue.m_CurrentTime<=1e7f-dt&&(!cue.instance||cue.instance->currentTime<=1e7f-dt),"Frontend audio clock budget exhausted");
    }
    p.busy=true;
    try
    {
        for(std::size_t i=0;i<p.order.size();)
        {
            const auto slot=p.order[i];auto& cue=*p.slots[slot].cue;cue.Update(dt);
            if(cue.m_State==9)p.Erase(slot);else ++i;
        }
        p.busy=false;
    }
    catch(...){p.busy=false;const auto failure=std::current_exception();try{p.Drain();}catch(...){}std::rethrow_exception(failure);}
}
void FrontendAudio::Stop(FrontendAudioHandle handle,bool auto_release)
{
    auto& p=*impl_;p.Mutable();auto& cue=p.Get(handle);
    // Original StopSound dispatch never calls instance->Stop on failed6/retired9.
    switch(cue.m_State)
    {
    case 2:case 3:case 4:case 5:cue.Stop(auto_release);break;
    case 7:cue.m_CallbackEnabled=auto_release;break;
    case 8:cue.Release();break;
    }
}
void FrontendAudio::Release(FrontendAudioHandle handle){auto& p=*impl_;p.Mutable();p.Get(handle).Release();}
void FrontendAudio::Cancel(FrontendAudioHandle handle){auto& p=*impl_;p.Mutable();p.Get(handle);p.Erase(handle.slot);}
FrontendAudioStatus FrontendAudio::Status(FrontendAudioHandle handle)const
{
    const auto& cue=impl_->Get(handle);FrontendAudioStatus status{cue.hash,cue.m_State,0,0,0,cue.m_CallbackEnabled,cue.limited,cue.sample};
    if(cue.instance)
    {
        status.instance_state=cue.instance->state;
        if(cue.instance->voices)
        {
            const auto& event=*cue.instance->voices->events;status.event_state=event.state;status.source_state=event.source->GetState();
        }
    }
    return status;
}
bool FrontendAudio::IsFinished(FrontendAudioHandle handle)const
{const auto* cue=impl_->Find(handle);return !cue||AudioFrontendFinishedStep(cue->m_State);}
std::uint32_t FrontendAudio::ActiveCount(std::uint32_t hash)const{return impl_->definitions.at(impl_->Index(hash)).activeCount;}
AudioCueSelectionState FrontendAudio::SelectionState(std::uint32_t hash)const
{const auto index=impl_->Index(hash);return impl_->selection->State(index);}
std::vector<FrontendAudioHandle> FrontendAudio::Handles()const
{impl_->Ready();std::vector<FrontendAudioHandle> out;out.reserve(impl_->order.size());for(auto i:impl_->order)out.push_back(impl_->slots[i].cue->handle);return out;}
void FrontendAudio::Enable(bool value){impl_->Mutable();impl_->enabled=value;}
bool FrontendAudio::Enabled()const{impl_->Thread();return impl_->enabled;}
void FrontendAudio::Unload(std::uint32_t slot)
{impl_->Thread();Check(!impl_->busy,"Frontend audio cannot unload during update");Check(slot==21,"Frontend audio cannot unload an unrelated bank");if(impl_->loaded)impl_->Drain();}
bool FrontendAudio::Loaded()const{impl_->Thread();return impl_->loaded;}
}
