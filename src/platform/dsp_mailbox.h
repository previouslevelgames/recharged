#pragma once
#include <cstdint>

namespace mscharged::platform {
struct NativeDSPMailboxEndpoint { std::uint64_t generation; };
struct NativeDSPMailboxStatus {
    bool connected;
    bool cpu_mail_full;
    bool dsp_mail_full;
    std::uint64_t generation;
};

// Attach only the wire transport. It does not boot a DSP, execute firmware,
// synthesize a handshake, install an AX callback or report engine readiness.
NativeDSPMailboxEndpoint AttachNativeDSPMailboxes();
void DetachNativeDSPMailboxes();
NativeDSPMailboxStatus GetNativeDSPMailboxStatus();

// Actual device endpoint: low reads acknowledge; low writes publish. Device
// firmware separately requests/acknowledges its interrupt, as on the hardware.
std::uint16_t DSPBackendMailToHigh(NativeDSPMailboxEndpoint endpoint);
std::uint16_t DSPBackendMailToLow(NativeDSPMailboxEndpoint endpoint);
void DSPBackendMailFromWriteHigh(NativeDSPMailboxEndpoint endpoint,std::uint16_t value);
void DSPBackendMailFromWriteLow(NativeDSPMailboxEndpoint endpoint,std::uint16_t value);
bool DSPBackendSetInterrupt(NativeDSPMailboxEndpoint endpoint,bool asserted);
} // namespace mscharged::platform
