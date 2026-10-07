#pragma once
#include <cstddef>
#include <cstdint>

namespace mscharged::platform {
struct NativeDSPMemoryEndpoint { std::uint64_t generation; };
struct NativeDSPMemoryPin { std::uint64_t generation, identity; };

// Native CPU fields retain their original arithmetic representation. Only device
// transfers project those fields to the big-endian DSP bus. Raw-byte pins retain
// the existing unchanged-byte contract (firmware/raw sample bytes, opaque ITD).
enum class NativeDSPMemoryEncoding {
    RawBytes,
    NativeU16,
    NativeU32,
    AXParameterBlocks, // 320 bytes: u32 mixerCtrl, u16 fields, opaque tail.
    AXStudio          // 120 bytes: twenty packed s32/s16 depop pairs.
};

// SDK must own live MEM1 throughout this attached endpoint. Pins do not take
// source/game ownership: release after draining device access, before freeing
// the source backing, and detach before the actual SDK arena shutdown.
NativeDSPMemoryEndpoint AttachNativeDSPMEM1();
void DetachNativeDSPMEM1();
NativeDSPMemoryPin PinNativeDSPMEM1(const void* address, std::size_t bytes, bool writable);
// Explicit extension to actual SDK MEM2 and registered static reservations.
// A static owner is retained by the shared SDK until all device pins drain.
NativeDSPMemoryPin PinNativeDSPMemory(const void* address, std::size_t bytes, bool writable);
NativeDSPMemoryPin PinNativeDSPMemory(const void* address, std::size_t bytes, bool writable,
                                   NativeDSPMemoryEncoding encoding);
void ReleaseNativeDSPMemory(NativeDSPMemoryPin pin);

// Checked copy operations keep the ownership lock through each transfer. No
// raw pointer escapes to a device worker. A typed pin applies its declared DSP
// byte order, including partial-field writes; it does not take source ownership
// or synchronize direct CPU writes with a running device. The caller must
// preserve the original request/completion/cache synchronization boundary.
// Validation makes no device write or reservation. It is not a job lease:
// source pins must remain live and drained by the caller for a whole device job.
void DSPBackendValidateMemory(NativeDSPMemoryEndpoint endpoint, std::uint32_t physical_address,
                              std::size_t bytes, bool writing);
void DSPBackendReadMemory(NativeDSPMemoryEndpoint endpoint, std::uint32_t physical_address,
                          void* destination, std::size_t bytes);
void DSPBackendWriteMemory(NativeDSPMemoryEndpoint endpoint, std::uint32_t physical_address,
                           const void* source, std::size_t bytes);
} // namespace mscharged::platform
