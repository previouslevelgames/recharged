#pragma once
#include <cstddef>
#include <cstdint>

class nlMatrix4;
class nlVector3;
struct MorphDelta;

namespace mscharged::platform
{
// Preserve opaque Wii GPU vertex bytes and publish their real completed source
// copy. Index cells are numeric u16 consumed by original CPU/display-list code.
void CopyRLGWireVertices(void* output, const void* source, std::size_t bytes);
void CopyRLGWireSkin(void* output, const void* source, std::size_t bytes);
void CopyRLGNativeIndices(void* output, const void* source, std::size_t bytes);
// Original GLMatrix records are sixteen numeric float words consumed by the CPU.
// The caller retains its original 64-byte size mask and owning allocation.
void CopyRLGNativeMatrices(void* output, const void* source, std::size_t bytes);

// The original skin factory keeps its chunk walk, counts and allocations.
// Its serialized IDs/counts are Wii32 and bind matrices contain sixteen
// big-endian float words. Morph records remain borrowed from their raw owner.
std::uint32_t ReadRLGSkinWord(const void* source);
void ReadRLGSkinMatrix(nlMatrix4& output, const void* source);
const MorphDelta* ReadRLGSkinMorphDeltas(const void* source,
    std::size_t count, std::size_t sourceStride);

// Original software skin consumes numeric Float3 rows while its source GPU
// streams retain their authored byte order. Caller retains the source owner
// throughout this scoped CPU read; no stream address or raw bytes are changed.
class NativeRLGFloat3Rows
{
public:
    NativeRLGFloat3Rows(const void* source, std::size_t vertices, std::size_t sourceStride);
    ~NativeRLGFloat3Rows();
    NativeRLGFloat3Rows(const NativeRLGFloat3Rows&) = delete;
    NativeRLGFloat3Rows& operator=(const NativeRLGFloat3Rows&) = delete;
    const nlVector3* Data() const;
private:
    void* state_;
    const void* source_;
};

// Original CPU readers that index authored GPU vertex streams directly (the
// goal-net loader keeps pointers into position rows) read this numeric twin of
// the same completed stream: identical extent, stride and offsets, with every
// cellBytes-wide scalar in native order. The twin is released with its source
// incarnation and the GPU stream bytes are unchanged. Native-produced streams
// and single-byte cells are returned unchanged.
const void* ReadRLGNumericStream(const void* source, std::size_t bytes, std::size_t cellBytes);

enum class RLGWeightAccess { ReadOnly, ReadWrite };
// Only original CPU float consumers use this scoped row view. The original
// loops still compare and swap; StoreWire encodes their result back into the
// same owned Wii vertex bytes, without converting the GPU stream or stride.
class NativeRLGWeightRows
{
public:
    NativeRLGWeightRows(const void* source, std::size_t vertices, RLGWeightAccess access);
    ~NativeRLGWeightRows();
    NativeRLGWeightRows(const NativeRLGWeightRows&) = delete;
    NativeRLGWeightRows& operator=(const NativeRLGWeightRows&) = delete;
    float* Data() const noexcept;
    void StoreWire();
private:
    void* state_;
};
}
