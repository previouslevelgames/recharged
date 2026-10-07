#pragma once
#include <cstddef>
#include <cstdint>
class nlChunk;
class cSAnim;
namespace mscharged::platform {
struct NativeSAnimView {
  nlChunk *source{};
  std::size_t source_bytes{};
  void *data{};
  std::size_t native_bytes{};
  std::uint64_t incarnation{};
};
NativeSAnimView PrepareNativeSAnim(nlChunk *);
NativeSAnimView NativeSAnimObjectView(const cSAnim *, nlChunk *sourceProbe);
void *NativeSAnimChunkData(const NativeSAnimView &, nlChunk *);
} // namespace mscharged::platform
