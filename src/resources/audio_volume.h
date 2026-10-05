#pragma once
#include "resources/audio_calculation.h"
#include <array>
namespace mscharged::resources
{
// Bounded original five-category calculation profile. All serialized references
// are checked; this is not an AudioSystem/global registry instance.
class AudioVolumeProfile
{
public:
    using Handle=std::shared_ptr<const AudioVolumeProfile>;
    const auto& Initial()const{return initial_;}
private:
    AudioVolumeProfile()=default;
    AudioCalculationInitial::Handle initial_;
    friend Handle ReadAudioVolumeProfile(Bytes);
};
AudioVolumeProfile::Handle ReadAudioVolumeProfile(Bytes nlxgs);
}
