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

// Native processor service at actual CPU mailbox register boundaries. Runs
// only on the attached owner after the register lock is released. Device workers
// still cannot run original callbacks; reentrant service is the sink's duty.
using NativeDSPMailboxService = void (*)(void*);
void AttachNativeDSPMailboxService(NativeDSPMailboxEndpoint endpoint,
                                    NativeDSPMailboxService service, void* context);
void DetachNativeDSPMailboxService(NativeDSPMailboxEndpoint endpoint, void* context);
// A halted/drained real processor resets only its hardware mail cells.
void DSPBackendResetMailboxes(NativeDSPMailboxEndpoint endpoint);

// Actual device endpoint: low reads acknowledge; low writes publish. Device
// firmware separately requests/acknowledges its interrupt, as on the hardware.
std::uint16_t DSPBackendMailToHigh(NativeDSPMailboxEndpoint endpoint);
std::uint16_t DSPBackendMailToLow(NativeDSPMailboxEndpoint endpoint);
// DSP-side read of its own outgoing high/status cell. It neither acknowledges
// the mail nor calls CPU service/handlers; the CPU low read owns acknowledgment.
std::uint16_t DSPBackendMailFromHigh(NativeDSPMailboxEndpoint endpoint);
void DSPBackendMailFromWriteHigh(NativeDSPMailboxEndpoint endpoint,std::uint16_t value);
void DSPBackendMailFromWriteLow(NativeDSPMailboxEndpoint endpoint,std::uint16_t value);
bool DSPBackendSetInterrupt(NativeDSPMailboxEndpoint endpoint,bool asserted);
} // namespace mscharged::platform
