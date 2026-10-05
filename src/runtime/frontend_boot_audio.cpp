#include "runtime/frontend_boot_audio.h"
#include <thread>

namespace mscharged
{
struct FrontendBootAudio::Implementation
{
    LoadedAudioBank::Handle loaded;
    resources::AudioCalculationInitial::Handle calculation;
    std::unique_ptr<AudioBankSelection> selection;
    std::unique_ptr<ResidentAudioOutput> output;
    unsigned* seed;
    std::uint32_t device,cue;
    std::optional<std::uint32_t> sample;
    FrontendBootAudioState state=FrontendBootAudioState::Loaded;
    std::thread::id thread=std::this_thread::get_id();
    Implementation(unsigned& rng,std::uint32_t id):seed(&rng),device(id){}
    void Thread()const
    {if(thread!=std::this_thread::get_id())throw std::logic_error("Boot audio requires its creating thread");}
    // Members normally destruct in reverse order, placing output before all
    // retained bank/source metadata and before the caller may shut SDL down.
};
FrontendBootAudio::FrontendBootAudio(LoadedAudioBank::Handle loaded,
    resources::AudioCalculationInitial::Handle calculation,unsigned& seed,std::uint32_t device)
    :impl_(std::make_unique<Implementation>(seed,device))
{
    resources::Require(bool(loaded)&&bool(loaded->bank)&&bool(calculation),"Boot audio requires checked retained assets");
    resources::Require(loaded->name_index==25&&loaded->slot_index==23&&
        loaded->name.name=="FE_GEN_Splash"&&!loaded->slot.streaming,
        "Boot audio requires resident FE_GEN_Splash name25/slot23");
    const auto cue=loaded->bank->CuesByKey().Find({0xde83984e,0,0,0});
    resources::Require(cue!=0xffff,"Boot logo cue is missing");
    auto selection=std::make_unique<AudioBankSelection>(loaded->bank);
    impl_->cue=cue;impl_->loaded=std::move(loaded);impl_->calculation=std::move(calculation);impl_->selection=std::move(selection);
}
FrontendBootAudio::~FrontendBootAudio()=default;
void FrontendBootAudio::PlayLogo()
{
    auto& p=*impl_;p.Thread();
    if(p.state!=FrontendBootAudioState::Loaded||p.output||!p.loaded)
        throw std::logic_error("Boot logo cannot overlap, restart or play after bank unload");
    auto next_selection=std::make_unique<AudioBankSelection>(*p.selection);
    auto next_seed=*p.seed;
    auto selected=next_selection->Select({0xde83984e,0,0,0},next_seed);
    resources::Require(bool(selected),"Boot logo selection failed");
    auto buffer=PrepareResidentAudio(*selected,p.calculation);
    const auto sample=buffer->Sample();
    auto next_output=std::make_unique<ResidentAudioOutput>(std::move(buffer),p.device);
    next_output->Start();
    // After admission: swaps/scalar writes and noexcept destructors only.
    p.selection.swap(next_selection);p.output.swap(next_output);*p.seed=next_seed;
    p.sample=sample;p.state=FrontendBootAudioState::Playing;
}
void FrontendBootAudio::Unload(std::uint32_t slot)
{
    auto& p=*impl_;p.Thread();
    resources::Require(slot==23,"Boot audio cannot unload an unrelated slot");
    std::exception_ptr error;
    try{if(p.output)p.output->Stop();}catch(...){error=std::current_exception();}
    p.output.reset();p.selection.reset();p.loaded.reset();p.calculation.reset();
    p.state=error?FrontendBootAudioState::Failed:FrontendBootAudioState::Unloaded;
    if(error)std::rethrow_exception(error);
}
FrontendBootAudioStatus FrontendBootAudio::Status()
{
    auto& p=*impl_;p.Thread();
    if(!p.output)return {p.state,p.sample};
    try
    {
        const auto status=p.output->Status();
        if(status.state==ResidentAudioState::InputConsumed)p.state=FrontendBootAudioState::InputConsumed;
        else if(status.state==ResidentAudioState::Stopped)p.state=FrontendBootAudioState::Failed;
        return {p.state,p.sample,status.queued_input_bytes,status.available_output_bytes};
    }
    catch(...){p.state=FrontendBootAudioState::Failed;throw;}
}
AudioCueSelectionState FrontendBootAudio::SelectionState()const
{
    auto& p=*impl_;p.Thread();if(!p.selection)throw std::logic_error("Boot audio selection was unloaded");
    return p.selection->State(p.cue);
}
}
