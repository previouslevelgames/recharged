#pragma once
#include "platform/dsp_memory.h"
#include "platform/dsp_mailbox.h"
#include "platform/dsp_control.h"
#include <optional>
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
    // Exact16-bit hardware cells. Defaults are fixture input, not reset values.
    std::array<std::uint16_t,4> address{},index{},wrap{};
    // Exact16-bit ST0..ST3 cells. Only qualified LOOP/BLOOP stack transitions are
    // qualified; general call/data stack instructions remain unsupported.
    std::array<std::uint16_t,4> stack{};
    // Saved ST0/ST2/ST3 tops for the four hardware loop levels. Explicit
    // context metadata is not a hardware reset/ROM initialization claim.
    std::array<std::array<std::uint16_t,3>,4> loop_stack{};
    std::uint8_t loop_depth{};
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
    // Retained unconnected diagnostic mode cannot execute IFX IRQ requests.
    explicit DSPInstructionCore(NativeDSPMailboxEndpoint mailboxes);
    DSPInstructionCore(NativeDSPMailboxEndpoint mailboxes,NativeDSPControlEndpoint control);
    void LoadInstructionMemory(NativeDSPMemoryEndpoint endpoint,std::uint32_t physical_address,
                               std::uint32_t bytes,std::uint16_t word_address);
    void LoadDataMemory(NativeDSPMemoryEndpoint endpoint,std::uint32_t physical_address,
                        std::uint32_t bytes,std::uint16_t word_address);
    // Caller-supplied complete BE ROM banks, copied through genuine checked
    // bus backing. This supplies bytes, never a boot context or readiness.
    void LoadInstructionROM(NativeDSPMemoryEndpoint endpoint,std::uint32_t physical_address);
    void LoadCoefficientROM(NativeDSPMemoryEndpoint endpoint,std::uint32_t physical_address);
    void BeginExecution(const DSPInstructionRegisters& context);
    DSPInstructionRegisters Step();
    DSPInstructionRegisters Registers() const;
    std::uint16_t InstructionWord(std::uint16_t address) const;
    std::uint16_t DataWord(std::uint16_t address) const;
private:
    static constexpr std::size_t WORDS=4096;
    static constexpr std::size_t IROM_WORDS=4096;
    static constexpr std::size_t COEFFICIENT_WORDS=2048;
    NativeDSPMailboxEndpoint mailboxes_;
    std::optional<NativeDSPControlEndpoint> control_;
    std::array<std::uint16_t,WORDS> instructions_{},data_{};
    std::array<bool,WORDS> instruction_valid_{},data_valid_{};
    std::array<std::uint16_t,IROM_WORDS> instruction_rom_{};
    std::array<std::uint16_t,COEFFICIENT_WORDS> coefficient_rom_{};
    bool instruction_rom_loaded_{},coefficient_rom_loaded_{};
    DSPInstructionRegisters registers_{};
    bool running_{};
    void Load(NativeDSPMemoryEndpoint endpoint,std::uint32_t physical_address,
              std::uint32_t bytes,std::uint16_t word_address,bool instruction);
    void LoadROM(NativeDSPMemoryEndpoint endpoint,std::uint32_t physical_address,bool instruction);
};
} // namespace mscharged::platform
