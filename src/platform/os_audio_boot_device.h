#pragma once
#include "platform/dsp_instruction_core.h"
#include "platform/dsp_boot_memory.h"
#include <cstdint>
#include <memory>

namespace mscharged::platform {
// Explicit GPIO latch input. No BIOS/reset values or audio-ready state are
// inferred. Generated-bank qualifiers supply these cells as fixture input.
struct NativeOSAudioGPIO {std::uint32_t diflags,direction,input;};
enum class NativeOSAudioBootPhase {Cold,ImageLoaded,Executing,Halted,Faulted,Retired};
struct NativeOSAudioBootStatus {
    NativeOSAudioBootPhase phase;
    std::uint64_t reset_uploads,aram_transfers,executed_instructions,gpio_writes;
    NativeOSAudioGPIO gpio;
    std::uint16_t aram_size;
};
// One chip is borrowed throughout original OS boot and subsequent AX loading.
// Source decides every register request, wait, memory copy and mail ack. This
// endpoint routes only its bounded boot hardware requests; no game flags or
// callbacks are supplied. Authentic ROM and the first register image remain
// explicit inputs. A generated image/register fixture is not retail readiness.
class NativeOSAudioBootDevice {
public:
    NativeOSAudioBootDevice(NativeDSPMemoryEndpoint memory,NativeDSPMailboxEndpoint mailboxes,
                            NativeDSPControlEndpoint control,DSPInstructionCore& chip,
                            NativeOSAudioGPIO gpio,const DSPInstructionRegisters& boot_entry);
    ~NativeOSAudioBootDevice();
    NativeOSAudioBootDevice(const NativeOSAudioBootDevice&)=delete;
    NativeOSAudioBootDevice& operator=(const NativeOSAudioBootDevice&)=delete;
    std::uint16_t ReadDSP(std::uint32_t reg);
    void WriteDSP(std::uint32_t reg,std::uint16_t value);
    std::uint32_t ReadDSPPair(std::uint32_t high_reg);
    void WriteDSPPair(std::uint32_t high_reg,std::uint32_t value);
    std::uint32_t ReadIPC(std::uint32_t reg);
    void WriteIPC(std::uint32_t reg,std::uint32_t value);
    void* WorkMemory() const;
    NativeOSAudioBootStatus Status() const;
    // CPU must have issued actual HALT and drained all device calls. Keeps the
    // borrowed core, RAM/ROM validity, mailbox/control generations and pins.
    void Close();
private:
    struct State;
    std::unique_ptr<State> state_;
};
} // namespace mscharged::platform
