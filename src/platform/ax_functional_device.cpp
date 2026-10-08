#include "platform/ax_functional_device.h"
#include "platform/os_audio_registers.h"
#include "platform/dsp_control_abi.h"
#include "platform/dsp_mailbox_abi.h"
#include <dolphin/ai.h>
#include "platform/ai.h"
#include "platform/interrupts.h"
#include <limits>
#include <stdexcept>
#include <thread>

namespace mscharged::platform {
namespace {
constexpr std::array<std::size_t,13> Bytes{256,30720,6144,4608,4608,3456,4032,120,1152,768,1440,64,8192};
constexpr std::array<bool,13> Writable{true,true,true,true,true,true,false,false,true,true,true,true,false};
std::uint32_t Address(const NativeAXCommand& command,unsigned at=0) {
    return (std::uint32_t(command.arguments[at])<<16)|command.arguments[at+1];
}
bool Contains(std::uint32_t base,std::size_t bytes,std::uint32_t at,std::size_t count) {
    return at>=base&&std::uint64_t(at)+count<=std::uint64_t(base)+bytes;
}
}
struct NativeAXFunctionalDevice::State {
    NativeDSPMemoryEndpoint memory;
    NativeAXFunctionalBindings bindings;
    NativeAXNativeFilter filter;
    NativeAXCommandHistory history{};
    std::thread::id owner=std::this_thread::get_id();
    std::uint64_t initializations{},processed{};
    std::uint16_t active{},aux{};
    bool initialized{},retired{};
    void RequireOwner() const {
        if(retired||owner!=std::this_thread::get_id())
            throw std::logic_error("functional AX device needs its actual live native owner");
    }
    void ValidateBindings() const {
        RequireOwner();
        for(std::size_t i=0;i<Bytes.size();++i) {
            const auto address=bindings.addresses[i];
            if(!address||address%32||std::uint64_t(address)+Bytes[i]>std::uint64_t(std::numeric_limits<std::uint32_t>::max())+1)
                throw std::invalid_argument("functional AX source static span has invalid bus geometry");
            DSPBackendValidateMemory(memory,address,Bytes[i],Writable[i]);
            for(std::size_t j=0;j<i;++j)
                if(std::uint64_t(address)<std::uint64_t(bindings.addresses[j])+Bytes[j]&&
                   std::uint64_t(bindings.addresses[j])<std::uint64_t(address)+Bytes[i])
                    throw std::invalid_argument("functional AX source static spans overlap");
        }
    }
    void Role(unsigned index,std::uint32_t address,std::size_t count) const {
        if(!Contains(bindings.addresses[index],Bytes[index],address,count))
            throw std::invalid_argument("functional AX command address differs from its retained source storage role");
    }
    void ValidateList(const NativeAXCommandList& list) const {
        for(unsigned i=0;i<list.command_count;++i) {
            const auto& c=list.commands[i];
            switch(c.opcode) {
            case NativeAXOpcode::Setup: Role(7,Address(c),120);break;
            case NativeAXOpcode::AddToLR:case NativeAXOpcode::SubToLR:case NativeAXOpcode::AddSubToLR:
                Role(9,Address(c),384);break;
            case NativeAXOpcode::Process: Role(1,Address(c),320);break;
            case NativeAXOpcode::MixAuxA:case NativeAXOpcode::MixAuxB:case NativeAXOpcode::MixAuxC: {
                const unsigned role=3+unsigned(c.opcode)-unsigned(NativeAXOpcode::MixAuxA);
                Role(role,Address(c,1),1152);Role(role,Address(c,3),1152);break;
            }
            case NativeAXOpcode::Compressor:Role(6,Address(c,2),2);break;
            case NativeAXOpcode::RemoteOutput:
                for(unsigned channel=0;channel<4;++channel)Role(10,Address(c,channel*2),36);
                break;
            case NativeAXOpcode::Output:Role(9,Address(c,1),384);Role(8,Address(c,3),384);break;
            default:break; // The true parser/preparer rejects unqualified semantics.
            }
        }
    }
    static void Initialize(void* context,NativeDSPMemoryEndpoint memory) {
        auto& s=*static_cast<State*>(context);s.RequireOwner();
        if(memory.generation!=s.memory.generation||s.initialized)
            throw std::logic_error("functional AX native initialization has a stale or duplicate lifetime");
        s.ValidateBindings();
        // These are actual private native processor resources, never ISA DMEM.
        // Source AX firmware's cold unity gain words and fresh compressor state
        // establish the functional platform initial state; no source field is set.
        const NativeAXCommandHistory cold{0x8000,0x8000,0x8000,0x8000,0,true};
        for(unsigned bank=0;bank<3;++bank)for(unsigned phase=0;phase<128;++phase) {
            const auto row=s.filter.Row(bank,phase<<9);int sum{};
            for(const auto coefficient:row)sum+=coefficient;
            if(sum!=32768)throw std::logic_error("functional AX native filter resource failed DC validation");
        }
        s.history=cold;s.initialized=true;++s.initializations;
    }
    static void ValidateReset(void* context) {
        auto& s=*static_cast<State*>(context);s.RequireOwner();
        const auto ai=GetNativeAIStatus();
        if(ai.running||ai.interrupt_pending||ai.callback_active||
           ai.retained_blocks||ai.queued_input_bytes)
            throw std::logic_error("functional AX reset requires stopped/drained actual AI DMA");
    }
    static void Reset(void* context) {
        auto& s=*static_cast<State*>(context);s.RequireOwner();
        s.history={};s.initialized=false;s.processed=0;s.active=s.aux=0;
    }
    static NativeAXDeviceFrameResult Process(void* context,NativeDSPMemoryEndpoint memory,
                                             std::uint32_t address,std::size_t bytes) {
        auto& s=*static_cast<State*>(context);s.RequireOwner();
        if(!s.initialized||memory.generation!=s.memory.generation)
            throw std::logic_error("functional AX processing precedes its actual native initialization");
        s.ValidateBindings();
        if(bytes!=128||(address!=s.bindings.addresses[0]&&address!=s.bindings.addresses[0]+128))
            throw std::invalid_argument("functional AX request is not an actual source command slot");
        s.ValidateList(ReadNativeAXCommandList(memory,address,bytes));
        auto frame=PrepareNativeAXCommandFrame(memory,address,bytes,s.history,s.filter.View());
        for(const auto& voice:frame.voices) {
            s.Role(1,voice.parameter_address,320);
            if((voice.parameter_address-s.bindings.addresses[1])%320)
                throw std::invalid_argument("functional AX PB has a non-original record boundary");
        }
        CommitNativeAXCommandFrame(memory,frame,s.history);
        ++s.processed;s.active=frame.active_voices;s.aux=frame.aux_commands;
        return {frame.consumed_words,std::uint16_t(frame.voice_count-frame.active_voices),96,18,frame.written_bytes};
    }
};
NativeAXFunctionalDevice::NativeAXFunctionalDevice(NativeDSPMemoryEndpoint memory,
    NativeDSPMailboxEndpoint mailboxes,NativeDSPControlEndpoint control,
    const NativeAXFunctionalBindings& bindings,NativeAXCoefficientPolicy policy)
    :state_(std::make_unique<State>()) {
    if(policy!=NativeAXCoefficientPolicy::NativeWindowedSinc4TapV1)
        throw std::invalid_argument("functional AX requires explicitly selected NativeWindowedSinc4TapV1 policy");
    auto& s=*state_;s.memory=memory;s.bindings=bindings;s.ValidateBindings();
    const NativeAXFunctionalProcessor processor(&s,State::Initialize,State::ValidateReset,State::Reset,State::Process);
    protocol_=std::make_unique<NativeAXBootstrapDevice>(memory,mailboxes,control,bindings.addresses[12],processor);
    const NativeOSAudioRegisterOwner::Operations operations{
        [](void* p,std::uint32_t r){return static_cast<NativeAXFunctionalDevice*>(p)->ReadOSDSP(r);},
        [](void* p,std::uint32_t r,std::uint16_t v){static_cast<NativeAXFunctionalDevice*>(p)->WriteOSDSP(r,v);},
        [](void* p,std::uint32_t r){return static_cast<NativeAXFunctionalDevice*>(p)->ReadOSDSPPair(r);},
        [](void*,std::uint32_t,std::uint32_t){throw std::logic_error("functional AX has no OS boot ARAM register service");},
        [](void*,std::uint32_t)->std::uint32_t{throw std::logic_error("functional AX has no OS boot GPIO register service");},
        [](void*,std::uint32_t,std::uint32_t){throw std::logic_error("functional AX has no OS boot GPIO register service");},
        [](void*)->void*{throw std::logic_error("functional AX has no OS boot work-memory service");}
    };
    try {registers_.reset(new NativeOSAudioRegisterOwner(this,operations));}
    catch(...) {protocol_->Close();protocol_.reset();throw;}
}
NativeAXFunctionalDevice::~NativeAXFunctionalDevice() {
    if(protocol_)std::terminate();
}
void NativeAXFunctionalDevice::ServiceOwner() {state_->RequireOwner();protocol_->ServiceOwner();}
std::uint16_t NativeAXFunctionalDevice::ReadOSDSP(std::uint32_t reg) {
    state_->RequireOwner();
    switch(reg) {
    case 0:return ChargedDSPMailToHigh();
    case 2:return ChargedDSPMailFromHigh();
    case 3:return ChargedDSPMailFromLow();
    case 5:ServiceOwner();return ChargedDSPControlRead();
    case 27:{const auto ai=GetNativeAIStatus();return static_cast<std::uint16_t>((ai.dma_bytes/32)&0x7fff)|(ai.running?0x8000:0);}
    default:throw std::logic_error("functional AX requested an unsupported OS audio register read");
    }
}
void NativeAXFunctionalDevice::WriteOSDSP(std::uint32_t reg,std::uint16_t value) {
    state_->RequireOwner();
    switch(reg) {
    case 0:ChargedDSPMailToWriteHigh(value);return;
    case 5:ChargedDSPControlWrite(value);return;
    case 27:{
        const auto ai=GetNativeAIStatus();
        if((value&0x8000)||(value&0x7fff)!=((ai.dma_bytes/32)&0x7fff))
            throw std::logic_error("OS stop requested unsupported AI DMA register changes");
        if(ai.running)AIStopDMA();return;
    }
    default:throw std::logic_error("functional AX requested an unsupported OS audio register write");
    }
}
std::uint32_t NativeAXFunctionalDevice::ReadOSDSPPair(std::uint32_t reg) {
    state_->RequireOwner();
    if(reg!=2)throw std::logic_error("functional AX OS audio mailbox pair read is unsupported");
    const auto high=ReadOSDSP(reg);const auto low=ReadOSDSP(reg+1);
    return (std::uint32_t(high)<<16)|low;
}
NativeAXFunctionalStatus NativeAXFunctionalDevice::Status() const {
    const auto& s=*state_;s.RequireOwner();
    return {protocol_->Status(),protocol_->FrameStatus(),s.history,
        NativeAXCoefficientPolicy::NativeWindowedSinc4TapV1,s.initializations,s.processed,s.active,s.aux,s.initialized};
}
void NativeAXFunctionalDevice::Close() {
    NativeInterruptGuard exclusion;auto& s=*state_;s.RequireOwner();
    const auto ai=GetNativeAIStatus();
    if(ai.running||ai.interrupt_pending||ai.callback_active)
        throw std::logic_error("functional AX retirement requires actual AI stop/drain");
    const auto mail=GetNativeDSPMailboxStatus();const auto csr=GetNativeDSPControlStatus().csr;
    const auto frame=protocol_->FrameStatus();
    if(mail.cpu_mail_full||mail.dsp_mail_full||(csr&0x80)||
       (frame.phase!=NativeAXFramePhase::Unavailable&&frame.phase!=NativeAXFramePhase::ReadyForListSize&&
        frame.phase!=NativeAXFramePhase::Faulted))
        throw std::logic_error("functional AX retirement requires actual drained source mail/cause/job");
    // The protocol refuses retirement while the DSP is live; only after it
    // accepts does the OS audio register routing retire, so a refused Close
    // leaves the device whole and retirable later.
    protocol_->Close();protocol_.reset();
    registers_->Close();
    s.history={};s.initialized=false;s.retired=true;
}
} // namespace mscharged::platform
