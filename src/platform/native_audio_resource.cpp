#include "platform/native_audio_resource.h"
#include "platform/game_allocation_ownership.h"
#include "platform/host_metadata.h"
#include "Game/Audio/AudioResourceBundle.h"
#include "Game/Audio/AudioSequenceEvent.h"
#include "Game/Audio/AudioSource.h"
#include "Game/Audio/SoundMap.h"
#include "NL/nlChunk.h"

#include <bit>
#include <cstring>
#include <limits>
#include <new>
#include <stdexcept>
#include <type_traits>
#include <vector>

namespace mscharged::platform {
namespace {
template<class T> struct MetadataAllocator {
    using value_type = T;
    MetadataAllocator() noexcept = default;
    template<class U> MetadataAllocator(const MetadataAllocator<U>&) noexcept {}
    T* allocate(std::size_t n) {
        if (n > std::numeric_limits<std::size_t>::max() / sizeof(T)) throw std::bad_alloc();
        auto* p = ChargedNativeMetadataAllocate(n * sizeof(T));
        if (!p) throw std::bad_alloc();
        return static_cast<T*>(p);
    }
    void deallocate(T* p, std::size_t) noexcept { ChargedNativeMetadataRelease(p); }
    template<class U> bool operator==(const MetadataAllocator<U>&) const noexcept { return true; }
};
struct Child {
    nlChunk* header;
    const unsigned char* data;
    std::size_t bytes;
    std::size_t native_offset;
};
struct Array {
    std::uint32_t origin;
    std::uint32_t count;
    std::size_t offset;
    std::size_t wire_stride;
    std::size_t native_stride;
};
struct Storage {
    std::uint32_t marker;
    std::uint32_t children;
    Array arrays[5];
};
constexpr std::uint32_t MapMarker = 0x414d4150;
constexpr std::uint32_t ResourceMarker = 0x41524553;
constexpr std::uint32_t SourceMarker = 0x41535243;
using Children = std::vector<Child, MetadataAllocator<Child>>;
static_assert(std::is_trivially_destructible_v<AudioResourceBundle>);
static_assert(std::is_trivially_destructible_v<AudioCueDefinition>);
static_assert(std::is_trivially_destructible_v<AudioSourceInfo>);

std::uint32_t Word(const unsigned char* p) {
    return std::uint32_t(p[0]) << 24 | std::uint32_t(p[1]) << 16
         | std::uint32_t(p[2]) << 8 | p[3];
}
float Float(const unsigned char* p) { return std::bit_cast<float>(Word(p)); }
template<class T> T* Saved(std::uint32_t word) {
    // Serialized origins and name hashes, not live MEM1/MEM2/device addresses.
    return reinterpret_cast<T*>(std::uintptr_t(word));
}
std::size_t Extend(std::size_t& cursor, std::size_t count, std::size_t stride) {
    if (cursor > std::numeric_limits<std::size_t>::max() - 7)
        throw std::overflow_error("Audio resource native alignment overflow");
    cursor = (cursor + 7) & ~std::size_t(7);
    const auto at = cursor;
    if (count > (std::numeric_limits<std::size_t>::max() - cursor) / stride)
        throw std::overflow_error("Audio resource native extent overflow");
    cursor += count * stride;
    return at;
}
void Require(const Child& child, std::size_t count, std::size_t stride) {
    if (count > child.bytes / stride)
        throw std::out_of_range("Original audio fields leave their completed source child");
}
struct Raw {
    nlChunk* source;
    std::size_t bytes;
    GameCompletedSpan completed;
    Children children;
    nlChunk* next;
    nlChunk* end;

