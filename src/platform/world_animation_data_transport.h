#pragma once

#include <cstddef>
#include <cstdint>

namespace mscharged::platform {
// Numeric parent arrays beneath original WorldAnimObject::Initialize. No world
// object, vtable, controller, source manager or game readiness is represented.
// The caller retains the actual parent-file lifetime through every access.
struct NativeWorldAnimParentView {
    const void* source{};
    std::size_t source_bytes{};
    void* backing{};
    std::size_t native_bytes{};
    void* data{};
    std::uint64_t incarnation{};
};
NativeWorldAnimParentView PrepareNativeWorldAnimParent(
    const void* originalParentData, int bindings, int animations);
void* NativeWorldAnimParentData(const NativeWorldAnimParentView&);
}
