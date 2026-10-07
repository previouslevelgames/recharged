#include "platform/ax_frame_commands.h"
#include <algorithm>
#include <cstring>
#include <limits>

namespace mscharged::platform {
namespace {
std::uint16_t Half(const unsigned char* p) {return (std::uint16_t(p[0])<<8)|p[1];}
std::uint32_t Word(const unsigned char* p) {return (std::uint32_t(Half(p))<<16)|Half(p+2);}
std::uint32_t Address(const NativeAXCommand& c,unsigned i=0) {
    return (std::uint32_t(c.arguments[i])<<16)|c.arguments[i+1];
}
void PutHalf(unsigned char* p,std::uint16_t x) {p[0]=x>>8;p[1]=x;}
void PutWord(unsigned char* p,std::uint32_t x) {PutHalf(p,x>>16);PutHalf(p+2,x);}
std::int16_t SignedHalf(std::uint16_t x) {std::int16_t s;std::memcpy(&s,&x,2);return s;}
std::int32_t SignedWord(std::uint32_t x) {std::int32_t s;std::memcpy(&s,&x,4);return s;}
std::int64_t SignedWidth(std::int64_t x,unsigned width) {
    const auto mask=(std::uint64_t(1)<<width)-1;
    const auto bits=std::uint64_t(x)&mask;
    return bits&(std::uint64_t(1)<<(width-1)) ? std::int64_t(bits)-std::int64_t(mask+1) : std::int64_t(bits);
}
std::int64_t Floor(std::int64_t x,std::int64_t d) {return x>=0?x/d:-((-x+d-1)/d);}
std::int16_t Saturate(std::int64_t x) {return std::int16_t(std::max<std::int64_t>(-32768,std::min<std::int64_t>(32767,x)));}
std::int32_t StoreBus(std::int64_t x) {
    // Literal40-bit mode stores saturated AC.M and the unchanged AC.L, not a
    // generic s32 clamp. Preserve this source hardware overflow behavior.
    const auto value=SignedWidth(x,40);
    const auto high=std::uint16_t(Saturate(Floor(value,65536)));
    return SignedWord((std::uint32_t(high)<<16)|(std::uint64_t(value)&65535));
}
[[noreturn]] void Fail(NativeAXFailure why,const NativeAXCommand& c,const char* message) {
    throw NativeAXCommandError(why,c.word_offset,message);
}
std::vector<unsigned char> Read(NativeDSPMemoryEndpoint e,std::uint32_t a,std::size_t bytes) {
    std::vector<unsigned char> data(bytes);DSPBackendReadMemory(e,a,data.data(),bytes);return data;
}
bool Overlap(std::uint32_t a,std::size_t an,std::uint32_t b,std::size_t bn) {
    return std::uint64_t(a)<std::uint64_t(b)+bn && std::uint64_t(b)<std::uint64_t(a)+an;
}
std::vector<unsigned char> Capture(NativeAXPreparedCommandFrame& f,NativeDSPMemoryEndpoint e,
                                  std::uint32_t a,std::size_t bytes) {
    auto wire=Read(e,a,bytes);f.reads.push_back({a,wire,{}});return wire;
}
void Stage(NativeAXPreparedCommandFrame& f,NativeDSPMemoryEndpoint e,std::uint32_t a,
           std::vector<unsigned char> after) {
    DSPBackendValidateMemory(e,a,after.size(),true);
    const auto end=std::uint64_t(a)+after.size();
    for(const auto& w:f.writes)
        if(std::uint64_t(a)<std::uint64_t(w.address)+w.after.size() && w.address<end)
            throw std::runtime_error("AX original output/AUX writable spans overlap");
    f.written_bytes+=after.size();f.writes.push_back({a,Read(e,a,after.size()),std::move(after)});
}
bool IsZero(const std::vector<unsigned char>& bytes) {
    return std::all_of(bytes.begin(),bytes.end(),[](unsigned char x){return x==0;});
}
}
bool operator==(const NativeAXCommandHistory& a,const NativeAXCommandHistory& b) noexcept {
    return a.master==b.master && a.aux_a==b.aux_a && a.aux_b==b.aux_b && a.aux_c==b.aux_c &&
           a.compressor_counter==b.compressor_counter && a.compressor_counter_known==b.compressor_counter_known;
}
std::array<std::uint16_t,96> NativeAXCommandGainRamp(std::uint16_t before,std::uint16_t requested) {
    const auto delta=SignedHalf(std::uint16_t(requested-before));
    std::int64_t accumulator=std::int64_t(before)*65536;
    std::array<std::uint16_t,96> result{};
    constexpr int weight[3]={683,683,682};
    for(unsigned i=0;i<95;++i) {
        accumulator+=std::int64_t(delta)*weight[i%3];
        result[i]=std::uint16_t(Floor(accumulator,65536));
    }
    result.back()=requested;return result;
}
NativeAXChannel96 NativeAXMixAuxReturn(const NativeAXChannel96& main,
                                      const NativeAXChannel96& returned,
                                      const std::array<std::uint16_t,96>& gain) {
    NativeAXChannel96 result{};
    for(unsigned i=0;i<96;++i)
        result[i]=StoreBus(std::int64_t(main[i])+Floor(std::int64_t(returned[i])*gain[i],32768));
    return result;
}
NativeAXStereo96 NativeAXPackStereo(const NativeAXChannel96& left,const NativeAXChannel96& right,
                                    const std::array<std::uint16_t,96>& gain) {
    NativeAXStereo96 result{};
    for(unsigned i=0;i<96;++i) {
        // Actual0631..0655 interleaves right,left. LSL16 wraps the40-bit
        // accumulator before its saturated middle store, including wide edges.
        result[i*2]=Saturate(SignedWidth(Floor(std::int64_t(right[i])*gain[i],32768),24));
        result[i*2+1]=Saturate(SignedWidth(Floor(std::int64_t(left[i])*gain[i],32768),24));
    }
    return result;
}
NativeAXPreparedCommandFrame PrepareNativeAXCommandFrame(
    NativeDSPMemoryEndpoint memory,std::uint32_t address,std::size_t bytes,
    const NativeAXCommandHistory& history,const NativeAXSuppliedCoefficientROM& coefficients) {
    if(!bytes || bytes>128 || bytes%2 || address%2)
        throw NativeAXCommandError(NativeAXFailure::InvalidListExtent,0,"AX staged list must be an aligned original128-byte slot");
    NativeAXPreparedCommandFrame f;f.before=f.after=history;
    const auto list_before=Capture(f,memory,address,bytes);
    const auto list=ReadNativeAXCommandList(memory,address,bytes);
    if(Read(memory,address,bytes)!=list_before)
        throw std::runtime_error("AX source command list changed while parsing");
    f.consumed_words=list.consumed_words;
    std::size_t next=0;
    auto Take=[&](NativeAXOpcode op)->const NativeAXCommand& {
        const auto& c=list.commands[next<list.command_count?next:list.command_count-1];
        if(next>=list.command_count || c.opcode!=op)
            Fail(NativeAXFailure::UnsupportedSequence,c,"AX staged frame requires original normal-output command order");
        ++next;return c;
    };
    const auto& setup=Take(NativeAXOpcode::Setup);
    if(!IsZero(Capture(f,memory,Address(setup),120)))
        Fail(NativeAXFailure::NonzeroStudio,setup,"AX Studio depop history processing is not yet qualified");
    const auto& surround=list.commands[next<list.command_count?next:list.command_count-1];
    if(next>=list.command_count || (surround.opcode!=NativeAXOpcode::AddToLR && surround.opcode!=NativeAXOpcode::SubToLR))
        Fail(NativeAXFailure::UnsupportedSequence,surround,"AX DPL2 routing/output is not yet qualified");
    ++next;
    if(!IsZero(Capture(f,memory,Address(surround),384)))
        Fail(NativeAXFailure::NonzeroSurround,surround,"AX prior surround routing is not yet qualified");
    const auto& process=Take(NativeAXOpcode::Process);
    std::array<NativeAXChannel96,12> buses{};
    std::array<std::uint32_t,96> seen{};auto voice_address=Address(process);
    while(voice_address) {
        if(f.voice_count==96 || std::find(seen.begin(),seen.begin()+f.voice_count,voice_address)!=seen.begin()+f.voice_count)
            Fail(NativeAXFailure::InvalidVoiceChain,process,"AX source PB chain is cyclic or exceeds96 voices");
        for(unsigned i=0;i<f.voice_count;++i)
            if(Overlap(seen[i],320,voice_address,320))
                Fail(NativeAXFailure::InvalidVoiceChain,process,"AX distinct source PB records partially overlap");
        const auto voice=PrepareNativeAXADPCMVoiceFrame(memory,voice_address,coefficients);
        seen[f.voice_count++]=voice_address;
        if(voice.was_running) {
            ++f.active_voices;
            for(unsigned c=0;c<12;++c)for(unsigned i=0;i<96;++i)
                buses[c][i]=StoreBus(std::int64_t(buses[c][i])+voice.buses[c][i]);
            f.written_bytes+=voice.parameters_after.size();
        }
        voice_address=Word(voice.parameters_before.data());f.voices.push_back(voice);
    }
    if(f.voice_count!=96)
        Fail(NativeAXFailure::InvalidVoiceChain,process,"AX staged frame requires the complete original96-PB owner");
    constexpr unsigned aux_bus[3][3]={{2,3,9},{4,5,10},{6,7,11}};
    std::array<std::uint16_t*,3> previous{{&f.after.aux_a,&f.after.aux_b,&f.after.aux_c}};
    for(unsigned aux=0;aux<3;++aux) {
        const auto opcode=static_cast<NativeAXOpcode>(unsigned(NativeAXOpcode::MixAuxA)+aux);
        if(next>=list.command_count || list.commands[next].opcode!=opcode)continue;
        const auto& c=Take(opcode);const auto gain=NativeAXCommandGainRamp(*previous[aux],c.arguments[0]);
        *previous[aux]=c.arguments[0];++f.aux_commands;
        const auto returned=Capture(f,memory,Address(c,3),1152);
        std::vector<unsigned char> upload(1152);
        for(unsigned channel=0;channel<3;++channel) {
            NativeAXChannel96 incoming{};
            for(unsigned i=0;i<96;++i) {
                PutWord(upload.data()+(channel*96+i)*4,std::uint32_t(buses[aux_bus[aux][channel]][i]));
                incoming[i]=SignedWord(Word(returned.data()+(channel*96+i)*4));
            }
            const unsigned main=channel<2?channel:8;
            buses[main]=NativeAXMixAuxReturn(buses[main],incoming,gain);
        }
        Stage(f,memory,Address(c,1),std::move(upload));
    }
    if(next<list.command_count && list.commands[next].opcode==NativeAXOpcode::Compressor) {
        const auto& c=Take(NativeAXOpcode::Compressor);
        if(c.arguments[0]!=32768 || c.arguments[1]!=10 || !history.compressor_counter_known || history.compressor_counter)
            Fail(NativeAXFailure::UnsupportedCompressor,c,"AX compressor history/parameters are outside fresh no-attack proof");
        for(unsigned channel=0;channel<2;++channel)for(auto sample:buses[channel])
            if(sample<=-32768 || sample>=32768)
                Fail(NativeAXFailure::UnsupportedCompressor,c,"AX compressor attack/attenuation is not yet qualified");
        // Owned057B..059F skips all coefficient DMA and compression when no
        // sample reaches the threshold and the actual release counter is zero.
        DSPBackendValidateMemory(memory,Address(c,2),2,false);
    }
    const auto& remote=Take(NativeAXOpcode::RemoteOutput);
    const auto& output=Take(NativeAXOpcode::Output);
    if(Address(output,1)!=Address(surround))
        Fail(NativeAXFailure::UnsupportedSequence,output,"AX surround output owner differs from source routing input");
    Take(NativeAXOpcode::End);
    if(next!=list.command_count)
        Fail(NativeAXFailure::UnsupportedSequence,list.commands[next],"AX staged frame has commands after END");
    for(unsigned channel=0;channel<4;++channel)Stage(f,memory,Address(remote,channel*2),std::vector<unsigned char>(36));
    std::vector<unsigned char> surround_wire(384);
    for(unsigned i=0;i<96;++i)PutWord(surround_wire.data()+i*4,std::uint32_t(buses[8][i]));
    Stage(f,memory,Address(output,1),std::move(surround_wire));
    const auto gain=NativeAXCommandGainRamp(history.master,output.arguments[0]);f.after.master=output.arguments[0];
    const auto packed=NativeAXPackStereo(buses[0],buses[1],gain);
    std::vector<unsigned char> pcm(384);
    for(unsigned i=0;i<192;++i)PutHalf(pcm.data()+i*2,std::uint16_t(packed[i]));
    Stage(f,memory,Address(output,3),std::move(pcm));
    ValidateNativeAXCommandCommit(memory,f,history);return f;
}
void ValidateNativeAXCommandCommit(NativeDSPMemoryEndpoint memory,const NativeAXPreparedCommandFrame& f,
                                  const NativeAXCommandHistory& history) {
    if(!(history==f.before))throw std::runtime_error("AX device history changed before whole-frame commit");
    // Typed memory transport permits interior byte transfers. It does not
    // certify320-byte PB starts, so prove all participants disjoint explicitly
    // before validation/commit of any individual PB or output span.
    for(std::size_t i=0;i<f.voices.size();++i) {
        const auto& voice=f.voices[i];
        for(std::size_t j=0;j<i;++j)
            if(Overlap(voice.parameter_address,320,f.voices[j].parameter_address,320))
                throw std::runtime_error("AX source PB records overlap before whole-frame commit");
        for(const auto& r:f.reads)
            if(Overlap(voice.parameter_address,320,r.address,r.before.size()))
                throw std::runtime_error("AX source PB aliases a command/contributor before whole-frame commit");
    }
    for(const auto& r:f.reads)
        if(Read(memory,r.address,r.before.size())!=r.before)
            throw std::runtime_error("AX source command/contributor changed before whole-frame commit");
    for(const auto& voice:f.voices) {
        if(voice.was_running)ValidateNativeAXVoiceCommit(memory,voice);
        else if(Read(memory,voice.parameter_address,voice.parameters_before.size())!=std::vector<unsigned char>(voice.parameters_before.begin(),voice.parameters_before.end()))
            throw std::runtime_error("AX stopped source PB changed before whole-frame commit");
    }
    for(const auto& w:f.writes) {
        DSPBackendValidateMemory(memory,w.address,w.after.size(),true);
        if(Read(memory,w.address,w.before.size())!=w.before)
            throw std::runtime_error("AX source output/AUX owner changed before whole-frame commit");
        for(std::size_t i=0;i<f.reads.size();++i) {
            const auto& r=f.reads[i];const auto end=std::uint64_t(w.address)+w.after.size();
            if(std::uint64_t(w.address)<std::uint64_t(r.address)+r.before.size() && r.address<end) {
                // The original prior-surround operand is the same source
                // in/out span. Original CL/Studio/AUX-return owners are not.
                if(i!=2 || w.address!=r.address || w.after.size()!=r.before.size())
                    throw std::runtime_error("AX output aliases an original readonly command contributor");
            }
        }
        for(const auto& voice:f.voices) {
            const auto end=std::uint64_t(w.address)+w.after.size();
            if(std::uint64_t(w.address)<std::uint64_t(voice.parameter_address)+320 && voice.parameter_address<end)
                throw std::runtime_error("AX output aliases its original parameter-block owner");
        }
    }
}
void CommitNativeAXCommandFrame(NativeDSPMemoryEndpoint memory,const NativeAXPreparedCommandFrame& f,
                               NativeAXCommandHistory& history) {
    ValidateNativeAXCommandCommit(memory,f,history);
    for(const auto& voice:f.voices)if(voice.was_running)CommitNativeAXVoiceFrame(memory,voice);
    for(const auto& w:f.writes)DSPBackendWriteMemory(memory,w.address,w.after.data(),w.after.size());
    history=f.after;
}
} // namespace mscharged::platform
