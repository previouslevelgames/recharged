#include "platform/native_audio_controls.h"
#include "platform/game_allocation_ownership.h"
#include "Game/Audio/AudioSlider.h"
#include "Game/Audio/AudioCalculation.h"
#include "NL/nlChunk.h"
#include <bit>
#include <cstring>
#include <limits>
#include <new>
#include <stdexcept>
#include <type_traits>

namespace mscharged::platform {
namespace {
struct Child {
    nlChunk* header;
    const unsigned char* data;
    std::size_t bytes;
    std::size_t native_offset;
};
struct Storage {
    std::uint32_t marker;
    std::uint32_t original_origin;
    std::uint32_t count;
    std::uint32_t child_count;
    std::size_t records_offset;
    std::size_t words_offset;
    Child children[3];
};
constexpr std::uint32_t SliderMarker = 0x534c4944u;
constexpr std::uint32_t CalculationMarker = 0x43414c43u;
static_assert(sizeof(AudioSliderTable) == 72 && alignof(AudioSliderTable) == 8);
static_assert(sizeof(AudioSliderDefinition) == 40 && alignof(AudioSliderDefinition) == 8);
static_assert(sizeof(AudioCalculationTable) == 24 && alignof(AudioCalculationTable) == 8);
static_assert(sizeof(AudioCalculationDefinition) == 40 && alignof(AudioCalculationDefinition) == 8);
static_assert(std::is_trivially_destructible_v<AudioSliderTable>);
static_assert(std::is_trivially_destructible_v<AudioCalculationTable>);

std::uint32_t Word(const unsigned char* p) {
    return std::uint32_t(p[0]) << 24 | std::uint32_t(p[1]) << 16
        | std::uint32_t(p[2]) << 8 | p[3];
}
float Float(const unsigned char* p) { return std::bit_cast<float>(Word(p)); }
template<class T> T* SavedWord(std::uint32_t word) {
    // Authored relocation/hash bits only; never a live device address.
    return reinterpret_cast<T*>(std::uintptr_t(word));
}
std::size_t Extend(std::size_t& cursor, std::size_t count, std::size_t stride) {
    if (cursor > std::numeric_limits<std::size_t>::max() - 7)
        throw std::overflow_error("Audio control native alignment overflow");
    cursor = (cursor + 7) & ~std::size_t(7);
    const auto at = cursor;
    if (count > (std::numeric_limits<std::size_t>::max() - cursor) / stride)
        throw std::overflow_error("Audio control native extent overflow");
    cursor += count * stride;
    return at;
}
const Storage& Validate(const NativeAudioControlView& view) {
    GameNativeBackingSpan backing{};
    if (!FindGameNativeBacking(view.source, view.source_bytes, backing)
        || backing.data != view.data || backing.bytes != view.native_bytes
        || backing.allocation.incarnation != view.incarnation)
        throw std::invalid_argument("Audio control has no live completed source incarnation");
    const auto& s = *static_cast<const Storage*>(view.data);
    if (s.marker != SliderMarker && s.marker != CalculationMarker)
        throw std::invalid_argument("Audio control attachment has another typed layout");
    return s;
}
struct RawSource {
    GameCompletedSpan completed;
    std::size_t bytes;
    Child children[3];
};
RawSource Inspect(nlChunk* source, unsigned count) {
    RawSource result{};
    if (!source || !FindGameCompletedSpan(source, 8, result.completed))
        throw std::invalid_argument("Audio control requires actual completed NL bytes");
    result.bytes = std::size_t(source->GetSize()) + 8;
    if (!FindGameCompletedSpan(source, result.bytes, result.completed)
        || FindGameByteDomain(source, result.bytes) != GameByteDomain::WiiSerialized)
        throw std::invalid_argument("Audio control requires one exact serialized source extent");
    auto* current = source->GetFirstChunk();
    auto* end = source->GetLastChunk();
    for (unsigned i = 0; i < count; ++i) {
        if (current >= end)
            throw std::out_of_range("Audio control required source child is absent");
        auto* next = current->GetNextChunk();
        if (next <= current || next > end)
            throw std::out_of_range("Audio control child leaves its original parent extent");
        const auto* data = static_cast<const unsigned char*>(current->GetData());
        const auto dataBytes = current->GetDataSize();
        const auto start = reinterpret_cast<std::uintptr_t>(current) + 8;
        const auto limit = reinterpret_cast<std::uintptr_t>(next);
        const auto address = reinterpret_cast<std::uintptr_t>(data);
        if (address < start || address > limit || dataBytes > limit - address
            || !FindGameCompletedSpan(data, dataBytes, result.completed))
            throw std::out_of_range("Audio control alignment/data span leaves actual source child");
        result.children[i] = {current, data, dataBytes, 0};
        current = next;
    }
    // The source requests only these children. Unrequested tail is unchanged.
    return result;
}
bool Existing(nlChunk* source, const RawSource& raw, std::uint32_t marker,
              NativeAudioControlView& view) {
    GameNativeBackingSpan backing{};
    if (!FindGameNativeBacking(source, raw.bytes, backing)) return false;
    view = {source, raw.bytes, backing.data, backing.bytes, backing.allocation.incarnation};
    if (Validate(view).marker != marker)
        throw std::invalid_argument("Audio control source has another native record profile");
    return true;
}
}

NativeAudioControlView PrepareNativeAudioSliderChunk(nlChunk* source) {
    auto raw = Inspect(source, 3);
    NativeAudioControlView existing{};
    if (Existing(source, raw, SliderMarker, existing)) return existing;
    const auto* h = raw.children[0].data;
    if (raw.children[0].bytes < 36)
        throw std::out_of_range("Original slider table header is incomplete");
    const auto global = Word(h + 8), local = Word(h + 16);
    const auto definitions = raw.children[1].bytes / 32;
    if (global > definitions || local > definitions - global)
        throw std::out_of_range("Original slider counts leave authored definitions");
    const auto indices = raw.children[2].bytes / 4;
    std::size_t bytes = sizeof(Storage);
    raw.children[0].native_offset = Extend(bytes, 1, sizeof(AudioSliderTable));
    raw.children[1].native_offset = Extend(bytes, definitions, sizeof(AudioSliderDefinition));
    raw.children[2].native_offset = Extend(bytes, indices, sizeof(std::uint32_t));
    GameNativeBackingReservation reservation(source, raw.bytes, bytes);
    auto* base = static_cast<unsigned char*>(reservation.Data());
    auto* s = new(base) Storage{SliderMarker, 0, static_cast<std::uint32_t>(definitions),
                              3, raw.children[1].native_offset, 0, {}};
    for (unsigned i = 0; i < 3; ++i) s->children[i] = raw.children[i];
    auto* table = new(base + s->children[0].native_offset) AudioSliderTable{};
    table->field_00 = Word(h);
    table->globalDefinitions = SavedWord<AudioSliderDefinition>(Word(h + 4));
    table->globalCount = global;
    table->globalDefinitionsCopy = SavedWord<AudioSliderDefinition>(Word(h + 12));
    table->localCount = local;
    table->localDefinitions = SavedWord<AudioSliderDefinition>(Word(h + 20));
    table->localToGlobal = SavedWord<std::uint32_t>(Word(h + 24));
    table->globalSliders = SavedWord<AudioSlider>(Word(h + 28));
    table->localSets = SavedWord<AudioSliderSet>(Word(h + 32));
    for (std::size_t i = 0; i < definitions; ++i) {
        const auto* p = raw.children[1].data + i * 32;
        auto* d = new(base + s->records_offset + i * sizeof(AudioSliderDefinition)) AudioSliderDefinition{};
        d->field_00 = Word(p);
        // Original callers pass name to nlLookupDebugString as a hash word;
        // ParseAudioSliderTable performs no string relocation or string load.
        d->name = SavedWord<const char>(Word(p + 4));
        d->initialValue = Float(p + 8);
        d->minimumValue = Float(p + 12);
        d->maximumValue = Float(p + 16);
        d->field_14 = Word(p + 20);
        d->kind = std::bit_cast<std::int32_t>(Word(p + 24));
        d->index = Word(p + 28);
    }
    auto* output = reinterpret_cast<std::uint32_t*>(base + s->children[2].native_offset);
    for (std::size_t i = 0; i < indices; ++i)
        new(output + i) std::uint32_t(Word(raw.children[2].data + i * 4));
    reservation.Commit();
    return {source, raw.bytes, base, bytes, raw.completed.allocation.incarnation};
}

NativeAudioControlView PrepareNativeAudioCalculationChunk(nlChunk* source) {
    auto raw = Inspect(source, 2);
    NativeAudioControlView existing{};
    if (Existing(source, raw, CalculationMarker, existing)) return existing;
    if (raw.children[0].bytes < 12)
        throw std::out_of_range("Original calculation table header is incomplete");
    const auto* h = raw.children[0].data;
    const auto count = Word(h), origin = Word(h + 4);
    const auto wireWords = raw.children[1].bytes / 4;
    if (count > raw.children[1].bytes / 24)
        throw std::out_of_range("Original calculation count leaves authored definitions");
    for (std::size_t i = 0; i < count; ++i) {
        const auto* p = raw.children[1].data + i * 24;
        for (unsigned field = 12; field <= 20; field += 4) {
            const auto old = Word(p + field);
            if (old) {
                const std::uint32_t offset = old - origin;
                if (offset % 4 || offset / 4 >= wireWords)
                    throw std::invalid_argument("Calculation saved word leaves its actual original definition data");
                if (offset % 24 >= 12)
                    throw std::invalid_argument("Calculation saved word targets an unqualified mutable pointer slot");
            }
        }
    }
    std::size_t bytes = sizeof(Storage);
    raw.children[0].native_offset = Extend(bytes, 1, sizeof(AudioCalculationTable));
    raw.children[1].native_offset = Extend(bytes, count, sizeof(AudioCalculationDefinition));
    const auto words = Extend(bytes, wireWords, sizeof(std::int32_t));
    GameNativeBackingReservation reservation(source, raw.bytes, bytes);
    auto* base = static_cast<unsigned char*>(reservation.Data());
    auto* s = new(base) Storage{CalculationMarker, origin, count, 2,
                              raw.children[1].native_offset, words, {}};
    for (unsigned i = 0; i < 2; ++i) s->children[i] = raw.children[i];
    auto* table = new(base + s->children[0].native_offset) AudioCalculationTable{};
    table->count = count;
    table->definitions = SavedWord<AudioCalculationDefinition>(origin);
    table->sliders = SavedWord<AudioCalculationSlider>(Word(h + 8));
    for (std::size_t i = 0; i < count; ++i) {
        const auto* p = raw.children[1].data + i * 24;
        auto* d = new(base + s->records_offset + i * sizeof(AudioCalculationDefinition)) AudioCalculationDefinition{};
        std::memcpy(d->pad_00, p, 8);
        d->initialValue = Float(p + 8);
        d->field_0C = SavedWord<std::int32_t>(Word(p + 12));
        d->field_10 = SavedWord<std::int32_t>(Word(p + 16));
        d->field_14 = SavedWord<std::int32_t>(Word(p + 20));
    }
    // Qualified source s32* fields address numeric words inside the wire
    // records (including their opaque first8bytes), not pointer slots. Keep
    // exact word offsets in separately typed endian-correct storage. An alias
    // of a pointer slot would observe source relocation writes and is held.
    auto* output = reinterpret_cast<std::int32_t*>(base + words);
    for (std::size_t i = 0; i < wireWords; ++i)
        new(output + i) std::int32_t(std::bit_cast<std::int32_t>(Word(raw.children[1].data + i * 4)));
    reservation.Commit();
    return {source, raw.bytes, base, bytes, raw.completed.allocation.incarnation};
}

void* NativeAudioControlData(const NativeAudioControlView& view, nlChunk* child) {
    const auto& s = Validate(view);
    for (unsigned i = 0; i < s.child_count; ++i)
        if (s.children[i].header == child)
            return static_cast<unsigned char*>(view.data) + s.children[i].native_offset;
    throw std::invalid_argument("Audio control child is outside its actual typed image");
}

NativeAudioCalculationRelocation NativeAudioCalculationDelta(
    const NativeAudioControlView& view, AudioCalculationDefinition* original,
    AudioCalculationDefinition* definitions, std::uint32_t count) {
    const auto& s = Validate(view);
    if (s.marker != CalculationMarker || count != s.count
        || definitions != reinterpret_cast<AudioCalculationDefinition*>(
            static_cast<unsigned char*>(view.data) + s.records_offset))
        throw std::invalid_argument("Calculation rebasing lost its exact definition extent");
    const auto* words = reinterpret_cast<const std::int32_t*>(
        static_cast<unsigned char*>(view.data) + s.words_offset);
    const auto wordCount = static_cast<std::uint32_t>(s.children[1].bytes / 4);
    if (original == definitions) return {view, s.original_origin, wordCount, words, true};
    if (reinterpret_cast<std::uintptr_t>(original) != s.original_origin)
        throw std::invalid_argument("Calculation saved origin differs from authored word");
    return {view, s.original_origin, wordCount, words, false};
}

std::int32_t* RelocateNativeAudioCalculationWord(
    std::int32_t* original, const NativeAudioCalculationRelocation& delta) {
    (void)Validate(delta.view);
    const auto address = reinterpret_cast<std::uintptr_t>(original);
    if (delta.already_native) {
        const auto start = reinterpret_cast<std::uintptr_t>(delta.words);
        if (address < start || (address - start) % 4
            || (address - start) / 4 >= delta.original_words)
            throw std::out_of_range("Repeated calculation word leaves its native typed span");
        return original;
    }
    if (address > std::numeric_limits<std::uint32_t>::max())
        throw std::invalid_argument("Calculation saved word has an unsupported width");
    const std::uint32_t offset = std::uint32_t(address) - delta.original_origin;
    if (offset % 4 || offset / 4 >= delta.original_words)
        throw std::out_of_range("Calculation word rebasing leaves its original source extent");
    return const_cast<std::int32_t*>(delta.words) + offset / 4;
}
}
