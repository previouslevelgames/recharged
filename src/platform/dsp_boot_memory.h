#pragma once
#include "platform/dsp_instruction_core.h"
#include <array>
#include <cstdint>
#include <thread>

namespace mscharged::platform {
struct NativeDSPBootMemoryStatus {
    std::uint64_t aram_transfers, instruction_transfers;
    std::uint32_t last_aram_bytes, last_instruction_bytes;
};
// Bounded boot hardware memory transport only, beneath unchanged OS requests.
// No CSR reset context, GPIO clock, ARAM cause/IRQ, program start, ready mail
// or source flag is supplied. Owner retains the same chip/core and real pins,
// halted/drained throughout each transfer. Other ARAM modes remain unsupported.
class NativeDSPBootMemory {
public:
    NativeDSPBootMemory(NativeDSPMemoryEndpoint memory,NativeDSPControlEndpoint control,
                        DSPInstructionCore& chip_core);
    // Exact source boot main-memory->ARAM request: address01000000, ARAM0,
    // count32. The independent hardware RES/reset bootstrap copies1024 bytes.
    void TransferARAM(std::uint32_t main_address,std::uint32_t aram_address,
                      std::uint32_t count_and_direction);
    void TransferBootstrapInstructions();
    std::array<unsigned char,32> ARAMBytes() const;
    NativeDSPBootMemoryStatus Status() const;
private:
    void RequireOwner() const;
    NativeDSPMemoryEndpoint memory_;
    NativeDSPControlEndpoint control_;
    DSPInstructionCore& core_;
    std::thread::id owner_;
    std::array<unsigned char,32> aram_{};
    bool aram_written_{};
    NativeDSPBootMemoryStatus status_{};
};
} // namespace mscharged::platform