    explicit Raw(nlChunk* chunk) : source(chunk) {
        if (!chunk || !FindGameCompletedSpan(chunk, 8, completed))
            throw std::invalid_argument("Audio resource requires original completed NL bytes");
        bytes = std::size_t(chunk->GetSize()) + 8;
        if (!FindGameCompletedSpan(chunk, bytes, completed)
            || FindGameByteDomain(chunk, bytes) != GameByteDomain::WiiSerialized)
            throw std::invalid_argument("Audio resource requires one exact serialized source span");
        next = chunk->GetFirstChunk(); end = chunk->GetLastChunk();
    }
    void Append() {
        const auto here = reinterpret_cast<std::uintptr_t>(next);
        const auto limit = reinterpret_cast<std::uintptr_t>(end);
        if (here >= limit) throw std::out_of_range("Original required audio child is absent");
        auto* following = next->GetNextChunk();
        const auto after = reinterpret_cast<std::uintptr_t>(following);
        if (after <= here || after > limit)
            throw std::out_of_range("Original audio child leaves its actual parent");
        auto* data = static_cast<const unsigned char*>(next->GetData());
        const auto dataBytes = std::size_t(next->GetDataSize());
        const auto address = reinterpret_cast<std::uintptr_t>(data);
        if (address < here + 8 || address > after || dataBytes > after - address
            || (dataBytes && !FindGameCompletedSpan(data, dataBytes, completed)))
            throw std::out_of_range("Audio alignment/data leaves its completed child");
        children.push_back({next, data, dataBytes, 0}); next = following;
    }
    void Gather(std::size_t count) {
        if (count > bytes / 8)
            throw std::out_of_range("Audio required child count leaves its actual parent");
        while (children.size() < count) Append();
        // Do not inspect unrequested tail headers/bytes or impose new readiness.
    }
};
const Storage& Validate(const NativeAudioResourceView& view) {
    GameNativeBackingSpan backing{};
    if (!FindGameNativeBacking(view.source, view.source_bytes, backing)
        || backing.data != view.data || backing.bytes != view.native_bytes
        || backing.allocation.incarnation != view.incarnation)
        throw std::invalid_argument("Audio resource backing has no live original allocation incarnation");
    const auto& s = *static_cast<const Storage*>(view.data);
    if (s.marker != MapMarker && s.marker != ResourceMarker && s.marker != SourceMarker)
        throw std::invalid_argument("Audio resource backing has another typed layout");
    return s;
}
bool Existing(const Raw& raw, std::uint32_t marker, NativeAudioResourceView& view) {
    GameNativeBackingSpan backing{};
    if (!FindGameNativeBacking(raw.source, raw.bytes, backing)) return false;
    view = {raw.source, raw.bytes, backing.data, backing.bytes, backing.allocation.incarnation};
    if (Validate(view).marker != marker)
        throw std::invalid_argument("Audio source range has another native profile");
    return true;
}
std::size_t Start(const Raw& raw) {
    if (raw.children.size() > (std::numeric_limits<std::size_t>::max() - sizeof(Storage)) / sizeof(Child))
        throw std::overflow_error("Audio typed child descriptors overflow");
    return sizeof(Storage) + raw.children.size() * sizeof(Child);
}
Storage* Begin(unsigned char* base, const Raw& raw, std::uint32_t marker) {
    auto* s = new(base) Storage{};
    s->marker = marker; s->children = static_cast<std::uint32_t>(raw.children.size());
    auto* children = reinterpret_cast<Child*>(s + 1);
    for (std::size_t i = 0; i < raw.children.size(); ++i) new(children + i) Child(raw.children[i]);
    return s;
}
const Array& GetArray(const Storage& s, AudioResourceArray array) {
    if (s.marker != ResourceMarker || unsigned(array) >= 5)
        throw std::invalid_argument("Audio relocation requires its original resource profile");
    return s.arrays[unsigned(array)];
}
void CheckSaved(std::uint32_t word, const Array& a) {
    const std::uint32_t offset = word - a.origin;
    if (offset % a.wire_stride || offset / a.wire_stride >= a.count)
        throw std::invalid_argument("Authored audio address leaves its original record array");
}
}

NativeAudioResourceView PrepareNativeSoundMapChunk(nlChunk* source) {
    Raw raw(source); NativeAudioResourceView existing{};
    if (Existing(raw, MapMarker, existing)) return existing;
    raw.Gather(2); Require(raw.children[0], 1, 12);
    const auto* h = raw.children[0].data; const auto count = Word(h);
    Require(raw.children[1], count, 20);
    auto bytes = Start(raw);
    raw.children[0].native_offset = Extend(bytes, 1, sizeof(SoundMap));
    raw.children[1].native_offset = Extend(bytes, count, sizeof(SoundCue));
    GameNativeBackingReservation reservation(source, raw.bytes, bytes);
    auto* base = static_cast<unsigned char*>(reservation.Data()); Begin(base, raw, MapMarker);
    auto* map = new(base + raw.children[0].native_offset) SoundMap{};
    map->m_CueCount = count; map->m_Cues = Saved<SoundCue>(Word(h + 4));
    map->m_CueTree = Saved<SoundCueTree>(Word(h + 8));
    for (std::size_t i = 0; i < count; ++i) {
        const auto* r = raw.children[1].data + i * 20;
        auto* cue = new(base + raw.children[1].native_offset + i * sizeof(SoundCue)) SoundCue{};
        cue->field_0x0 = Word(r); cue->field_0x4 = Word(r + 4);
        cue->field_0x8 = Word(r + 8); cue->field_0xC = Word(r + 12); cue->field_0x10 = Word(r + 16);
    }
    reservation.Commit();
    return {source, raw.bytes, base, bytes, raw.completed.allocation.incarnation};
}

NativeAudioResourceView PrepareNativeAudioSourceChunk(nlChunk* source) {
    Raw raw(source); NativeAudioResourceView existing{};
    if (Existing(raw, SourceMarker, existing)) return existing;
    raw.Gather(2); Require(raw.children[0], 9, 1);
    const auto* h = raw.children[0].data; const auto count = Word(h);
    Require(raw.children[1], count, 28);
    auto bytes = Start(raw);
    raw.children[0].native_offset = Extend(bytes, 1, sizeof(AudioSourceData));
    raw.children[1].native_offset = Extend(bytes, count, sizeof(AudioSourceInfo));
    GameNativeBackingReservation reservation(source, raw.bytes, bytes);
    auto* base = static_cast<unsigned char*>(reservation.Data()); Begin(base, raw, SourceMarker);
    auto* table = new(base + raw.children[0].native_offset) AudioSourceData{};
    table->m_SourceCount = count; table->m_StreamBlockSize = Word(h + 4); table->m_IsStream = h[8];
    if (raw.children[0].bytes >= 12) std::memcpy(reinterpret_cast<unsigned char*>(table) + 9, h + 9, 3);
    for (std::size_t i = 0; i < count; ++i) {
        const auto* r = raw.children[1].data + i * 28;
        auto* info = new(base + raw.children[1].native_offset + i * sizeof(AudioSourceInfo)) AudioSourceInfo{};
        info->m_SoundIndex = Word(r); info->m_StreamOffset = Word(r + 4);
        info->m_Unknown08 = Word(r + 8); info->m_Unknown0C = Word(r + 12);
        info->m_ChannelCount = Word(r + 16); info->m_Unknown14 = Word(r + 20);
        info->m_BankLoader = Saved<AudioBankLoader>(Word(r + 24));
    }
    reservation.Commit();
    return {source, raw.bytes, base, bytes, raw.completed.allocation.incarnation};
}

NativeAudioResourceView PrepareNativeAudioResourceChunk(nlChunk* source) {
    Raw raw(source); NativeAudioResourceView existing{};
    if (Existing(raw, ResourceMarker, existing)) return existing;
    raw.Gather(7); Require(raw.children[0], 1, 56);
    const auto* h = raw.children[0].data;
    const auto cues = Word(h + 8), voices = Word(h + 16), sequences = Word(h + 24),
               sounds = Word(h + 32), hits = Word(h + 40), parameters = Word(h + 48);
    std::size_t required = 7;
    for (std::size_t count : {std::size_t(cues), std::size_t(voices), std::size_t(voices),
                            std::size_t(sequences), std::size_t(sounds)}) {
        if (count > std::numeric_limits<std::size_t>::max() - required)
            throw std::overflow_error("Audio requested child count overflows");
        required += count;
    }
    raw.Gather(required);
    const std::size_t counts[] = {1, cues, voices, sequences, sounds, hits, parameters};
    const std::size_t wire[] = {56, 40, 44, 12, 48, 16, 24};
    const std::size_t native[] = {sizeof(AudioResourceBundle), sizeof(AudioCueDefinition), sizeof(AudioVoiceDefinition),
        sizeof(AudioSequenceDefinition), sizeof(SoundEventDefinition), sizeof(HitMarkerEventDefinition), sizeof(ParameterChangeEventDefinition)};
    auto bytes = Start(raw);
    for (unsigned i = 0; i < 7; ++i) {
        Require(raw.children[i], counts[i], wire[i]);
        raw.children[i].native_offset = Extend(bytes, counts[i], native[i]);
    }
    Array arrays[] = {{Word(h + 20), voices, raw.children[2].native_offset, 44, sizeof(AudioVoiceDefinition)},
                      {Word(h + 28), sequences, raw.children[3].native_offset, 12, sizeof(AudioSequenceDefinition)},
                      {Word(h + 36), sounds, raw.children[4].native_offset, 48, sizeof(SoundEventDefinition)},
                      {Word(h + 44), hits, raw.children[5].native_offset, 16, sizeof(HitMarkerEventDefinition)},
                      {Word(h + 52), parameters, raw.children[6].native_offset, 24, sizeof(ParameterChangeEventDefinition)}};
    std::size_t child = 7;
    for (std::size_t i = 0; i < cues; ++i, ++child) {
        const auto count = Word(raw.children[1].data + i * 40 + 4);
        Require(raw.children[child], count, 20);
        for (std::size_t j = 0; j < count; ++j) CheckSaved(Word(raw.children[child].data + j * 20), arrays[0]);
        raw.children[child].native_offset = Extend(bytes, count, sizeof(AudioCueEntry));
    }
    for (std::size_t i = 0; i < voices; ++i) {
        const auto* r = raw.children[2].data + i * 44;
        const auto count = Word(r + 16), rpcCount = Word(r + 24);
        Require(raw.children[child], count, 4);
        for (std::size_t j = 0; j < count; ++j) CheckSaved(Word(raw.children[child].data + j * 4), arrays[1]);
        raw.children[child++].native_offset = Extend(bytes, count, sizeof(AudioSequenceDefinition*));
        Require(raw.children[child], rpcCount, 4);
        raw.children[child++].native_offset = Extend(bytes, rpcCount, sizeof(u32));
    }
    for (std::size_t i = 0; i < sequences; ++i, ++child) {
        const auto count = Word(raw.children[3].data + i * 12 + 4);
        Require(raw.children[child], count, 8);
        for (std::size_t j = 0; j < count; ++j) {
            const auto* r = raw.children[child].data + j * 8; const auto type = Word(r);
            if (type == 1) CheckSaved(Word(r + 4), arrays[2]);
            else if (type == 3) CheckSaved(Word(r + 4), arrays[3]);
            else if (type == 2) CheckSaved(Word(r + 4), arrays[4]);
        }
        raw.children[child].native_offset = Extend(bytes, count, sizeof(AudioSequenceEventDefinition));
    }
    for (std::size_t i = 0; i < sounds; ++i, ++child) {
        const auto count = Word(raw.children[4].data + i * 48 + 8);
        Require(raw.children[child], count, 8);
        raw.children[child].native_offset = Extend(bytes, count, sizeof(SoundChoice));
    }
    GameNativeBackingReservation reservation(source, raw.bytes, bytes);
    auto* base = static_cast<unsigned char*>(reservation.Data()); auto* storage = Begin(base, raw, ResourceMarker);
    for (unsigned i = 0; i < 5; ++i) storage->arrays[i] = arrays[i];
    auto* bundle = new(base + raw.children[0].native_offset) AudioResourceBundle{};
    bundle->field_00 = Word(h); bundle->field_04 = Word(h + 4);
    bundle->cueCount = cues; bundle->cues = Saved<AudioCueDefinition>(Word(h + 12));
    bundle->voiceCount = voices; bundle->voices = Saved<AudioVoiceDefinition>(Word(h + 20));
    bundle->sequenceCount = sequences; bundle->sequences = Saved<AudioSequenceDefinition>(Word(h + 28));
    bundle->soundEventCount = sounds; bundle->soundEvents = Saved<SoundEventDefinition>(Word(h + 36));
    bundle->hitMarkerEventCount = hits; bundle->hitMarkerEvents = Saved<HitMarkerEventDefinition>(Word(h + 44));
    bundle->parameterEventCount = parameters; bundle->parameterEvents = Saved<ParameterChangeEventDefinition>(Word(h + 52));
    for (std::size_t i = 0; i < cues; ++i) {
        const auto* r = raw.children[1].data + i * 40;
        auto* cue = new(base + raw.children[1].native_offset + i * sizeof(AudioCueDefinition)) AudioCueDefinition{};
        cue->name = Saved<const char>(Word(r)); cue->voiceCount = Word(r + 4); cue->voices = Saved<AudioCueEntry>(Word(r + 8));
        cue->useSlider = r[12]; std::memcpy(cue->pad_0D, r + 13, 3);
        cue->selectionMode = static_cast<eAudioCueSelection>(std::bit_cast<s32>(Word(r + 16))); cue->sliderIndex = Word(r + 20);
        cue->selectedVoiceIndex = Word(r + 24); cue->activeCount = Word(r + 28);
        cue->maximumCount = Word(r + 32); cue->field_24 = Saved<void>(Word(r + 36));
    }
    for (std::size_t i = 0; i < voices; ++i) {
        const auto* r = raw.children[2].data + i * 44;
        auto* voice = new(base + raw.children[2].native_offset + i * sizeof(AudioVoiceDefinition)) AudioVoiceDefinition{};
        voice->name = Saved<const char>(Word(r)); voice->volume = Float(r + 4); voice->pitch = Float(r + 8);
        voice->sliderIndex = Word(r + 12); voice->sequenceCount = Word(r + 16); voice->sequences = Saved<AudioSequenceDefinition*>(Word(r + 20));
        voice->rpcGroupCount = Word(r + 24); voice->rpcGroupIndices = Saved<u32>(Word(r + 28));
        voice->dynamicRpcCount = Word(r + 32); voice->modifierCount = Word(r + 36); voice->modifiers = Saved<AudioRpcRuntimeNode*>(Word(r + 40));
    }
    for (std::size_t i = 0; i < sequences; ++i) {
        const auto* r = raw.children[3].data + i * 12;
        auto* sequence = new(base + raw.children[3].native_offset + i * sizeof(AudioSequenceDefinition)) AudioSequenceDefinition{};
        sequence->volumeOffset = Float(r); sequence->eventCount = Word(r + 4); sequence->eventDefinitions = Saved<AudioSequenceEventDefinition>(Word(r + 8));
    }
    for (std::size_t i = 0; i < sounds; ++i) {
        const auto* r = raw.children[4].data + i * 48;
        auto* sound = new(base + raw.children[4].native_offset + i * sizeof(SoundEventDefinition)) SoundEventDefinition{};
        sound->field_00 = Word(r); sound->soundId = Word(r + 4); sound->choiceCount = Word(r + 8); sound->field_0C = Word(r + 12);
        sound->choices = Saved<SoundChoice>(Word(r + 16)); sound->randomPitch = r[20]; sound->randomVolume = r[21]; std::memcpy(sound->pad_16, r + 22, 2);
        sound->pitchMinimum = Float(r + 24); sound->pitchMaximum = Float(r + 28); sound->volumeMinimum = Float(r + 32); sound->volumeMaximum = Float(r + 36);
        sound->delayMinimum = Float(r + 40); sound->delayRange = Float(r + 44);
    }
    for (std::size_t i = 0; i < hits; ++i) {
        const auto* r = raw.children[5].data + i * 16;
        auto* hit = new(base + raw.children[5].native_offset + i * sizeof(HitMarkerEventDefinition)) HitMarkerEventDefinition{};
        hit->field_00 = Word(r); hit->marker = Saved<void>(Word(r + 4)); hit->delayMinimum = Float(r + 8); hit->delayRange = Float(r + 12);
    }
    for (std::size_t i = 0; i < parameters; ++i) {
        const auto* r = raw.children[6].data + i * 24;
        auto* parameter = new(base + raw.children[6].native_offset + i * sizeof(ParameterChangeEventDefinition)) ParameterChangeEventDefinition{};
        parameter->field_00 = Word(r); parameter->field_04 = Word(r + 4); parameter->value = Float(r + 8);
        parameter->parameter = r[12]; std::memcpy(parameter->pad_0D, r + 13, 3); parameter->delayMinimum = Float(r + 16); parameter->delayRange = Float(r + 20);
    }
    child = 7;
    for (std::size_t i = 0; i < cues; ++i, ++child) {
        const auto count = Word(raw.children[1].data + i * 40 + 4);
        for (std::size_t j = 0; j < count; ++j) {
            const auto* r = raw.children[child].data + j * 20;
            auto* entry = new(base + raw.children[child].native_offset + j * sizeof(AudioCueEntry)) AudioCueEntry{};
            entry->voice = Saved<AudioVoiceDefinition>(Word(r)); entry->minimumValue = Float(r + 4); entry->weight = Float(r + 8);
            entry->selectionCount = Word(r + 12); entry->eligible = r[16]; std::memcpy(entry->pad_11, r + 17, 3);
        }
    }
    for (std::size_t i = 0; i < voices; ++i) {
        const auto* r = raw.children[2].data + i * 44;
        const auto count = Word(r + 16), rpcCount = Word(r + 24);
        for (std::size_t j = 0; j < count; ++j)
            new(base + raw.children[child].native_offset + j * sizeof(AudioSequenceDefinition*)) AudioSequenceDefinition*(Saved<AudioSequenceDefinition>(Word(raw.children[child].data + j * 4)));
        ++child;
        for (std::size_t j = 0; j < rpcCount; ++j) new(base + raw.children[child].native_offset + j * sizeof(u32)) u32(Word(raw.children[child].data + j * 4));
        ++child;
    }
    for (std::size_t i = 0; i < sequences; ++i, ++child) {
        const auto count = Word(raw.children[3].data + i * 12 + 4);
        for (std::size_t j = 0; j < count; ++j) {
            const auto* r = raw.children[child].data + j * 8;
            auto* event = new(base + raw.children[child].native_offset + j * sizeof(AudioSequenceEventDefinition)) AudioSequenceEventDefinition{};
            event->type = static_cast<eAudioSequenceEventType>(std::bit_cast<s32>(Word(r))); event->sound = Saved<SoundEventDefinition>(Word(r + 4));
        }
    }
    for (std::size_t i = 0; i < sounds; ++i, ++child) {
        const auto count = Word(raw.children[4].data + i * 48 + 8);
        for (std::size_t j = 0; j < count; ++j) {
            const auto* r = raw.children[child].data + j * 8;
            new(base + raw.children[child].native_offset + j * sizeof(SoundChoice)) SoundChoice{Word(r), Word(r + 4)};
        }
    }
    reservation.Commit();
    return {source, raw.bytes, base, bytes, raw.completed.allocation.incarnation};
}

void* NativeAudioResourceData(const NativeAudioResourceView& view, nlChunk* child) {
    const auto& s = Validate(view); const auto* children = reinterpret_cast<const Child*>(&s + 1);
    for (std::size_t i = 0; i < s.children; ++i)
        if (children[i].header == child) return static_cast<unsigned char*>(view.data) + children[i].native_offset;
    throw std::invalid_argument("Original audio chunk is outside this exact typed profile");
}
NativeAudioResourceRelocation NativeAudioResourceOrigin(
    const NativeAudioResourceView& view, AudioResourceArray array, const void* original) {
    const auto& a = GetArray(Validate(view), array);
    const auto* records = static_cast<unsigned char*>(view.data) + a.offset;
    if (original == records) return {view, array, true};
    if (reinterpret_cast<std::uintptr_t>(original) != a.origin)
        throw std::invalid_argument("Audio old-record value differs from its authored32-bit origin");
    return {view, array, false};
}
NativeAudioResourceRelocation NativeAudioResourceDelta(
    const NativeAudioResourceRelocation& original, const void* records) {
    const auto& a = GetArray(Validate(original.view), original.array);
    if (records != static_cast<unsigned char*>(original.view.data) + a.offset)
        throw std::invalid_argument("Audio original rebasing lost its actual native record array");
    return original;
}
void* RelocateNativeAudioResourceRecord(
    const void* original, const NativeAudioResourceRelocation& relocation) {
    const auto& a = GetArray(Validate(relocation.view), relocation.array);
    auto* records = static_cast<unsigned char*>(relocation.view.data) + a.offset;
    const auto address = reinterpret_cast<std::uintptr_t>(original);
    if (relocation.already_native) {
        const auto base = reinterpret_cast<std::uintptr_t>(records);
        if (address < base || (address - base) % a.native_stride || (address - base) / a.native_stride >= a.count)
            throw std::out_of_range("Repeated audio record leaves its live typed array");
        return const_cast<void*>(original);
    }
    if (address > std::numeric_limits<std::uint32_t>::max())
        throw std::invalid_argument("Audio serialized address has an unsupported width");
    CheckSaved(static_cast<std::uint32_t>(address), a);
    const std::uint32_t offset = static_cast<std::uint32_t>(address) - a.origin;
    return records + offset / a.wire_stride * a.native_stride;
}
}
