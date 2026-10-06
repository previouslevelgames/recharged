#pragma once

#include "Game/Replay.h"
#include <cstdint>
#include <cstring>
#include <type_traits>

namespace mscharged::platform {
// Actual native address bits only. This does not create an object, own its
// lifetime, relocate Wii/persisted replay words or invent callback readiness.
// Original Save/Load requests and stream advance still run in the source POD API.
template <int Interval, typename Frame, typename Pointer>
void ReplayNativeAddress(Frame& frame, Pointer& pointer) {
    static_assert(std::is_pointer_v<Pointer>);
    static_assert(sizeof(Pointer) == sizeof(std::uintptr_t));
    std::uintptr_t word;
    std::memcpy(&word, &pointer, sizeof(word));
    frame.template Replayable<Interval>(word, ReplayablePod{});
    std::memcpy(&pointer, &word, sizeof(word));
}
}
