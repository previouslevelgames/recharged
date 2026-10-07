#include "platform/dsp_boot_memory.h"
#include <stdexcept>

namespace mscharged::platform {
NativeDSPBootMemory::NativeDSPBootMemory(NativeDSPMemoryEndpoint memory,NativeDSPControlEndpoint control,
                                       DSPInstructionCore& core)
    :memory_(memory),control_(control),core_(core),owner_(std::this_thread::get_id()) {
    RequireOwner();
}
void NativeDSPBootMemory::RequireOwner() const {
    if(owner_!=std::this_thread::get_id())
        throw std::logic_error("DSP boot memory transport requires its live native owner");
    const auto state=GetNativeDSPControlStatus();
    if(!state.connected || state.generation!=control_.generation || !(state.csr&4))
        throw std::logic_error("DSP boot memory transfer requires actual halted/drained hardware");
}
void NativeDSPBootMemory::TransferARAM(std::uint32_t main,std::uint32_t aram,std::uint32_t count) {
    RequireOwner();
    // The actual OS bootstrap makes only these two identical32-byte writes.
    // Do not pretend this qualified leaf implements generic ARAM/read DMA.
    if(main!=0x01000000u || aram!=0 || count!=32)
        throw std::invalid_argument("DSP boot ARAM request has an unqualified address/count/direction");
    std::array<unsigned char,32> staged;
    DSPBackendReadMemory(memory_,main,staged.data(),staged.size());
    aram_=staged;aram_written_=true;
    ++status_.aram_transfers;status_.last_aram_bytes=staged.size();
}
void NativeDSPBootMemory::TransferBootstrapInstructions() {
    RequireOwner();
    // Primary hardware initialization evidence records a1024-byte transfer
    // independent of the source's32-byte ARAM requests and128-byte program.
    // Copy the full actual supplied backing; never trim or synthesize its tail.
    core_.LoadInstructionMemory(memory_,0x01000000u,1024,0);
    ++status_.instruction_transfers;status_.last_instruction_bytes=1024;
}
std::array<unsigned char,32> NativeDSPBootMemory::ARAMBytes() const {
    RequireOwner();
    if(!aram_written_)throw std::logic_error("DSP boot ARAM bytes were never supplied by a transfer");
    return aram_;
}
NativeDSPBootMemoryStatus NativeDSPBootMemory::Status() const {RequireOwner();return status_;}
} // namespace mscharged::platform
