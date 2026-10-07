#pragma once
#include <cstddef>
#include <cstdint>

class nlChunk;

namespace mscharged::platform {
// A native representation of the original allocation-owned hierarchy bytes.
// Original Initialize still performs its chunk walk, pointer assignments and
// BuildPushPopFlags. This view contains no loaded/completed game state.
struct NativeHierarchyView {
    nlChunk* source{};
    std::size_t source_bytes{};
    void* data{};
    std::size_t native_bytes{};
    std::uint64_t incarnation{};
};
NativeHierarchyView PrepareNativeHierarchy(nlChunk* source);
void* NativeHierarchyChunkData(const NativeHierarchyView& view, nlChunk* child);
}
