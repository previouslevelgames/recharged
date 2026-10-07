#pragma once
#include <cstddef>
#include <cstdint>

class nlChunk;
struct AudioCalculationDefinition;

namespace mscharged::platform {
// Typed views of completed original data; source parser/constructor/update
// decisions remain in the actual AudioSlider and AudioCalculation TUs.
struct NativeAudioControlView {
    const void* source;
    std::size_t source_bytes;
    void* data;
    std::size_t native_bytes;
    std::uint64_t incarnation;
};
NativeAudioControlView PrepareNativeAudioSliderChunk(nlChunk* source);
NativeAudioControlView PrepareNativeAudioCalculationChunk(nlChunk* source);
void* NativeAudioControlData(const NativeAudioControlView& view, nlChunk* child);

struct NativeAudioCalculationRelocation {
    NativeAudioControlView view;
    std::uint32_t original_origin;
    std::uint32_t original_words;
    const std::int32_t* words;
    bool already_native;
};
NativeAudioCalculationRelocation NativeAudioCalculationDelta(
    const NativeAudioControlView& view, AudioCalculationDefinition* original,
    AudioCalculationDefinition* definitions, std::uint32_t count);
std::int32_t* RelocateNativeAudioCalculationWord(
    std::int32_t* original, const NativeAudioCalculationRelocation& delta);
}
