#pragma once
#include "resources/audio_catalog.h"
#include <optional>

namespace mscharged::resources
{
struct AudioBankCueEntry { std::uint32_t voice; float minimum,weight; };
struct AudioBankCue
{
    std::uint32_t hash,maximum_count;
    int selection_mode;
    std::vector<AudioBankCueEntry> voices;
};
struct AudioBankVoice
{
    std::uint32_t hash,slider;
    float volume,pitch;
    std::vector<std::uint32_t> sequences;
};
struct AudioBankSequence { float volume; std::vector<std::uint32_t> events; };
struct AudioBankChoice { std::uint32_t index,weight; };
struct AudioBankSound
{
    std::uint32_t sound_id;
    bool random_pitch,random_volume;
    float pitch_min,pitch_max,volume_min,volume_max,delay_min,delay_range;
    std::vector<AudioBankChoice> choices;
};
struct AudioBankSource { std::array<std::uint32_t,6> fields; };
struct AudioDspSample
{
    std::uint32_t rate,current_nibble,end_nibble;
    std::array<std::int16_t,16> coefficients;
    std::uint16_t gain,predictor_scale,loop_predictor_scale;
    std::int16_t history1,history2,loop_history1,loop_history2;
    std::size_t first_byte,byte_count;
    std::uint32_t sample_count;
};
class AudioResidentBank
{
public:
    using Handle=std::shared_ptr<const AudioResidentBank>;
    const AudioCueCatalog& CuesByKey()const{return *map_;}
    const auto& Cues()const{return cues_;}
    const auto& Voices()const{return voices_;}
    const auto& Sequences()const{return sequences_;}
    const auto& Sounds()const{return sounds_;}
    const auto& Sources()const{return sources_;}
    const auto& Samples()const{return samples_;}
    // Borrowed encoded bytes; retain this bank handle for the complete span lifetime.
    Bytes SampleBytes(std::uint32_t sample)const;
    std::size_t WaveBytes()const{return wave_.size();}
private:
    AudioResidentBank()=default;
    AudioCueCatalog::Handle map_;
    std::vector<AudioBankCue> cues_;
    std::vector<AudioBankVoice> voices_;
    std::vector<AudioBankSequence> sequences_;
    std::vector<AudioBankSound> sounds_;
    std::vector<AudioBankSource> sources_;
    std::vector<AudioDspSample> samples_;
    std::vector<std::uint8_t> wave_;
    friend Handle ReadAudioResidentBank(Bytes,Bytes);
};
// Checked selected graph: no slider/RPC/hit-marker/parameter/loop/stream services.
// Copies sample bytes and publishes only after the entire closure validates.
AudioResidentBank::Handle ReadAudioResidentBank(Bytes resbun,Bytes nlxwb);
}
