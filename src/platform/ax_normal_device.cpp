#include "platform/ax_normal_device.h"
#include "platform/interrupts.h"
#include <stdexcept>
#include <thread>

namespace mscharged::platform {
struct NativeAXNormalCommandDevice::State {
    NativeAXBootstrapDevice& device;
    NativeDSPMemoryEndpoint memory;
    DSPInstructionCore& chip;
    NativeAXSuppliedCoefficientROM coefficients;
    NativeAXNormalDeviceStatus status{};
    std::thread::id owner=std::this_thread::get_id();
    bool attached{};
    State(NativeAXBootstrapDevice& parent,NativeDSPMemoryEndpoint bus,DSPInstructionCore& chip,
          const NativeAXSuppliedCoefficientROM& supplied):device(parent),memory(bus),chip(chip),coefficients(supplied) {
        if(device.Status().phase!=NativeAXBootstrapPhase::InitPrefixCompleted)
            throw std::logic_error("AX normal slice requires the real original init prefix");
        device.RequireRetainedChip(chip);
        const auto gains=device.InitializedGainWords();
        // DataWord rejects an uninitialized cell; reset alone is not cold zero.
        const auto counter=chip.DataWord(0x0ce4);
        if(counter)throw std::logic_error("AX nonzero compressor history is unqualified");
        if(!coefficients.loaded())
            throw NativeAXVoiceError(NativeAXVoiceFailure::MissingCoefficientBank,
                                     "AX attached normal slice requires an explicit supplied coefficient bank");
        for(unsigned bank=0;bank<3;++bank)for(unsigned phase=0;phase<128;++phase) {
            const auto row=coefficients.Row(bank,phase<<9);
            for(unsigned tap=0;tap<4;++tap)
                if(std::uint16_t(row[tap])!=chip.DataWord(0x1000+bank*512+phase*4+tap))
                    throw std::invalid_argument("AX frame coefficient input differs from retained hardware bank");
        }
        status.history={gains[0],gains[1],gains[2],gains[3],counter,true};
    }
    void RequireOwner() const {
        if(!attached||owner!=std::this_thread::get_id())
            throw std::logic_error("AX normal slice needs its live native hardware owner");
        if(device.Status().phase!=NativeAXBootstrapPhase::InitPrefixCompleted)
            throw std::logic_error("AX normal slice initialization lifetime is no longer valid");
    }
    static std::array<std::uint16_t,5> Context(const NativeAXCommandHistory& h) {
        return {h.compressor_counter,h.master,h.aux_a,h.aux_b,h.aux_c};
    }
    static NativeAXDeviceFrameResult Process(void* context,NativeDSPMemoryEndpoint bus,
                                         std::uint32_t address,std::size_t bytes) {
        auto& s=*static_cast<State*>(context);s.RequireOwner();
        if(bus.generation!=s.memory.generation)
            throw std::logic_error("AX normal slice received a different bus lifetime");
        const auto before=Context(s.status.history);
        s.chip.ValidateNativeDataWords(0x0ce4,before.data(),before.size());
        const auto frame=PrepareNativeAXCommandFrame(bus,address,bytes,s.status.history,s.coefficients);
        CommitNativeAXCommandFrame(bus,frame,s.status.history);
        const auto after=Context(s.status.history);
        s.chip.CommitNativeDataWords(0x0ce4,before.data(),after.data(),after.size());
        ++s.status.processed_frames;s.status.last_active_voices=frame.active_voices;
        s.status.last_aux_commands=frame.aux_commands;
        // The outer real device publishes SYNC only after these genuine stores.
        return {frame.consumed_words,std::uint16_t(frame.voice_count-frame.active_voices),96,18,
                frame.written_bytes};
    }
};
NativeAXNormalCommandDevice::NativeAXNormalCommandDevice(NativeAXBootstrapDevice& device,
    NativeDSPMemoryEndpoint memory,DSPInstructionCore& chip,const NativeAXSuppliedCoefficientROM& coefficients)
    :state_(std::make_unique<State>(device,memory,chip,coefficients)) {
    NativeInterruptGuard exclusion;auto& s=*state_;
    device.AttachFrameProcessor({&s,State::Process});s.attached=true;
}
NativeAXNormalCommandDevice::~NativeAXNormalCommandDevice() {
    if(state_&&state_->attached)std::terminate();
}
NativeAXNormalDeviceStatus NativeAXNormalCommandDevice::Status() const {
    state_->RequireOwner();return state_->status;
}
void NativeAXNormalCommandDevice::Close() {
    NativeInterruptGuard exclusion;auto& s=*state_;
    if(!s.attached||s.owner!=std::this_thread::get_id())
        throw std::logic_error("AX normal slice retirement needs its live hardware owner");
    // Parent status may now be Faulted after a rejected real source request.
    // Its actual HALT/mail/cause checks, not a game readiness flag, govern close.
    s.device.DetachFrameProcessor(&s);s.attached=false;
}
}
