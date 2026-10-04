#pragma once
#include "resources/audio_bank.h"
#include <thread>

namespace mscharged
{
struct AudioSelectedEvent
{
    std::uint32_t sequence,event,source,sample;
    float start_time;
};
struct AudioBankSelectionResult
{
    resources::AudioResidentBank::Handle bank;
    std::uint32_t cue,voice;
    std::vector<AudioSelectedEvent> events; // Original sequence/event construction order.
};
struct AudioCueSelectionState
{
    std::uint32_t selected=0xffff;
    std::vector<std::uint32_t> counts;
};
// Executes only original cue/event-construction selection, not playback or
// active voice counts. Explicit shared seed may be nlDefaultSeed once genuine
// boot ownership is wired. Both seed and selection state commit on success.
class AudioBankSelection
{
public:
    explicit AudioBankSelection(resources::AudioResidentBank::Handle);
    std::optional<AudioBankSelectionResult> Select(const resources::AudioCueKey&,unsigned int& seed);
    AudioCueSelectionState State(std::uint32_t cue)const;
    void Reset();
private:
    void Thread()const;
    resources::AudioResidentBank::Handle bank_;
    std::vector<AudioCueSelectionState> states_;
    std::thread::id thread_=std::this_thread::get_id();
};
}
