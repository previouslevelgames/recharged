#include "platform/ax_adpcm_samples.h"
#include <revolution/thp/THPAdpcmStep.h>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>

namespace mscharged::platform {
namespace {
std::uint16_t Half(const unsigned char* p) {return (std::uint16_t(p[0])<<8)|p[1];}
std::uint32_t Word(const unsigned char* p) {return (std::uint32_t(Half(p))<<16)|Half(p+2);}
std::int16_t SignedHalf(const unsigned char* p) {
    const auto bits=Half(p);std::int16_t value;std::memcpy(&value,&bits,sizeof(value));return value;
}
// Accelerator sample bytes for one decode block, fetched in windows under one
// DSP memory lock each. Every byte equals DSPBackendReadSampleByte's; a byte
// outside the window is read afresh, so unavailable bytes fail as before.
class SampleBytes {
public:
    explicit SampleBytes(NativeDSPMemoryEndpoint endpoint) : endpoint_(endpoint) {}
    unsigned char operator()(std::uint32_t address) {
        if (address-start_>=count_) {
            count_=DSPBackendReadSampleBytes(endpoint_,address,window_.data(),window_.size());
            start_=address;
        }
        return window_[address-start_];
    }
private:
    NativeDSPMemoryEndpoint endpoint_;
    std::array<unsigned char,64> window_{};
    std::uint32_t start_=0;
    std::size_t count_=0;
};
std::int16_t DecodeSample(unsigned char nibble,const NativeAXADPCMState& state) {
    const auto predictor=(state.predictor_scale>>4)&7;
    const auto scale=state.predictor_scale&15;
    const auto sample=nibble<8?std::int32_t(nibble):std::int32_t(nibble)-16;
    const auto& coefficient=state.coefficients[predictor];
    // The verified original THP integer DSP ADPCM equation is the same codec:
    // two owned signed coefficients/histories, signed4-bit residual, Q11 round
    // and signed16 saturation. Container parsing and accelerator addresses are
    // separate here; an SP sample is never treated as a THP container.
    const auto accumulator=THPAdpcmAccumulator(sample,scale,coefficient[0],coefficient[1],
                                               state.history1,state.history2);
    // Defined floor division is equivalent to the original arithmetic >>16,
    // including negative rounded values, without a signed-shift assumption.
    const auto pcm=accumulator>=0?accumulator/65536:-((-accumulator+65535)/65536);
    return static_cast<std::int16_t>(pcm);
}
std::string Hex(std::uint32_t value) {
    char text[11];std::snprintf(text,sizeof(text),"0x%08x",value);return text;
}
void Geometry(const NativeAXADPCMState& state) {
    if(state.current_nibble&0x40000000u || state.end_nibble&0xc0000000u || state.loop_nibble&0xc0000000u)
        throw std::invalid_argument("AX ADPCM nibble address uses an unqualified hardware address domain");
    if((state.end_nibble&15)<2)
        throw std::logic_error("AX ADPCM special header-nibble end-address behavior is not implemented");
    // Firmware0F61..0F71: the end exception of a voice whose loopFlag is not1
    // stops it and points AR2 at the zero cell. Its loop address (often a
    // silence buffer at a frame header) is never decoded; the accelerator's
    // wrapped address is only written back as the final current address.
    if((state.current_nibble&15)<2 || state.current_nibble>state.end_nibble ||
       (state.loop_flag && ((state.loop_nibble&15)<2 || state.loop_nibble>state.end_nibble)))
        throw std::logic_error("AX ADPCM raw decoding requires the original ordinary payload/end domain (current=" +
                               Hex(state.current_nibble) + " loop=" + Hex(state.loop_nibble) + " end=" +
                               Hex(state.end_nibble) + " loopFlag=" + std::to_string(state.loop_flag) + ")");
    if(state.loop_flag>1 || state.voice_type>1)
        throw std::logic_error("AX ADPCM loop/type descriptor is outside the qualified source domain");
}
}
NativeAXADPCMState ReadNativeAXADPCMState(NativeDSPMemoryEndpoint endpoint,std::uint32_t address) {
    std::array<unsigned char,320> wire{};DSPBackendReadMemory(endpoint,address,wire.data(),wire.size());
    if(Half(wire.data()+0x70)!=0)
        throw std::logic_error("AX sample format is not DSP ADPCM");
    const auto status=Half(wire.data()+0x10);
    if(status>1)throw std::logic_error("AX voice state is outside the qualified stop/run domain");
    NativeAXADPCMState result;
    result.src_select=Half(wire.data()+8);result.coefficient_select=Half(wire.data()+10);
    result.running=status!=0;result.voice_type=Half(wire.data()+0x12);
    result.loop_flag=Half(wire.data()+0x6e);result.loop_nibble=Word(wire.data()+0x72);
    result.end_nibble=Word(wire.data()+0x76);result.current_nibble=Word(wire.data()+0x7a);
    for(unsigned i=0;i<8;++i)for(unsigned j=0;j<2;++j)
        result.coefficients[i][j]=SignedHalf(wire.data() + 0x7e + (i*2+j)*2);
    result.gain=Half(wire.data()+0x9e);result.predictor_scale=Half(wire.data()+0xa0)&0x7f;
    result.history1=SignedHalf(wire.data()+0xa2);result.history2=SignedHalf(wire.data()+0xa4);
    result.ratio=Word(wire.data()+0xa6);result.fraction=Half(wire.data()+0xaa);
    for(unsigned i=0;i<4;++i)result.src_history[i]=SignedHalf(wire.data()+0xac+i*2);
    result.loop_predictor_scale=Half(wire.data()+0xb4)&0x7f;
    result.loop_history1=SignedHalf(wire.data()+0xb6);result.loop_history2=SignedHalf(wire.data()+0xb8);
    return result;
}
NativeAXRawADPCMBlock DecodeNativeAXRawADPCM(NativeDSPMemoryEndpoint endpoint,
                                           const NativeAXADPCMState& input,std::size_t limit) {
    if(!limit || limit>96)throw std::invalid_argument("AX raw ADPCM block must request 1..96 samples");
    NativeAXRawADPCMBlock result;result.next=input;
    auto& state=result.next;
    if(!state.running)return result;
    Geometry(state);state.predictor_scale&=0x7f;state.loop_predictor_scale&=0x7f;
    const auto initial=state;
    SampleBytes Byte(endpoint);
    try {
    while(result.samples_decoded<limit && state.running) {
        const auto at=state.current_nibble;
        const auto packed=Byte(at/2);
        const auto nibble=static_cast<unsigned char>(at&1?packed&15:packed>>4);
        const auto decoded=DecodeSample(nibble,state);
        state.history2=state.history1;state.history1=decoded;
        result.samples[result.samples_decoded++]=decoded;
        const bool ended=at==state.end_nibble;
        ++state.current_nibble;
        // Header prefetch is a real accelerator boundary, including a payload
        // end at nibble15: the next frame's header is read before the end
        // check, past the allocation if the sample ends there. A looping voice
        // then takes its loop pred/scale; a one-shot voice writes it back.
        if((state.current_nibble&15)==0) {
            state.predictor_scale=Byte(state.current_nibble/2)&0x7f;
            state.current_nibble+=2;
        }
        if(ended) {
            result.end_reached=true;state.current_nibble=state.loop_nibble;
            if(state.loop_flag) {
                state.predictor_scale=state.loop_predictor_scale;
                if(state.voice_type!=1) {
                    state.history1=state.loop_history1;state.history2=state.loop_history2;
                }
                ++result.loops;
            } else state.running=false;
        }
    }
    } catch(const std::out_of_range& error) {
        throw std::out_of_range(std::string(error.what())+" during ADPCM decode at nibble "+Hex(state.current_nibble)+
                                " (voice current="+Hex(initial.current_nibble)+" loop="+Hex(initial.loop_nibble)+
                                " end="+Hex(initial.end_nibble)+" loopFlag="+std::to_string(initial.loop_flag)+
                                " type="+std::to_string(initial.voice_type)+")");
    }
    return result;
}
void RequireNativeAXADPCMFrameProcessing(const NativeAXADPCMState& state) {
    if(state.src_select==0)
        throw std::logic_error("AX source-selected four-tap SRC needs a qualified original DSP coefficient bank; ratio 1 is not direct mode");
    if(state.src_select==1)
        throw std::logic_error("AX source-selected linear SRC/frame history is not implemented");
    if(state.src_select==2)
        throw std::logic_error("AX direct sample frame still needs original voice/envelope/filter/mix/remote processing");
    throw std::logic_error("AX source sample-rate mode is unknown");
}
} // namespace mscharged::platform
