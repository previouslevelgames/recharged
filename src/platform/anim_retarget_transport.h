#pragma once
#include <cstddef>
#include <cstdint>
class nlChunk;
struct AnimRetarget;
namespace mscharged::platform {
struct NativeAnimRetargetView {
    nlChunk* source{};
    std::size_t source_bytes{};
    void* data{};
    std::size_t native_bytes{};
    std::uint64_t incarnation{};
};
// Only completed original serialized bytes with their retained source owner.
// Native backing shares that incarnation and grants no retained-reference lease.
NativeAnimRetargetView PrepareNativeAnimRetarget(nlChunk* source);
void* NativeAnimRetargetChunkData(const NativeAnimRetargetView&, nlChunk* child);
// Typed, current-incarnation projection of an already initialized record.
// Neither direction creates native backing or retains the original owner.
const void* NativeAnimRetargetRawRecord(const AnimRetarget* record);
const AnimRetarget* NativeAnimRetargetRecordFromRaw(const void* record);
}
