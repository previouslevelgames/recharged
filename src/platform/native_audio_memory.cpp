#include "platform/native_audio_memory.h"
#include "platform/dsp_memory.h"
#include "NL/MemAlloc.h"
#include <new>
#include <stdexcept>

namespace mscharged::platform {
namespace {
struct AudioPin { NativeDSPMemoryPin pin{}; };
void ReleaseAudioPin(void* data) {
    auto& state = *static_cast<AudioPin*>(data);
    if (!state.pin.identity) return;
    ReleaseNativeDSPMemory(state.pin);
    state.pin = {};
}
}
NativeAudioMemoryReservation::NativeAudioMemoryReservation()
    : ownership_(sizeof(AudioPin), ReleaseAudioPin) {
    new(ownership_.Data()) AudioPin{};
}
void NativeAudioMemoryReservation::Commit(void* pointer, std::size_t bytes) {
    if (!pointer) return; // Keep the original null-allocation branch unchanged.
    // Prepare rejects preexisting/interior/stale owners before any hardware pin
    // is attempted. The original allocator has recorded this fresh allocation.
    ownership_.Prepare(pointer, bytes);
    GameAllocationSpan source{};
    if (!FindGameAllocationSpan(pointer, bytes, source) || source.base != pointer)
        throw std::invalid_argument("Audio device lease lost its fresh original allocation");
    auto& state = *static_cast<AudioPin*>(ownership_.Data());
    try {
        state.pin = PinNativeDSPMemory(pointer, bytes, false);
        ownership_.Commit();
    } catch (...) {
        ReleaseAudioPin(&state);
        // A host registration failure must not leak an otherwise successful
        // source request. This is the true recorded owner/free path.
        source.owner->Free(pointer);
        throw;
    }
}
}
