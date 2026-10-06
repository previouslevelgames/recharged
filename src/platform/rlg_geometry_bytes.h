#pragma once
#include <cstddef>
namespace mscharged::platform
{
// Preserve opaque Wii GPU vertex bytes and publish their real completed source
// copy. Index cells are numeric u16 consumed by original CPU/display-list code.
void CopyRLGWireVertices(void* output, const void* source, std::size_t bytes);
void CopyRLGNativeIndices(void* output, const void* source, std::size_t bytes);
}
