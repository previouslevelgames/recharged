#pragma once
#include <cstdint>

class nlChunk;
struct SPSoundTable;
namespace mscharged::platform {
// Actual original source allocation owns the raw Wii table and its native POD
// backing. SPInitSoundTable/SPPrepareSound keep every original decision/write.
SPSoundTable* PrepareNativeSPSoundTable(nlChunk* chunk);
// Checked live pinned byte span -> original cached Wii word. These do not
// register, own, initialize or manufacture a DSP/sample/backend endpoint.
std::uint32_t NativeAudioCachedWord(const void* data, std::uint32_t bytes);
std::uint32_t NativeAudioSampleCachedWord(const void* data, std::uint32_t bytes);
}
