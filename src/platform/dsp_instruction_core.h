#pragma once
#include "platform/dsp_memory.h"
#include "platform/dsp_mailbox.h"
#include <array>
#include <cstddef>
#include <cstdint>
#include <stdexcept>

namespace mscharged::platform {
struct DSPInstructionRegisters {
    std::uint16_t pc{},status{};
    std::uint8_t control{};
    std::array<std::uint64_t,2> accumulator{}; // Exact raw 40-bit hardware cells.
    std::uint64_t instructions{}; // Executed instructions, not cycle timing.
};
struct DSPUnsupportedInstruction : std::runtime_error {
    DSPUnsupportedInstruction(std::uint16_t address,std::uint16_t word,std::uint16_t detail);
    std::uint16_t pc,opcode,operand;
};

// Partial real DSP ISA, with no boot/readiness replacement. Register input is
// an explicit diagnostic context until ROM/control/start sequencing exists.
// Unknown instructions/registers/IFX operations fail before a success update.
class DSPInstructionCore {
public:
    explicit DSPInstructionCore(NativeDSPMailboxEndpoint mailboxes);
    void LoadInstructionMemory(NativeDSPMemoryEndpoint endpoint,std::uint32_t physical_address,
                               std::uint32_t bytes,std::uint16_t word_address);
    void LoadDataMemory(NativeDSPMemoryEndpoint endpoint,std::uint32_t physical_address,
                        std::uint32_t bytes,std::uint16_t word_address);
    void BeginExecution(const DSPInstructionRegisters& context);
    DSPInstructionRegisters Step();
    DSPInstructionRegisters Registers() const;
    std::uint16_t InstructionWord(std::uint16_t address) const;
    std::uint16_t DataWord(std::uint16_t address) const;
private:
    static constexpr std::size_t WORDS=4096;
    NativeDSPMailboxEndpoint mailboxes_;
    std::array<std::uint16_t,WORDS> instructions_{},data_{};
    std::array<bool,WORDS> instruction_valid_{},data_valid_{};
    DSPInstructionRegisters registers_{};
    bool running_{};
    void Load(NativeDSPMemoryEndpoint endpoint,std::uint32_t physical_address,
              std::uint32_t bytes,std::uint16_t word_address,bool instruction);
};
} // namespace mscharged::platform
