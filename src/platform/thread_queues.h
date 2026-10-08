#pragma once

#include <cstddef>
#include <cstdint>

// Wii SDK timed sleep, provided by the actual native alarm/thread endpoints.
extern "C" void OSSleepTicks(std::int64_t ticks);

namespace mscharged::platform {

using NativeThreadWaitService = void (*)();

// Host setup may install its actual interrupt-only hardware service on the
// calling native owner. A sleeping owner invokes it outside queue/mask locks;
// foreign waiters do not inherit that registration. Clear the borrowed service
// before its implementation or device storage is retired. No callback or source
// readiness is supplied when this slot is empty.
NativeThreadWaitService SetNativeThreadWaitService(NativeThreadWaitService service);

// Stable nonzero numeric alarm tag for this live native SDK thread incarnation.
// This is a cancellation identity, never a truncated pointer or device address.
// Requires an actual runnable caller with its borrowed hardware wait service.
std::uint32_t NativeThreadAlarmTag();

struct NativeThreadPowerRemovalStatus {
    std::size_t completed_workers{};
    std::size_t retained_moribund_threads{};
};

// Observation-only terminal power fence, under the original plain interrupt
// mask. Checks real native completion and borrowed wait/continuation state;
// source MORIBUND joinable descriptors and still-joinable host handles stay
// retained. It neither joins/detaches/cancels workers nor retires their images.
// The caller must keep the mask, descriptors, callbacks and arenas alive until
// actual process removal. Live, failed or servicing peers remain unsupported.
NativeThreadPowerRemovalStatus ValidateNativeThreadsForPowerRemoval();

// Borrowed SDK descriptors/callback images must remain alive until their actual
// workers have ended and attached source joins/detaches have completed. Host
// teardown retires real execution resources; active lifetimes fail explicitly.
void DrainNativeThreadLifetimes();

} // namespace mscharged::platform
