#include "runtime/audio_bank_selection.h"
#include "Game/Audio/AudioBankSelectionSteps.h"
#include "NL/nlMath.h"
#include <limits>

namespace mscharged
{
using resources::Require;
namespace
{
struct Voice { std::uint32_t index; };
struct Entry { Voice* voice; float minimumValue,weight; std::uint32_t selectionCount; std::uint8_t eligible=0; };
struct Cue { std::uint32_t voiceCount;Entry* voices;int selectionMode;std::uint32_t selectedVoiceIndex; };
struct Definition
{
    std::uint32_t choiceCount;
    const resources::AudioBankChoice* choices;
    float delayMinimum,delayRange;
};
}
AudioBankSelection::AudioBankSelection(resources::AudioResidentBank::Handle bank):bank_(std::move(bank))
{Require(bool(bank_),"Audio selection requires a retained checked bank");Reset();}
void AudioBankSelection::Thread()const
{if(thread_!=std::this_thread::get_id())throw std::logic_error("Audio selection requires its owning thread");}
void AudioBankSelection::Reset()
{
    Thread();std::vector<AudioCueSelectionState> next;
    for(const auto& cue:bank_->Cues())next.push_back({0xffff,std::vector<std::uint32_t>(cue.voices.size())});states_=std::move(next);
}
AudioCueSelectionState AudioBankSelection::State(std::uint32_t cue)const{Thread();return states_.at(cue);}
std::optional<AudioBankSelectionResult> AudioBankSelection::Select(const resources::AudioCueKey& key,unsigned int& seed)
{
    Thread();const auto index=bank_->CuesByKey().Find(key);if(index==0xffff)return {};
    const auto& definition=bank_->Cues().at(index);auto next=states_.at(index);auto next_seed=seed;
    // Own all transient pointer targets. No packed Wii address enters a native pointer.
    std::vector<Voice> voices;voices.reserve(definition.voices.size());for(const auto& v:definition.voices)voices.push_back({v.voice});
    std::vector<Entry> entries;entries.reserve(voices.size());
    for(unsigned i=0;i<voices.size();++i)
    {Require(next.counts[i]!=UINT32_MAX,"Audio cue selection counter exhausted");entries.push_back({&voices[i],definition.voices[i].minimum,definition.voices[i].weight,next.counts[i]});}
    Cue cue{std::uint32_t(entries.size()),entries.data(),definition.selection_mode,next.selected};
    auto* selected=AudioSelectCueVoiceSteps<Voice,Entry>(&cue,[&](unsigned range){return nlRandom(range,&next_seed);});
    Require(selected!=nullptr,"Original audio selection produced no voice");
    AudioBankSelectionResult result{bank_,index,selected->index,{}};
    const auto& voice=bank_->Voices().at(result.voice);
    // SoundInstance constructs sequence array order; AudioSequenceInstance
    // constructs event array order. Delay consumes RNG before source choice.
    for(auto sequence:voice.sequences)for(auto event:bank_->Sequences().at(sequence).events)
    {
        Require(result.events.size()<4096,"Audio selection event budget exceeded");
        const auto& sound=bank_->Sounds().at(event);
        Definition d{std::uint32_t(sound.choices.size()),sound.choices.data(),sound.delay_min,sound.delay_range};
        const auto time=AudioEventStartTimeSteps(&d,[&](float low,float high){return nlRandomf(low,high,&next_seed);});
        const auto source=AudioSelectSourceSteps(&d,[&](unsigned range){return nlRandom(range,&next_seed);});
        const auto sample=bank_->Sources().at(source).fields[0];
        result.events.push_back({sequence,event,source,sample,time});
    }
    next.selected=cue.selectedVoiceIndex;for(unsigned i=0;i<entries.size();++i)next.counts[i]=entries[i].selectionCount;
    states_[index]=std::move(next);seed=next_seed;return result;
}
}
