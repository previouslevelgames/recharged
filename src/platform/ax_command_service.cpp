#include "platform/ax_command_service.h"
#include <algorithm>
#include <array>
#include <limits>

namespace mscharged::platform {
NativeAXCommandError::NativeAXCommandError(NativeAXFailure failure,std::size_t word,
                                         const char* message)
    : std::runtime_error(message),failure_(failure),word_(word) {}
namespace {
constexpr std::size_t FrameSamples=96, RemoteSamples=18, PBBytes=320, MaxVoices=96;
constexpr std::array<std::uint16_t,15> CommandWords{3,3,3,3,3,6,6,6,14,14,5,6,6,9,1};
std::uint16_t BE16(const unsigned char* p) {
    return (std::uint16_t(p[0])<<8)|std::uint16_t(p[1]);
}
std::uint32_t BE32(const unsigned char* p) {
    return (std::uint32_t(BE16(p))<<16)|BE16(p+2);
}
std::uint32_t Address(const NativeAXCommand& cmd,std::size_t argument=0) {
    return (std::uint32_t(cmd.arguments[argument])<<16)|cmd.arguments[argument+1];
}
[[noreturn]] void Fail(NativeAXFailure failure,const NativeAXCommand& cmd,
                      const char* message) {
    throw NativeAXCommandError(failure,cmd.word_offset,message);
}
void RequireZero(NativeDSPMemoryEndpoint endpoint,std::uint32_t address,
                 std::size_t bytes,NativeAXFailure failure,const NativeAXCommand& cmd,
                 const char* message) {
    std::array<unsigned char,768> wire{};
    if (bytes>wire.size()) throw std::logic_error("AX zero contributor extent is invalid");
    DSPBackendReadMemory(endpoint,address,wire.data(),bytes);
    if (std::any_of(wire.begin(),wire.begin()+bytes,[](unsigned char x){return x!=0;}))
        Fail(failure,cmd,message);
}
std::uint16_t CheckStoppedVoices(NativeDSPMemoryEndpoint endpoint,std::uint32_t address,
                                const NativeAXCommand& cmd) {
    std::array<std::uint32_t,MaxVoices> visited{};
    std::uint16_t voices=0;
    while(address) {
        if(voices==MaxVoices || std::find(visited.begin(),visited.begin()+voices,address)!=visited.begin()+voices)
            Fail(NativeAXFailure::InvalidVoiceChain,cmd,"AX parameter-block chain is cyclic or exceeds original96 voices");
        std::array<unsigned char,PBBytes> wire{};
        DSPBackendReadMemory(endpoint,address,wire.data(),wire.size());
        if(BE32(wire.data()+4)!=address)
            Fail(NativeAXFailure::InvalidVoiceChain,cmd,"AX parameter-block self address differs from its checked physical backing");
        if(BE16(wire.data()+16)!=0)
            Fail(NativeAXFailure::ActiveVoice,cmd,"AX active voice/sample/filter/SRC processing is not implemented");
        visited[voices++]=address;
        address=BE32(wire.data());
    }
    if(voices!=MaxVoices)
        Fail(NativeAXFailure::InvalidVoiceChain,cmd,"AX zero slice requires all96 original initialized parameter blocks");
    return voices;
}
struct WriteSpan {std::uint32_t address{};std::size_t bytes{};};
}
NativeAXCommandList ReadNativeAXCommandList(NativeDSPMemoryEndpoint endpoint,
                                          std::uint32_t address,std::size_t bytes) {
    if(!bytes || bytes>128 || bytes%2 || address%2 ||
       bytes>std::numeric_limits<std::uint32_t>::max()-std::uint64_t(address)+1)
        throw NativeAXCommandError(NativeAXFailure::InvalidListExtent,0,"AX command list must be an aligned bounded original128-byte slot");
    std::array<unsigned char,128> wire{};
    DSPBackendReadMemory(endpoint,address,wire.data(),bytes);
    NativeAXCommandList result;
    const auto available=bytes/2;
    for(std::size_t at=0;at<available;) {
        const auto opcode=BE16(wire.data()+at*2);
        if(opcode>=CommandWords.size())
            throw NativeAXCommandError(NativeAXFailure::UnknownCommand,at,"AX command is unknown; no firmware layout fallback");
        const auto words=CommandWords[opcode];
        if(words>available-at)
            throw NativeAXCommandError(NativeAXFailure::TruncatedCommand,at,"AX command exceeds its published list extent");
        auto& command=result.commands[result.command_count++];
        command.opcode=static_cast<NativeAXOpcode>(opcode);
        command.word_offset=static_cast<std::uint16_t>(at);command.argument_count=words-1;
        for(unsigned i=1;i<words;++i)command.arguments[i-1]=BE16(wire.data()+(at+i)*2);
        at+=words;result.consumed_words=static_cast<std::uint16_t>(at);
        if(command.opcode==NativeAXOpcode::End)return result;
    }
    throw NativeAXCommandError(NativeAXFailure::MissingEnd,available,"AX bounded command list has no END");
}
NativeAXZeroFrameResult ExecuteNativeAXZeroInputFrame(NativeDSPMemoryEndpoint endpoint,
                                                     std::uint32_t address,std::size_t bytes) {
    const auto list=ReadNativeAXCommandList(endpoint,address,bytes);
    // Validate the actual AXCL source sequence and every contributing input
    // before any output store. Parsing never executes arbitrary device commands.
    std::size_t next=0;
    auto Take=[&](NativeAXOpcode opcode)->const NativeAXCommand& {
        const auto& cmd=list.commands[next<list.command_count?next:list.command_count-1];
        if(next>=list.command_count || cmd.opcode!=opcode)
            Fail(NativeAXFailure::UnsupportedSequence,cmd,"AX zero slice requires the original frame command order");
        ++next;return cmd;
    };
    const auto& setup=Take(NativeAXOpcode::Setup);
    RequireZero(endpoint,Address(setup),120,NativeAXFailure::NonzeroStudio,setup,
                "AX depop Studio contributor is nonzero; source ramp processing is unsupported");
    const auto& surround=list.commands[next<list.command_count?next:list.command_count-1];
    if(next>=list.command_count || (surround.opcode!=NativeAXOpcode::AddToLR &&
       surround.opcode!=NativeAXOpcode::SubToLR && surround.opcode!=NativeAXOpcode::AddSubToLR))
        Fail(NativeAXFailure::UnsupportedSequence,surround,"AX original surround routing command is absent");
    ++next;
    const bool dpl2=surround.opcode==NativeAXOpcode::AddSubToLR;
    const auto surround_bytes=FrameSamples*sizeof(std::uint32_t)*(dpl2?2:1);
    RequireZero(endpoint,Address(surround),surround_bytes,NativeAXFailure::NonzeroSurround,surround,
                "AX prior surround contributor is nonzero; routing arithmetic is unsupported");
    const auto& process=Take(NativeAXOpcode::Process);
    const auto voices=CheckStoppedVoices(endpoint,Address(process),process);
    bool compressor=false;
    if(next<list.command_count && list.commands[next].opcode>=NativeAXOpcode::MixAuxA &&
       list.commands[next].opcode<=NativeAXOpcode::UploadAuxBMixLRSC)
        Fail(NativeAXFailure::AuxiliaryProcessing,list.commands[next],"AX auxiliary effect bus upload/mix processing is unsupported");
    if(next<list.command_count && list.commands[next].opcode==NativeAXOpcode::Compressor) {
        const auto& cmd=Take(NativeAXOpcode::Compressor);
        if(cmd.arguments[0]!=32768 || cmd.arguments[1]!=10)
            Fail(NativeAXFailure::UnsupportedCompressor,cmd,"AX zero slice requires the original compressor threshold32768/release10");
        // No attack is triggered by exactly zero samples. A fresh attenuation
        // history stays zero; no coefficient is guessed or substituted. Validate
        // that the source supplied a genuinely readable coefficient owner.
        DSPBackendValidateMemory(endpoint,Address(cmd,2),2,false);
        compressor=true;
    }
    const auto& remote=Take(NativeAXOpcode::RemoteOutput);
    const auto& output=Take(dpl2?NativeAXOpcode::OutputDPL2:NativeAXOpcode::Output);
    if(Address(output,1)!=Address(surround))
        Fail(NativeAXFailure::UnsupportedSequence,output,"AX zero slice output surround owner differs from original input owner");
    Take(NativeAXOpcode::End);
    if(next!=list.command_count)
        Fail(NativeAXFailure::UnsupportedSequence,list.commands[next],"AX commands follow END");
    const std::array<WriteSpan,6> writes{{
        {Address(remote,0),RemoteSamples*sizeof(std::uint16_t)},
        {Address(remote,2),RemoteSamples*sizeof(std::uint16_t)},
        {Address(remote,4),RemoteSamples*sizeof(std::uint16_t)},
        {Address(remote,6),RemoteSamples*sizeof(std::uint16_t)},
        {Address(output,1),surround_bytes},
        {Address(output,3),FrameSamples*2*sizeof(std::uint16_t)}}};
    for(const auto& write:writes)DSPBackendValidateMemory(endpoint,write.address,write.bytes,true);
    const std::array<unsigned char,768> zero{};
    std::uint32_t written=0;
    for(const auto& write:writes) {
        DSPBackendWriteMemory(endpoint,write.address,zero.data(),write.bytes);
        written+=static_cast<std::uint32_t>(write.bytes);
    }
    return {list.consumed_words,voices,FrameSamples,RemoteSamples,written,compressor,dpl2};
}
} // namespace mscharged::platform
