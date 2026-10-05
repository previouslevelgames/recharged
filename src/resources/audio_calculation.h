#pragma once
#include "resources/audio_catalog.h"

namespace mscharged::resources
{
// Immutable initial table after original target initialization and one Update(0)
// in stored order. This does not implement later slider/volume automation.
class AudioCalculationInitial
{
public:
    using Handle=std::shared_ptr<const AudioCalculationInitial>;
    float Value(std::uint32_t index)const{return values_.at(index);}
    std::size_t Size()const{return values_.size();}
private:
    AudioCalculationInitial()=default;
    std::vector<float> values_;
    friend Handle ReadAudioCalculationInitial(Bytes);
};
AudioCalculationInitial::Handle ReadAudioCalculationInitial(Bytes nlxgs);
}
