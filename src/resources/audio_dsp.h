#pragma once
#include "resources/audio_bank.h"

namespace mscharged::resources
{
class AudioPcmSample
{
public:
    using Handle=std::shared_ptr<const AudioPcmSample>;
    std::uint32_t Rate()const{return rate_;}
    std::uint32_t SourceSample()const{return source_;}
    // Mono signed PCM16 at its original source rate, independent of bank storage.
    std::span<const std::int16_t> Samples()const{return samples_;}
private:
    AudioPcmSample()=default;
    std::uint32_t rate_=0,source_=0;
    std::vector<std::int16_t> samples_;
    friend Handle DecodeAudioDsp(AudioResidentBank::Handle,std::uint32_t,std::size_t);
};
// Start with SP predictor/history at current_nibble (including midframe starts).
// Later frame boundaries read their own predictor byte. End nibble is inclusive.
// No loops, resampling, mixer/effects, voice allocation or output are executed.
AudioPcmSample::Handle DecodeAudioDsp(AudioResidentBank::Handle,std::uint32_t sample,
    std::size_t maximum_samples=16*1024*1024);
}
