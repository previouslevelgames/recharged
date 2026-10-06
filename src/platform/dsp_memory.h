#pragma once
#include <cstddef>
#include <cstdint>

namespace mscharged::platform {
struct NativeDSPMemoryEndpoint { std::uint64_t generation; };
struct NativeDSPMemoryPin { std::uint64_t generation, identity; };

// SDK must own live MEM1 throughout this attached endpoint. Pins do not take
// source/game ownership: release after draining device access, before freeing
// the source backing, and detach before the actual SDK arena shutdown.
NativeDSPMemoryEndpoint AttachNativeDSPMEM1();
void DetachNativeDSPMEM1();
NativeDSPMemoryPin PinNativeDSPMEM1(const void* address, std::size_t bytes, bool writable);
// Explicit extension to actual SDK MEM2 and registered static reservations.
// A static owner is retained by the shared SDK until all device pins drain.
NativeDSPMemoryPin PinNativeDSPMemory(const void* address, std::size_t bytes, bool writable);
void ReleaseNativeDSPMemory(NativeDSPMemoryPin pin);

// Checked copy operations keep the ownership lock through each transfer. No
// raw pointer escapes to a device worker; byte order is transported unchanged.
void DSPBackendReadMemory(NativeDSPMemoryEndpoint endpoint, std::uint32_t physical_address,
                          void* destination, std::size_t bytes);
void DSPBackendWriteMemory(NativeDSPMemoryEndpoint endpoint, std::uint32_t physical_address,
                           const void* source, std::size_t bytes);
} // namespace mscharged::platform
