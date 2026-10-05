#include "runtime/audio_voices.h"
#include "Game/Audio/AudioVoiceSteps.h"
#include <algorithm>
#include <atomic>
#include <exception>
#include <limits>
#include <stdexcept>
#include <thread>

namespace mscharged
{
namespace
{
void Check(bool value,const char* message){if(!value)throw std::logic_error(message);}
std::uint64_t NewIdentity()
{
    static std::atomic<std::uint64_t> sequence{1};
    auto value=sequence.load();
    for(;;)
    {
        if(value==UINT64_MAX)throw std::length_error("Audio voice owner identity space exhausted");
        if(sequence.compare_exchange_weak(value,value+1))return value;
    }
}
struct VoiceInputs
{
    const AudioBankSelectionResult& selected;
    resources::AudioCalculationInitial::Handle calculation;
    std::uint32_t device;
};
struct Source
{
    AudioVoiceHandle handle;
    ResidentAudioBuffer::Handle buffer;
    std::unique_ptr<ResidentAudioOutput> output;
    std::size_t input_bytes=0;
    int m_Unknown04=0,m_Unknown10=0;
    unsigned m_PlayCount=0,m_Unknown14_0C=1,m_Unknown18=0;
    bool started=false,failed=false;
    void Initialize(VoiceInputs* inputs)
    {
        buffer=PrepareResidentAudio(inputs->selected,std::move(inputs->calculation));
        output=std::make_unique<ResidentAudioOutput>(buffer,inputs->device);
        input_bytes=buffer->Stereo().size_bytes();
        m_Unknown10=1;m_Unknown04=1;
    }
    bool HasVoice()const{return bool(output);}
    bool WasVoiceDropped()const{return failed;}
    void ReleaseVoice(bool release)
    {
        Check(release,"Native audio does not provide AX voice stealing");
        if(output){auto owned=std::move(output);owned->Stop();}
    }
    void SetLoop(bool enabled)
    { if(enabled)throw resources::UnsupportedResource("Native resident voice looping is unavailable"); }
    void SetLoopAddress(unsigned long)
    { throw resources::UnsupportedResource("Native resident loop addresses are unavailable"); }
    unsigned long SilenceAddress()
    { throw resources::UnsupportedResource("Native resident silence-loop addresses are unavailable"); }
    void Start()
    {
        Check(output&&!started,"Native resident voice cannot restart released/started output");
        output->Start();started=true;
    }
    bool Stopped()
    {
        Check(bool(output),"Native resident update has no output");
        const auto state=output->Status().state;
        return state==ResidentAudioState::InputConsumed||state==ResidentAudioState::Stopped;
    }
    unsigned CurrentAddress()
    {
        Check(bool(output),"Native resident cursor has no output");
        const auto queued=output->Status().queued_input_bytes;
        Check(queued>=0&&std::size_t(queued)<=input_bytes,"Native resident queue exceeds admitted bytes");
        // Only monotonic ordering participates in the qualified one-shot source
        // branch. This is an input-frame cursor, never a Wii DSP/nibble address.
        const auto frames=(input_bytes-std::size_t(queued))/(2*sizeof(float));
        Check(frames<=UINT32_MAX&&frames>=m_Unknown18,"Native resident cursor moved backwards");
        return unsigned(frames);
    }
    void StopAtSilence()
    { if(output)output->Stop(); } // Actual pause/clear/destroy; no synthetic silence pointer.
    void Fail()noexcept
    {
        failed=true;
        try{ReleaseVoice(true);}catch(...){}
    }
};
}
struct AudioVoices::Implementation
{
    struct Slot{std::uint32_t generation=0;std::unique_ptr<Source> source;};
    AudioVoicesOptions options;
    const std::thread::id thread=std::this_thread::get_id();
    const std::uint64_t identity=NewIdentity();
    std::vector<Slot> slots;
    std::vector<std::uint32_t> order;
    AudioVoiceHandle last{};
    std::size_t bytes=0;
    bool live=true,busy=false;
    explicit Implementation(AudioVoicesOptions value):options(value)
    {
        Check(options.capacity&&options.capacity<=64,"Native audio voice capacity must be1..64");
        Check(options.input_byte_budget&&options.input_byte_budget<=256*1024*1024,
              "Native audio voice input budget must be1..256MiB");
        slots.resize(options.capacity);order.reserve(options.capacity);
    }
    void Thread()const{Check(thread==std::this_thread::get_id(),"Audio voices require their creating thread");}
    void Ready()const{Thread();Check(live,"Audio voice owner has been released");}
    void Mutable()const{Ready();Check(!busy,"Audio voices cannot mutate during source service");}
    Source& Get(AudioVoiceHandle h)const
    {
        Ready();Check(h.owner==identity&&h.slot<slots.size()&&h.generation,
                     "Audio voice handle belongs to a different owner");
        const auto& slot=slots[h.slot];Check(slot.source&&slot.generation==h.generation,
                                           "Audio voice handle is stale");return *slot.source;
    }
    struct Registry
    {
        Implementation& owner;
        std::unique_ptr<Source>* pending=nullptr;
        void Add(Source* source)
        {
            auto& slot=owner.slots[source->handle.slot];
            // Capacity/order storage was reserved before output preparation.
            slot.generation=source->handle.generation;slot.source=std::move(*pending);
            owner.order.push_back(source->handle.slot);owner.bytes+=source->input_bytes;
        }
        void LastCreated(Source* source){owner.last=source->handle;}
        void Remove(Source* source)
        {
            const auto found=std::find(owner.order.begin(),owner.order.end(),source->handle.slot);
            Check(found!=owner.order.end(),"Audio source is outside its ownership list");
            owner.order.erase(found);owner.bytes-=source->input_bytes;
        }
        void Destroy(Source* source)
        {
            auto owned=std::move(owner.slots[source->handle.slot].source);
            owned->ReleaseVoice(true); // Slot is already invalid even if real Stop reports failure.
        }
    };
};
AudioVoices::AudioVoices(AudioVoicesOptions options):impl_(std::make_unique<Implementation>(options)){}
AudioVoices::~AudioVoices()
{
    if(impl_->thread!=std::this_thread::get_id()||impl_->busy)std::terminate();
    try{Release();}catch(...){} // Every stream was nevertheless destroyed by Release.
}
AudioVoiceHandle AudioVoices::Create(const AudioBankSelectionResult& selected,resources::AudioCalculationInitial::Handle calculation)
{
    impl_->Mutable();Check(selected.bank&&calculation&&selected.events.size()==1,"Native audio requires one selected retained event");
    const auto sample=selected.events.front().sample;
    const auto count=selected.bank->Samples().at(sample).sample_count;
    Check(count<=std::numeric_limits<std::size_t>::max()/(2*sizeof(float)),"Native audio input byte count overflow");
    const auto bytes=std::size_t(count)*2*sizeof(float);
    Check(bytes<=impl_->options.input_byte_budget-impl_->bytes,"Native audio input byte budget exhausted");
    const auto free=std::find_if(impl_->slots.begin(),impl_->slots.end(),[](const auto& slot){return !slot.source&&slot.generation!=UINT32_MAX;});
    Check(free!=impl_->slots.end(),"Native audio voice capacity/generation space exhausted");
    auto source=std::make_unique<Source>();
    source->handle={impl_->identity,std::uint32_t(free-impl_->slots.begin()),free->generation+1};
    const auto handle=source->handle;
    VoiceInputs inputs{selected,std::move(calculation),impl_->options.device_id};
    Implementation::Registry registry{*impl_,&source};
    AudioInitializeAndPublishSource(source.get(),&inputs,registry);
    return handle;
}
bool AudioVoices::Prepare(AudioVoiceHandle handle)
{
    impl_->Mutable();auto& source=impl_->Get(handle);
    Check(!source.failed&&!source.started&&source.HasVoice()&&source.m_Unknown10!=6,
          "Native audio source cannot prepare completed/failed output");
    return AudioSamplePrepareStep(source);
}
bool AudioVoices::Play(AudioVoiceHandle handle,unsigned count)
{
    impl_->Mutable();auto& source=impl_->Get(handle);
    if(count!=1)throw resources::UnsupportedResource("Native resident voices support play-count1 only");
    Check(!source.failed&&!source.started&&source.HasVoice()&&source.m_Unknown10!=6,
          "Native audio source cannot replay completed/failed output");
    return AudioSamplePlayStep(source,count);
}
void AudioVoices::Stop(AudioVoiceHandle handle)
{
    impl_->Mutable();auto& source=impl_->Get(handle);Check(!source.failed,"Native audio source failed");
    try{AudioSampleStopStep(source,source);}catch(...){source.Fail();throw;}
}
void AudioVoices::ServiceAudio()
{
    impl_->Mutable();impl_->busy=true;struct Guard{bool& busy;~Guard(){busy=false;}}guard{impl_->busy};
    for(auto slot:impl_->order)
    {
        auto& source=*impl_->slots[slot].source;
        if(source.failed)continue; // Inspect/destroy failed sources; unrelated output remains serviceable.
        try{AudioSampleUpdateStep(source,source);}catch(...){source.Fail();throw;}
    }
}
int AudioVoices::PollState(AudioVoiceHandle handle)
{
    impl_->Mutable();auto& source=impl_->Get(handle);Check(!source.failed,"Native audio source failed");
    try{AudioSourceStateStep(source);}catch(...){source.Fail();throw;}
    return source.m_Unknown04;
}
AudioVoiceStatus AudioVoices::Status(AudioVoiceHandle handle)const
{
    const auto& source=impl_->Get(handle);
    return{source.m_Unknown04,source.m_Unknown10,source.HasVoice(),source.failed,source.buffer->Sample(),source.input_bytes};
}
std::vector<AudioVoiceHandle> AudioVoices::Sources()const
{
    impl_->Ready();std::vector<AudioVoiceHandle> result;result.reserve(impl_->order.size());
    for(auto slot:impl_->order)result.push_back(impl_->slots[slot].source->handle);return result;
}
AudioVoiceHandle AudioVoices::LastCreated()const{impl_->Ready();return impl_->last;}
void AudioVoices::Destroy(AudioVoiceHandle handle)
{
    impl_->Mutable();auto& source=impl_->Get(handle);Implementation::Registry registry{*impl_};
    AudioRemoveAndDestroySource(&source,registry);
}
void AudioVoices::Release()
{
    impl_->Thread();Check(!impl_->busy,"Audio voices cannot release during source service");
    if(!impl_->live)return;
    std::exception_ptr failure;
    while(!impl_->order.empty())
    {
        const auto handle=impl_->slots[impl_->order.front()].source->handle;
        try{Destroy(handle);}catch(...){if(!failure)failure=std::current_exception();}
    }
    impl_->live=false;impl_->last={};
    if(failure)std::rethrow_exception(failure);
}
}
