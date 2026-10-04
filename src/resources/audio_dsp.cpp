#include "resources/audio_dsp.h"
#include "revolution/thp/THPAdpcmStep.h"

namespace mscharged::resources
{
AudioPcmSample::Handle DecodeAudioDsp(AudioResidentBank::Handle bank,std::uint32_t index,std::size_t maximum)
{
    Require(bool(bank),"DSP decode requires a retained checked bank");
    const auto& sample=bank->Samples().at(index);const auto bytes=bank->SampleBytes(index);
    Require(maximum&&maximum<=16*1024*1024&&sample.sample_count<=maximum,"DSP PCM sample budget exceeded");
    if(sample.gain!=0)throw UnsupportedResource("Nonzero SP ADPCM gain remains unqualified");
    Require(sample.predictor_scale<=0x7f,"SP initial predictor/scale is outside the selected profile");
    auto out=std::shared_ptr<AudioPcmSample>(new AudioPcmSample);
    out->rate_=sample.rate;out->source_=index;out->samples_.resize(sample.sample_count);
    std::int16_t history1=sample.history1,history2=sample.history2;
    unsigned predictor=sample.predictor_scale>>4,scale=sample.predictor_scale&15;
    std::uint32_t nibble=sample.current_nibble;
    for(auto& value:out->samples_)
    {
        if(nibble%16==0)
        {
            Require(nibble<=sample.end_nibble&&nibble/2>=sample.first_byte,"DSP frame header exceeds the retained sample");
            const auto at=nibble/2-sample.first_byte;Require(at<bytes.size(),"DSP frame header is absent");
            // Original THP/DSP frame header ignores bit7, masks three predictor bits.
            predictor=(bytes[at]>>4)&7;scale=bytes[at]&15;nibble+=2;
        }
        Require(nibble<=sample.end_nibble&&nibble/2>=sample.first_byte,"DSP data nibble exceeds its inclusive range");
        const auto at=nibble/2-sample.first_byte;Require(at<bytes.size(),"DSP data byte is absent");
        const auto encoded=(nibble&1)?bytes[at]&15:bytes[at]>>4;
        const int signed_nibble=encoded>=8?int(encoded)-16:int(encoded);
        const auto rounded=THPAdpcmAccumulator(signed_nibble,scale,sample.coefficients[predictor*2],
            sample.coefficients[predictor*2+1],history1,history2);
        value=static_cast<std::int16_t>(rounded>>16);
        history2=history1;history1=value;++nibble;
    }
    Require(nibble==sample.end_nibble+1,"DSP sample count disagrees with inclusive nibble range");
    return out;
}
}
