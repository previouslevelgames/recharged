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

// Exact observed IFX DIRQ values 0/1. Nonzero unknown bits remain unsupported.
// Requests latch DSPINT; source CPU acknowledgment is separate W1C hardware.
void DSPBackendWriteInterruptRequest(NativeDSPControlEndpoint endpoint,std::uint16_t value);
void DSPBackendRequireInstructionExecution(NativeDSPControlEndpoint endpoint,std::uint16_t dsp_status);
} // namespace mscharged::platform
