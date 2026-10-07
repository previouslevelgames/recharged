#pragma once

namespace mscharged::platform {

using NativeThreadWaitService = void (*)();

// Host setup may install its actual interrupt-only hardware service on the
// calling native owner. A sleeping owner invokes it outside queue/mask locks;
// foreign waiters do not inherit that registration. Clear the borrowed service
// before its implementation or device storage is retired. No callback or source
// readiness is supplied when this slot is empty.
NativeThreadWaitService SetNativeThreadWaitService(NativeThreadWaitService service);

// Borrowed SDK descriptors/callback images must remain alive until their actual
// workers have ended and attached source joins/detaches have completed. Host
// teardown retires real execution resources; active lifetimes fail explicitly.
void DrainNativeThreadLifetimes();

} // namespace mscharged::platform
