#include "runtime/audio_volume.h"
#include "Game/Audio/AudioCalculationSteps.h"
#include "Game/Audio/AudioOptionsVolumeSteps.h"
#include <bit>
#include <cmath>
#include <thread>
namespace mscharged
{
struct AudioCategoryVolumes::Implementation
{
    resources::AudioVolumeProfile::Handle profile;
    const std::thread::id thread=std::this_thread::get_id();
    std::array<AudioCalculationDefinition,5> definitions{};
    std::array<AudioCalculationSlider,5> sliders{};
    std::array<int,3> settings{};s32 global=0;std::uint64_t frame=0;
    void Check()const{if(thread!=std::this_thread::get_id())throw std::logic_error("Audio category volumes require their owning thread");}
    static unsigned Index(AudioCategory category)
    {switch(category){case AudioCategory::Music:return 2;case AudioCategory::Sfx:return 4;case AudioCategory::Voice:return 3;}throw std::invalid_argument("Unknown audio category");}
    void Set(AudioCategory category,int level)
    {
        auto& slider=sliders[Index(category)];AudioOptionsApplyVolume(level,slider.target,slider.remainingTime,slider.minimum,slider.maximum);
        settings[static_cast<unsigned>(category)]=level;
    }
    void Update(float delta)
    {AudioCalculationTable table{5,definitions.data(),sliders.data()};AudioUpdateCalculationTable(table,delta);}
};
AudioCategoryVolumes::AudioCategoryVolumes(resources::AudioVolumeProfile::Handle p,std::array<int,3> settings):impl_(std::make_unique<Implementation>())
{
    resources::Require(bool(p),"Audio settings require a checked category profile");for(int v:settings)resources::Require(v>=0&&v<=10,"Initial audio settings must be explicit0..10 values");
    auto& s=*impl_;s.profile=std::move(p);
    for(unsigned i=0;i<5;++i){s.definitions[i].field_0C=i?&s.global:nullptr;s.sliders[i].Initialize(&s.definitions[i]);s.sliders[i].SetTarget(0,0);}
    s.Update(0);SetAll(settings);s.Update(0);
}
AudioCategoryVolumes::~AudioCategoryVolumes(){if(impl_->thread!=std::this_thread::get_id())std::terminate();}
void AudioCategoryVolumes::Set(AudioCategory category,int level){impl_->Check();impl_->Set(category,level);}
void AudioCategoryVolumes::SetAll(std::array<int,3> settings)
{impl_->Check();for(unsigned i=0;i<3;++i)impl_->Set(static_cast<AudioCategory>(i),settings[i]);}
void AudioCategoryVolumes::Update(std::uint64_t frame,float delta)
{
    auto& s=*impl_;s.Check();resources::Require(frame>s.frame,"Audio category clock must advance once per increasing frame");
    resources::Require(std::isfinite(delta)&&delta>=0&&delta<=1,"Audio category delta must be finite0..1");s.Update(delta);s.frame=frame;
}
float AudioCategoryVolumes::Value(std::uint32_t slider)const
{impl_->Check();return impl_->sliders.at(slider).GetValue();}
AudioCategorySnapshot AudioCategoryVolumes::Snapshot()const
{
    impl_->Check();AudioCategorySnapshot out;out.settings=impl_->settings;out.frame=impl_->frame;
    for(unsigned i=0;i<3;++i){auto& v=impl_->sliders[Implementation::Index(static_cast<AudioCategory>(i))];out.targets[i]=v.target;out.values[i]=v.GetValue();}return out;
}
void AudioCategoryVolumes::RequireCompatible(resources::AudioCalculationInitial::Handle initial)const
{
    impl_->Check();resources::Require(initial&&initial->Size()==5,"Live audio categories require compatible initial calculation data");
    for(unsigned i=0;i<5;++i)resources::Require(std::bit_cast<std::uint32_t>(initial->Value(i))==std::bit_cast<std::uint32_t>(impl_->profile->Initial()->Value(i)),"Live audio category initial values differ");
}
resources::AudioCalculationInitial::Handle AudioCategoryVolumes::Initial()const{impl_->Check();return impl_->profile->Initial();}
}
