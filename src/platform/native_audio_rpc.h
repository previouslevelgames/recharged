#pragma once
#include <cstddef>
#include <cstdint>

class nlChunk;

namespace mscharged::platform {
// Allocation-owned POD projection only. The original RPC parser initializes
// its pointers, runtime pools and lists and remains the sole controller owner.
struct NativeAudioRpcView {
    nlChunk* source{};
    std::size_t source_bytes{};
    void* data{};
    std::size_t native_bytes{};
    std::uint64_t incarnation{};
};
NativeAudioRpcView PrepareNativeAudioRpc(nlChunk* source);
void* NativeAudioRpcChildData(const NativeAudioRpcView& view, nlChunk* child);
}
