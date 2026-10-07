#pragma once
#include "platform/dsp_mailbox.h"
#include <cstdint>

namespace mscharged::platform {
struct NativeDSPControlEndpoint {std::uint64_t generation;};
struct NativeDSPControlStatus {
    bool connected;
    std::uint16_t csr;
    std::uint64_t generation;
};

// A halted, explicitly attached diagnostic hardware context. This does not
// supply reset defaults, a ROM or a successful DSP/AX initialization state.
NativeDSPControlEndpoint AttachNativeDSPControl(NativeDSPMailboxEndpoint mailboxes);
void DetachNativeDSPControl();
NativeDSPControlStatus GetNativeDSPControlStatus();

// Optional native processor owns the actual cold/reset/INIT/HALT lifecycle.
// The default diagnostic endpoint retains its original unsupported-bit failures.
// Validate runs before CSR mutation; Apply runs after actual CSR publication.
// Neither hook may call a source callback or write source task/global state.
struct NativeDSPProcessorControl {
    void (*validate)(void*, std::uint16_t previous, std::uint16_t request);
    void (*apply)(void*, std::uint16_t previous, std::uint16_t request);
    void* context;
};
void AttachNativeDSPProcessorControl(NativeDSPControlEndpoint endpoint,
                                     NativeDSPProcessorControl processor);
void DetachNativeDSPProcessorControl(NativeDSPControlEndpoint endpoint, void* context);

// Source-selected OS boot DMA. Begin exposes the actual transfer's busy bit;
// End retires it after the checked copy. Only completed ARAM copies latch its
// hardware cause. The caller retains real pins and the whole-job HALT fence.
void DSPBackendBeginBootTransfer(NativeDSPControlEndpoint endpoint,bool instruction);
void DSPBackendEndBootTransfer(NativeDSPControlEndpoint endpoint,bool instruction,bool completed);

// Exact observed IFX DIRQ values 0/1. Nonzero unknown bits remain unsupported.
// Requests latch DSPINT; source CPU acknowledgment is separate W1C hardware.
void DSPBackendWriteInterruptRequest(NativeDSPControlEndpoint endpoint,std::uint16_t value);
void DSPBackendRequireInstructionExecution(NativeDSPControlEndpoint endpoint,std::uint16_t dsp_status);
// The actual DSP HALT opcode sets the hardware CSR bit. It preserves causes,
// enables and other control state and invokes no CPU/source callback.
void DSPBackendHaltExecution(NativeDSPControlEndpoint endpoint);
} // namespace mscharged::platform
