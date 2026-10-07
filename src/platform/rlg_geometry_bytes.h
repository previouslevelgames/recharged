#pragma once
#include <cstddef>
namespace mscharged::platform
{
// Preserve opaque Wii GPU vertex bytes and publish their real completed source
// copy. Index cells are numeric u16 consumed by original CPU/display-list code.
void CopyRLGWireVertices(void* output, const void* source, std::size_t bytes);
void CopyRLGNativeIndices(void* output, const void* source, std::size_t bytes);
// Original GLMatrix records are sixteen numeric float words consumed by the CPU.
// The caller retains its original 64-byte size mask and owning allocation.
void CopyRLGNativeMatrices(void* output, const void* source, std::size_t bytes);
}
