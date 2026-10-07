#include "platform/ax_bootstrap_device.h"
#include "platform/ax_command_service.h"
#include "platform/dsp_instruction_core.h"
#include "platform/dsp_control_abi.h"
#include "platform/interrupt_controller.h"
#include "platform/interrupts.h"
#include <array>
#include <algorithm>
#include <exception>
#include <stdexcept>
#include <thread>

namespace mscharged::platform {
namespace {
constexpr std::uint16_t Halt=4,Init=0x0800;
constexpr std::size_t FirmwareBytes=8192;
std::uint32_t PrepareRetainedChip(DSPInstructionCore& chip,NativeDSPMailboxEndpoint mailboxes,
                                 NativeDSPControlEndpoint control,std::uint32_t firmware) {
    chip.RequireHardwareEndpoints(mailboxes,control);chip.PauseHaltedExecution();return firmware;
}
// Exact SHA256 of the original reconstructed AX DSPCode.c byte image. This
// identifies task hardware code, not an external ROM or guessed replacement.
constexpr std::array<std::uint32_t,8> FirmwareDigest{
    0x329e87af,0xe881d4b5,0x71c74ac9,0x52c599ee,
    0xab8ec678,0x2748565c,0x8fd15551,0xc04bea24};
std::uint32_t Rotate(std::uint32_t x,unsigned n) {return (x>>n)|(x<<(32-n));}
// FIPS180-4 SHA256, fixed8192-byte input plus its mandatory padding block.
// Unsigned32 arithmetic preserves modular additions independent of host endian.
std::array<std::uint32_t,8> Digest(const std::array<unsigned char,FirmwareBytes>& input) {
    constexpr std::array<std::uint32_t,64> k{
        0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
        0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
        0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
        0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
        0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
        0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
        0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
        0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};
    std::array<std::uint32_t,8> h{0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,
        0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
    for(std::size_t block=0;block<=FirmwareBytes/64;++block) {
        std::array<std::uint32_t,64> w{};
        if(block<FirmwareBytes/64) {
            for(unsigned j=0;j<16;++j)for(unsigned b=0;b<4;++b)
                w[j]=(w[j]<<8)|input[block*64+j*4+b];
        } else {w[0]=0x80000000;w[15]=FirmwareBytes*8;}
        for(unsigned j=16;j<64;++j) {
            const auto s0=Rotate(w[j-15],7)^Rotate(w[j-15],18)^(w[j-15]>>3);
            const auto s1=Rotate(w[j-2],17)^Rotate(w[j-2],19)^(w[j-2]>>10);
            w[j]=w[j-16]+s0+w[j-7]+s1;
        }
        auto v=h;
        for(unsigned j=0;j<64;++j) {
            const auto s1=Rotate(v[4],6)^Rotate(v[4],11)^Rotate(v[4],25);
            const auto t1=v[7]+s1+((v[4]&v[5])^(~v[4]&v[6]))+k[j]+w[j];
            const auto s0=Rotate(v[0],2)^Rotate(v[0],13)^Rotate(v[0],22);
            const auto t2=s0+((v[0]&v[1])^(v[0]&v[2])^(v[1]&v[2]));
            for(unsigned n=7;n>0;--n)v[n]=v[n-1];
            v[4]+=t1;v[0]=t1+t2;
        }
        for(unsigned n=0;n<8;++n)h[n]+=v[n];
    }
    return h;
}
} // namespace
struct NativeAXBootstrapDevice::State {
    NativeDSPMemoryEndpoint memory;
    NativeDSPMailboxEndpoint mailboxes;
    NativeDSPControlEndpoint control;
    std::uint32_t firmware;
    std::thread::id owner=std::this_thread::get_id();
    NativeAXBootstrapPhase phase=NativeAXBootstrapPhase::Cold;
    std::array<std::uint32_t,10> loader{};
    std::uint16_t words{};
    std::uint64_t resets{};
    std::unique_ptr<DSPInstructionCore> owned_core;
    DSPInstructionCore* core{};
    DSPInstructionCore* retained_chip{};
    bool stopped_voice_commands{};
    NativeAXFrameProcessor processor{};
    void* native_context{};
    void (*native_initialize)(void*,NativeDSPMemoryEndpoint){};
    void (*native_reset)(void*){};
    NativeAXDeviceFrameResult (*native_process)(void*,NativeDSPMemoryEndpoint,std::uint32_t,std::size_t){};
    bool CommandsEnabled() const noexcept {return stopped_voice_commands||processor.process||native_process;}
    NativeAXStoppedVoiceStatus frames{};
    bool servicing{},attached{};
    void RequireOwner() const {
        if(owner!=std::this_thread::get_id()||phase==NativeAXBootstrapPhase::Retired)
            throw std::logic_error("AX bootstrap device needs its actual live CPU owner");
        const auto c=GetNativeDSPControlStatus();const auto m=GetNativeDSPMailboxStatus();
        if(!c.connected||c.generation!=control.generation||!m.connected||m.generation!=mailboxes.generation)
            throw std::logic_error("AX bootstrap device hardware generation retired");
    }
    void ValidateImage() const {
        std::array<unsigned char,FirmwareBytes> bytes{};
        DSPBackendReadMemory(memory,firmware,bytes.data(),bytes.size());
        if(Digest(bytes)!=FirmwareDigest)
            throw std::invalid_argument("AX bootstrap task image is not the exact source firmware");
    }
    void PublishLoader() {
        // A real native loader is operational only after cold state and actual
        // authorized source memory are validated. Primary ROM loader protocol
        // identifies its ready message8071FEED. This is NOT an AX INIT reply.
        ValidateImage();
        if(DSPBackendMailToHigh(mailboxes)&0x8000)
            throw std::logic_error("AX cold native loader found pending source request");
        if(GetNativeDSPMailboxStatus().dsp_mail_full)
            throw std::logic_error("AX native loader cannot overwrite unacknowledged output mail");
        DSPBackendMailFromWriteHigh(mailboxes,0x8071);
        DSPBackendMailFromWriteLow(mailboxes,0xfeed);
        phase=NativeAXBootstrapPhase::LoaderReady;
    }
    static void ValidateControl(void* context,std::uint16_t previous,std::uint16_t request) {
        auto& s=*static_cast<State*>(context);s.RequireOwner();
        if(request&2)throw std::logic_error("AX bootstrap PI interrupt/task continuation is not implemented");
        if((request&1)&&s.native_process &&
           (s.servicing || GetNativeDSPMailboxStatus().cpu_mail_full ||
            GetNativeDSPMailboxStatus().dsp_mail_full || (previous&0x80) ||
            (s.frames.phase!=NativeAXFramePhase::Unavailable &&
             s.frames.phase!=NativeAXFramePhase::ReadyForListSize &&
             s.frames.phase!=NativeAXFramePhase::Faulted)))
            throw std::logic_error("native AX reset requires its actual idle/drained request lifetime");
        if((request&1)&&s.processor.process)
            throw std::logic_error("AX hardware processor must halt/drain/detach before reset");
        if((request&1)&&!(request&Halt))
            throw std::logic_error("AX bootstrap reset requires actual halted/drained source device");
        if((request&Init)&&(!(previous&Init)||(!(request&Halt)&&s.phase==NativeAXBootstrapPhase::Cold)))s.ValidateImage();
        if(!(request&Halt)&&((request&Init)||s.native_process)&&s.phase==NativeAXBootstrapPhase::Faulted)
            throw std::logic_error("AX bootstrap device fault needs real halted reset");
    }
    static void ApplyControl(void* context,std::uint16_t previous,std::uint16_t request) {
        auto& s=*static_cast<State*>(context);s.RequireOwner();
        if(request&1) {
            if(s.native_reset)s.native_reset(s.native_context);
            if(s.retained_chip)s.retained_chip->PauseHaltedExecution();
            else {s.owned_core.reset();s.core=nullptr;}
            s.loader={};s.words=0;++s.resets;
            s.frames={};
            DSPBackendResetMailboxes(s.mailboxes);s.phase=NativeAXBootstrapPhase::Cold;
        }
        if((request&Init)&&!(request&Halt)&&
            ((previous&Halt)||!(previous&Init))&&s.phase==NativeAXBootstrapPhase::Cold)
            s.PublishLoader();
    }
    void Consume(std::uint32_t word) {
        if((phase==NativeAXBootstrapPhase::InitPrefixCompleted ||
            phase==NativeAXBootstrapPhase::NativeKernelInitialized)&&CommandsEnabled()) {
            ConsumeFrameWord(word);
            return;
        }
        if(phase!=NativeAXBootstrapPhase::LoaderReady&&phase!=NativeAXBootstrapPhase::Loading)
            throw std::logic_error("AX bootstrap commands/frames are not implemented after device-init prefix");
        constexpr std::array<std::uint32_t,5> commands{0x00f3a001,0x00f3c002,0x00f3a002,0x00f3b002,0x00f3d001};
        if(words%2==0&&word!=commands[words/2])
            throw std::invalid_argument("AX loader request differs from original ten-word protocol");
        if(words%2) {
            const auto expected=words==1?firmware:words==5?std::uint32_t(FirmwareBytes):words==9?0x10u:0u;
            if(word!=expected)throw std::invalid_argument("AX loader field/address/vector differs from authorized source task");
        }
        loader[words++]=word;phase=NativeAXBootstrapPhase::Loading;
        if(words==10) {
            if(GetNativeDSPMailboxStatus().dsp_mail_full)
                throw std::logic_error("AX INIT cannot overwrite unread actual loader-ready mail");
            ValidateImage();
            if(native_initialize) {
                native_initialize(native_context,memory);
                // Real supported native resources are initialized before the
                // original source handler can receive INIT. No ISA cells exist.
                phase=NativeAXBootstrapPhase::NativeKernelInitialized;
                frames.phase=NativeAXFramePhase::ReadyForListSize;
                PublishFrameCause(0);
                return;
            }
            if(retained_chip)core=retained_chip;
            else {owned_core=std::make_unique<DSPInstructionCore>(mailboxes,control);core=owned_core.get();}
            core->LoadInstructionMemory(memory,firmware,FirmwareBytes,0);
            DSPInstructionRegisters initial{};initial.pc=0x10;core->BeginExecution(initial);
            // Every step is the literal current qualified core executing the
            // supplied source image. INIT and DIRQ are actual SRS instructions.
            for(unsigned n=0;n<21;++n)core->Step();
            const auto result=core->Registers();
            if(result.pc!=0x30||result.instructions!=21||!(GetNativeDSPControlStatus().csr&0x80))
                throw std::logic_error("AX source initialization prefix did not complete actual IRQ");
            phase=NativeAXBootstrapPhase::InitPrefixCompleted;
            if(stopped_voice_commands)frames.phase=NativeAXFramePhase::ReadyForListSize;
        }
    }
    void PublishFrameCause(std::uint16_t code) {
        if(GetNativeDSPMailboxStatus().dsp_mail_full ||
           (GetNativeDSPControlStatus().csr&0x80))
            throw std::logic_error("AX completed hardware cause would overwrite an unacknowledged mail/IRQ");
        // Native hardware command semantics, not a guessed ready reply or a
        // claim that the complete DSP image executed. The real source firmware
        // output/END contracts are SYNC4 and YIELD2, each with a separate DIRQ.
        DSPBackendMailFromWriteHigh(mailboxes,0xdcd1);
        DSPBackendMailFromWriteLow(mailboxes,code);
        DSPBackendWriteInterruptRequest(control,1);
    }
    void ConsumeFrameWord(std::uint32_t word) {
        switch(frames.phase) {
        case NativeAXFramePhase::ReadyForListSize:
            if(word!=0x3abe0080)
                throw std::invalid_argument("AX device requires original BABE0080 list-size mail");
            if(GetNativeDSPMailboxStatus().dsp_mail_full ||
               (GetNativeDSPControlStatus().csr&0x80))
                throw std::logic_error("AX frame request precedes genuine initialization acknowledgment");
            frames.phase=NativeAXFramePhase::ReadyForListAddress;
            break;
        case NativeAXFramePhase::ReadyForListAddress: {
            // The actual command service validates contributors and all output
            // owners before stores. Unsupported work publishes no completion.
            DSPBackendValidateMemory(memory,word,128,false);
            NativeAXDeviceFrameResult result;
            if(native_process)result=native_process(native_context,memory,word,128);
            else if(processor.process)result=processor.process(processor.context,memory,word,128);
            else {
                const auto zero=ExecuteNativeAXZeroInputFrame(memory,word,128);
                result={zero.consumed_words,zero.stopped_voices,zero.stereo_frames,
                        zero.remote_samples_per_channel,zero.written_bytes};
            }
            frames.last_list_address=word;frames.written_bytes=result.written_bytes;
            frames.stopped_voices=result.stopped_voices;
            frames.stereo_frames=result.stereo_frames;
            frames.remote_samples=result.remote_samples_per_channel;
            ++frames.processed_frames;
            frames.phase=NativeAXFramePhase::WaitingSyncAcknowledgment;
            PublishFrameCause(4);
            ++frames.sync_interrupts;
            break;
        }
        case NativeAXFramePhase::WaitingContinue:
            // Original singleton-task YIELD handler writes this genuine reply,
            // then invokes the actual source resume callback. No host flag or
            // callback is written/invoked here.
            if(word!=0x4dd10003 || GetNativeDSPMailboxStatus().dsp_mail_full ||
               (GetNativeDSPControlStatus().csr&0x80))
                throw std::invalid_argument("AX stopped device requires acknowledged source CDD10003 continuation");
            ++frames.source_continues;++frames.completed_frames;
            frames.phase=NativeAXFramePhase::ReadyForListSize;
            break;
        default:
            throw std::logic_error("AX frame request is out of order or its hardware service is unavailable");
        }
    }
    void ConsumePending() {
        const auto high=DSPBackendMailToHigh(mailboxes);
        if(high&0x8000) {
            const auto low=DSPBackendMailToLow(mailboxes);
            Consume((std::uint32_t(high&0x7fff)<<16)|low);
        }
    }
    bool AdvanceAcknowledgedOutput() {
        if(CommandsEnabled()&&frames.phase==NativeAXFramePhase::WaitingSyncAcknowledgment&&
           !GetNativeDSPMailboxStatus().dsp_mail_full&&!(GetNativeDSPControlStatus().csr&0x80)) {
            // Preserve the real SYNC acknowledgment before the END/YIELD cause.
            frames.phase=NativeAXFramePhase::WaitingContinue;
            PublishFrameCause(2);++frames.yield_interrupts;return true;
        }
        return false;
    }
    void Poll() {
        RequireOwner();
        if(servicing) {
            // Original handlers synchronously wait for CONTINUE consumption.
            // A late AI period may also make the source resume callback send
            // the next list before this outer interrupt service returns. Consume
            // only actual expected mail at these register safe points; publish
            // real job causes, but never recursively dispatch another IRQ.
            if(CommandsEnabled()&&
               (frames.phase==NativeAXFramePhase::WaitingContinue||
                frames.phase==NativeAXFramePhase::ReadyForListSize||
                frames.phase==NativeAXFramePhase::ReadyForListAddress)&&
               !(GetNativeDSPControlStatus().csr&Halt)) {
                try {ConsumePending();}
                catch(...) {phase=NativeAXBootstrapPhase::Faulted;frames.phase=NativeAXFramePhase::Faulted;throw;}
            }
            return;
        }
        servicing=true;
        struct Guard {bool& value;~Guard(){value=false;}} guard{servicing};
        try {
            if(!(GetNativeDSPControlStatus().csr&Halt)) {
                ConsumePending();
                AdvanceAcknowledgedOutput();
            }
            // This safe point only delivers real pending causes, never invokes
            // source callbacks directly. Source masks/context still control it.
            if(NativeInterruptsEnabled())ServiceNativeInterruptController();
            if(!(GetNativeDSPControlStatus().csr&Halt)&&AdvanceAcknowledgedOutput()&&NativeInterruptsEnabled())
                ServiceNativeInterruptController();
        } catch(...) {
            phase=NativeAXBootstrapPhase::Faulted;
            if(CommandsEnabled())frames.phase=NativeAXFramePhase::Faulted;
            throw;
        }
    }
    static void Service(void* context) {static_cast<State*>(context)->Poll();}
};
NativeAXBootstrapDevice::NativeAXBootstrapDevice(NativeDSPMemoryEndpoint memory,NativeDSPMailboxEndpoint mailboxes,
                                               NativeDSPControlEndpoint control,std::uint32_t firmware)
    :NativeAXBootstrapDevice(memory,mailboxes,control,firmware,NativeAXFrameMode::BootstrapOnly) {}
NativeAXBootstrapDevice::NativeAXBootstrapDevice(NativeDSPMemoryEndpoint memory,NativeDSPMailboxEndpoint mailboxes,
                                               NativeDSPControlEndpoint control,std::uint32_t firmware,
                                               NativeAXFrameMode frame_mode)
    :state_(std::make_unique<State>()) {
    auto& s=*state_;s.memory=memory;s.mailboxes=mailboxes;s.control=control;s.firmware=firmware;
    if(frame_mode!=NativeAXFrameMode::BootstrapOnly&&frame_mode!=NativeAXFrameMode::StoppedVoices)
        throw std::invalid_argument("AX device frame mode is unknown");
    s.stopped_voice_commands=frame_mode==NativeAXFrameMode::StoppedVoices;
    s.RequireOwner();s.ValidateImage();
    const auto cold=GetNativeDSPControlStatus().csr;const auto cells=GetNativeDSPMailboxStatus();
    if(!(cold&Halt)||(cold&0x82)||cells.cpu_mail_full||cells.dsp_mail_full)
        throw std::logic_error("AX bootstrap attach needs actual cold HALT with drained mail/IRQ");
    AttachNativeDSPProcessorControl(control,{State::ValidateControl,State::ApplyControl,&s});
    try {AttachNativeDSPMailboxService(mailboxes,State::Service,&s);}
    catch(...) {DetachNativeDSPProcessorControl(control,&s);throw;}
    s.attached=true;
}
NativeAXBootstrapDevice::NativeAXBootstrapDevice(NativeDSPMemoryEndpoint memory,NativeDSPMailboxEndpoint mailboxes,
                                               NativeDSPControlEndpoint control,std::uint32_t firmware,
                                               NativeAXFrameMode mode,DSPInstructionCore& chip)
    :NativeAXBootstrapDevice(memory,mailboxes,control,PrepareRetainedChip(chip,mailboxes,control,firmware),mode) {
    state_->retained_chip=&chip;state_->core=&chip;
}
NativeAXBootstrapDevice::NativeAXBootstrapDevice(NativeDSPMemoryEndpoint memory,
    NativeDSPMailboxEndpoint mailboxes,NativeDSPControlEndpoint control,std::uint32_t firmware,
    const NativeAXFunctionalProcessor& processor)
    :NativeAXBootstrapDevice(memory,mailboxes,control,firmware,NativeAXFrameMode::BootstrapOnly) {
    auto& s=*state_;s.native_context=processor.context_;s.native_initialize=processor.initialize_;
    s.native_reset=processor.reset_;s.native_process=processor.process_;
}
NativeAXBootstrapDevice::~NativeAXBootstrapDevice() {
    if(state_&&state_->attached) {
        // An omitted explicit halt/drain is an owner lifetime bug; do not leave
        // a dangling callable context or silently repair source/task flags.
        std::terminate();
    }
}
void NativeAXBootstrapDevice::RequireRetainedChip(const DSPInstructionCore& chip) const {
    const auto& s=*state_;s.RequireOwner();
    if(s.phase!=NativeAXBootstrapPhase::InitPrefixCompleted||s.retained_chip!=&chip||s.core!=&chip)
        throw std::logic_error("AX normal context must belong to the actual initialized retained chip");
}
void NativeAXBootstrapDevice::AttachFrameProcessor(NativeAXFrameProcessor processor) {
    NativeInterruptGuard exclusion;auto& s=*state_;s.RequireOwner();
    if(!processor.context||!processor.process||s.processor.process||s.servicing||
       s.phase!=NativeAXBootstrapPhase::InitPrefixCompleted||
       (s.frames.phase!=NativeAXFramePhase::Unavailable&&s.frames.phase!=NativeAXFramePhase::ReadyForListSize)||
       GetNativeDSPMailboxStatus().cpu_mail_full||GetNativeDSPMailboxStatus().dsp_mail_full||
       (GetNativeDSPControlStatus().csr&0x84))
        throw std::logic_error("AX native frame processor requires an acknowledged idle original init prefix");
    s.processor=processor;s.frames.phase=NativeAXFramePhase::ReadyForListSize;
}
void NativeAXBootstrapDevice::DetachFrameProcessor(void* context) {
    NativeInterruptGuard exclusion;auto& s=*state_;s.RequireOwner();
    if(!s.processor.process||s.processor.context!=context||s.servicing||
       !(GetNativeDSPControlStatus().csr&Halt)||
       GetNativeDSPMailboxStatus().cpu_mail_full||GetNativeDSPMailboxStatus().dsp_mail_full||
       (GetNativeDSPControlStatus().csr&0x80)||
       (s.frames.phase!=NativeAXFramePhase::ReadyForListSize&&s.frames.phase!=NativeAXFramePhase::Faulted))
        throw std::logic_error("AX native frame processor needs real halted/drained owner retirement");
    s.processor={};if(!s.stopped_voice_commands)s.frames.phase=NativeAXFramePhase::Unavailable;
}
void NativeAXBootstrapDevice::ServiceOwner() {state_->Poll();}
NativeAXBootstrapStatus NativeAXBootstrapDevice::Status() const {
    const auto& s=*state_;s.RequireOwner();return {s.phase,s.words,(s.core&&s.words==10)?s.core->Registers().instructions:0,s.resets,
        (GetNativeDSPControlStatus().csr&Halt)!=0};
}
NativeAXStoppedVoiceStatus NativeAXBootstrapDevice::FrameStatus() const {
    state_->RequireOwner();return state_->frames;
}
void NativeAXBootstrapDevice::Close() {
    auto& s=*state_;s.RequireOwner();if(!(GetNativeDSPControlStatus().csr&Halt)||s.servicing)
        throw std::logic_error("AX bootstrap processor must halt/drain before owner retirement");
    if(s.processor.process)throw std::logic_error("AX hardware processor must detach before device retirement");
    DetachNativeDSPMailboxService(s.mailboxes,&s);DetachNativeDSPProcessorControl(s.control,&s);
    if(s.retained_chip)s.retained_chip->PauseHaltedExecution();
    s.owned_core.reset();s.core=nullptr;s.attached=false;s.phase=NativeAXBootstrapPhase::Retired;
}
} // namespace mscharged::platform

namespace mscharged::platform {
std::array<std::uint16_t,4> NativeAXBootstrapDevice::InitializedGainWords() const {
    state_->RequireOwner();
    if(state_->phase!=NativeAXBootstrapPhase::InitPrefixCompleted || !state_->core)
        throw std::logic_error("AX gain context needs actual completed initialization instructions");
    return {state_->core->DataWord(0x0ce5),state_->core->DataWord(0x0ce6),
            state_->core->DataWord(0x0ce7),state_->core->DataWord(0x0ce8)};
}
} // namespace mscharged::platform
