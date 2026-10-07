#pragma once
#include "platform/game_allocation_ownership.h"
#include <cstddef>

namespace mscharged::platform {
// A hardware RawBytes lease, attached to the actual freshly allocated original
// buffer. Construction reserves metadata before the original allocator mutates;
// Commit pins its genuine source extent or rolls that new allocation back.
// No audio initialization, sample read, callback or readiness is represented.
class NativeAudioMemoryReservation {
public:
    NativeAudioMemoryReservation();
    NativeAudioMemoryReservation(const NativeAudioMemoryReservation&) = delete;
    NativeAudioMemoryReservation& operator=(const NativeAudioMemoryReservation&) = delete;
    void Commit(void* pointer, std::size_t bytes);
private:
    GameAllocationDeviceReservation ownership_;
};
}
