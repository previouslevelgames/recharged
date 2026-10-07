#pragma once
#include <cstddef>
#include <cstdint>

class nlChunk;
namespace mscharged::platform {
// Source-owned Wii bytes stay raw. The actual parsers choose/traverse the
// chunks and assign/rebase every original field in these attached POD views.
struct NativeAudioResourceView {
    const void* source;
    std::size_t source_bytes;
    void* data;
    std::size_t native_bytes;
    std::uint64_t incarnation;
};
NativeAudioResourceView PrepareNativeSoundMapChunk(nlChunk* source);
NativeAudioResourceView PrepareNativeAudioResourceChunk(nlChunk* source);
NativeAudioResourceView PrepareNativeAudioSourceChunk(nlChunk* source);
void* NativeAudioResourceData(const NativeAudioResourceView& view, nlChunk* child);

enum class AudioResourceArray { Voice, Sequence, Sound, HitMarker, Parameter };
struct NativeAudioResourceRelocation {
    NativeAudioResourceView view;
    AudioResourceArray array;
    bool already_native;
};
NativeAudioResourceRelocation NativeAudioResourceOrigin(
    const NativeAudioResourceView& view, AudioResourceArray array,
    const void* original);
NativeAudioResourceRelocation NativeAudioResourceDelta(
    const NativeAudioResourceRelocation& original, const void* records);
void* RelocateNativeAudioResourceRecord(
    const void* original, const NativeAudioResourceRelocation& relocation);
}
