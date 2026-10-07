#pragma once
#include <cstddef>
#include <cstdint>

class nlChunk;
struct AudioResourceSource;
namespace mscharged::platform {
// Typed backing of completed original bank bytes. The original source retains
// chunk traversal, table assignment, rebasing order and load-owner requests.
struct NativeAudioBankView {
    const void* source;
    std::size_t source_bytes;
    void* data;
    std::size_t native_bytes;
    std::uint64_t incarnation;
};
NativeAudioBankView PrepareNativeAudioBankChunk(nlChunk* source);
void* NativeAudioBankData(const NativeAudioBankView& view,nlChunk* child);
struct NativeAudioBankRelocation {
    NativeAudioBankView view;
    std::uint32_t original_origin;
    AudioResourceSource* records;
    std::uint32_t count;
    bool already_native;
};
NativeAudioBankRelocation NativeAudioBankDelta(const NativeAudioBankView& view,
    AudioResourceSource* old_records,AudioResourceSource* records,std::uint32_t count);
AudioResourceSource* RelocateNativeAudioBankRecord(AudioResourceSource* original,
    const NativeAudioBankRelocation& relocation);
}
