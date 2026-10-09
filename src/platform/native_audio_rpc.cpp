#include "platform/native_audio_rpc.h"
#include "platform/game_allocation_ownership.h"
#include "platform/host_metadata.h"
#include "Game/Audio/AudioRpc.h"
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
        auto* result = ChargedNativeMetadataAllocate(n * sizeof(T));
        if (!result) throw std::bad_alloc();
        return static_cast<T*>(result);
    }
    void deallocate(T* p, std::size_t) noexcept { ChargedNativeMetadataRelease(p); }
    template<class U> bool operator==(const MetadataAllocator<U>&) const noexcept { return true; }
};
enum class Kind { Controller, Groups, Definitions, Points };
struct Child {
    nlChunk* source;
    const unsigned char* raw;
    std::size_t raw_bytes, count, native_offset;
    Kind kind;
};
struct StoredChild { nlChunk* source; std::size_t offset; };
struct Storage { std::uint32_t marker, children; };
constexpr std::uint32_t Marker = 0x41525043;
using Children = std::vector<Child, MetadataAllocator<Child>>;
static_assert(std::is_trivially_destructible_v<AudioRpcController>);
static_assert(std::is_trivially_destructible_v<AudioRpcGroup>);
static_assert(std::is_trivially_destructible_v<AudioRpcDefinition>);
static_assert(std::is_trivially_destructible_v<AudioRpcCurvePoint>);
// This projection follows the native POD declarations. The existing backing
// provider returns CRT storage, and this profile uses an eight-byte stride;
// never silently construct a wider-aligned typed object at that boundary.
static_assert(alignof(Storage) <= 8 && alignof(StoredChild) <= 8);
static_assert(alignof(AudioRpcController) <= 8 && alignof(AudioRpcGroup) <= 8);
static_assert(alignof(AudioRpcDefinition) <= 8 && alignof(AudioRpcCurvePoint) <= 8);

std::uint32_t Word(const unsigned char* p) {
    return std::uint32_t(p[0]) << 24 | std::uint32_t(p[1]) << 16
         | std::uint32_t(p[2]) << 8 | p[3];
}
float Float(const unsigned char* p) { return std::bit_cast<float>(Word(p)); }
template<class T> T* Saved(std::uint32_t word) {
    // Serialized words are retained until the original parser overwrites them.
    // These are not live hardware addresses or reconstructed native pointers.
    return reinterpret_cast<T*>(std::uintptr_t(word));
}
std::size_t Extend(std::size_t& bytes, std::size_t count, std::size_t stride) {
    if (bytes > std::numeric_limits<std::size_t>::max() - 7)
        throw std::overflow_error("RPC native alignment overflows");
    bytes = (bytes + 7) & ~std::size_t(7);
    const auto offset = bytes;
    if (count > (std::numeric_limits<std::size_t>::max() - bytes) / stride)
        throw std::overflow_error("RPC native array extent overflows");
    bytes += count * stride;
    return offset;
}
void Require(const Child& child, std::size_t count, std::size_t stride) {
    if (count > child.raw_bytes / stride)
        throw std::out_of_range("RPC authored records leave their completed child");
}
NativeAudioRpcView View(nlChunk* source, std::size_t source_bytes,
                        const GameNativeBackingSpan& backing) {
    if (backing.bytes < sizeof(Storage))
        throw std::invalid_argument("RPC native backing is incomplete");
    const auto& storage = *static_cast<const Storage*>(backing.data);
    if (storage.marker != Marker || storage.children < 2
        || storage.children > (backing.bytes - sizeof(Storage)) / sizeof(StoredChild))
        throw std::invalid_argument("RPC source has another native backing profile");
    return {source, source_bytes, backing.data, backing.bytes, backing.allocation.incarnation};
}
}

NativeAudioRpcView PrepareNativeAudioRpc(nlChunk* source) {
    GameCompletedSpan complete{};
    if (!source || !FindGameCompletedSpan(source, 8, complete))
        throw std::invalid_argument("RPC metadata requires actual completed NL bytes");
    const auto source_bytes = std::size_t(source->GetSize()) + 8;
    if (!FindGameCompletedSpan(source, source_bytes, complete)
        || FindGameByteDomain(source, source_bytes) != GameByteDomain::WiiSerialized)
        throw std::invalid_argument("RPC metadata requires one original serialized source span");
    GameNativeBackingSpan backing{};
    if (FindGameNativeBacking(source, source_bytes, backing)) return View(source, source_bytes, backing);

    Children children;
    // Match the original parser's raw chunk traversal. Counts and addresses in
    // the native projection do not modify source headers or serialized bytes.
    auto* cursor = static_cast<nlChunk*>(source->GetData());
    const auto limit = reinterpret_cast<std::uintptr_t>(source->GetLastChunk());
    auto Append = [&](Kind kind, std::size_t count, std::size_t stride) -> Child {
        const auto here = reinterpret_cast<std::uintptr_t>(cursor);
        if (here >= limit || limit - here < 8 || !FindGameCompletedSpan(cursor, 8, complete))
            throw std::out_of_range("Original RPC required child is absent");
        // Check the serialized extent before the original pointer-returning
        // helper forms its next address. Malformed raw lengths must not cause
        // native pointer arithmetic outside the completed parent allocation.
        const auto payload = std::size_t(cursor->GetSize());
        if (payload > limit - here - 8)
            throw std::out_of_range("RPC serialized child payload leaves its parent");
        const auto unaligned_end = here + 8 + payload;
        if (unaligned_end > std::numeric_limits<std::uintptr_t>::max() - 3
            || ((unaligned_end + 3) & ~std::uintptr_t(3)) > limit)
            throw std::out_of_range("RPC serialized child padding leaves its parent");
        auto* next = cursor->GetNextChunk();
        const auto after = reinterpret_cast<std::uintptr_t>(next);
        if (after <= here || after > limit)
            throw std::out_of_range("RPC child leaves its original parent");
        const auto* raw = static_cast<const unsigned char*>(cursor->GetData());
        const auto raw_bytes = std::size_t(cursor->GetDataSize());
        const auto start = reinterpret_cast<std::uintptr_t>(raw);
        if (start < here + 8 || start > after || raw_bytes > after - start
            || (raw_bytes && !FindGameCompletedSpan(raw, raw_bytes, complete)))
            throw std::out_of_range("RPC data leaves its original completed child");
        Child child{cursor, raw, raw_bytes, count, 0, kind};
        Require(child, count, stride); children.push_back(child); cursor = next;
        return child;
    };
    const auto header = Append(Kind::Controller, 1, 20);
    const auto group_count = Word(header.raw);
    const auto groups = Append(Kind::Groups, group_count, 28);
    for (std::size_t i = 0; i < group_count; ++i) {
        const auto* group = groups.raw + i * 28;
        const auto count = Word(group + 4);
        // ParseAudioRpcController forms definitions + staticDefinitionCount.
        // Only an element or the exact one-past address has a native array
        // representation; retain all valid original partition/count choices.
        if (Word(group + 12) > count)
            throw std::out_of_range("RPC static definition address leaves its authored array");
        const auto definitions = Append(Kind::Definitions, count, 36);
        for (std::size_t j = 0; j < definitions.count; ++j)
            Append(Kind::Points, Word(definitions.raw + j * 36 + 24), 8);
    }
    if (children.size() > std::numeric_limits<std::uint32_t>::max())
        throw std::overflow_error("RPC native child count overflows");
    std::size_t bytes = sizeof(Storage);
    const auto descriptors = Extend(bytes, children.size(), sizeof(StoredChild));
    for (auto& child : children) {
        const auto stride = child.kind == Kind::Controller ? sizeof(AudioRpcController)
            : child.kind == Kind::Groups ? sizeof(AudioRpcGroup)
            : child.kind == Kind::Definitions ? sizeof(AudioRpcDefinition)
            : sizeof(AudioRpcCurvePoint);
        child.native_offset = Extend(bytes, child.count, stride);
    }
    GameNativeBackingReservation reservation(source, source_bytes, bytes);
    auto* base = static_cast<unsigned char*>(reservation.Data());
    new(base) Storage{Marker, static_cast<std::uint32_t>(children.size())};
    auto* stored = reinterpret_cast<StoredChild*>(base + descriptors);
    for (std::size_t c = 0; c < children.size(); ++c) {
        const auto& child = children[c];
        new(stored + c) StoredChild{child.source, child.native_offset};
        for (std::size_t i = 0; i < child.count; ++i) {
            switch (child.kind) {
            case Kind::Controller: {
                auto* out = new(base + child.native_offset) AudioRpcController{};
                out->groupCount = Word(child.raw);
                out->groups = Saved<AudioRpcGroup>(Word(child.raw + 4));
                out->runtimeCount = Word(child.raw + 8);
                out->runtimeNodes = Saved<AudioRpcRuntimeNode*>(Word(child.raw + 12));
                out->dynamicNodes = Saved<AudioRpcList>(Word(child.raw + 16));
                break;
            }
            case Kind::Groups: {
                const auto* raw = child.raw + i * 28;
                auto* out = new(base + child.native_offset + i * sizeof(AudioRpcGroup)) AudioRpcGroup{};
                out->field_00 = Word(raw); out->definitionCount = Word(raw + 4);
                out->definitions = Saved<AudioRpcDefinition>(Word(raw + 8));
                out->staticDefinitionCount = Word(raw + 12);
                out->staticDefinitions = Saved<AudioRpcDefinition>(Word(raw + 16));
                out->dynamicDefinitionCount = Word(raw + 20);
                out->dynamicDefinitions = Saved<AudioRpcDefinition>(Word(raw + 24));
                break;
            }
            case Kind::Definitions: {
                const auto* raw = child.raw + i * 36;
                auto* out = new(base + child.native_offset + i * sizeof(AudioRpcDefinition)) AudioRpcDefinition{};
                out->field_00 = Word(raw); out->field_04 = Word(raw + 4);
                out->sliderIndex = Word(raw + 8); out->kind = static_cast<eAudioRpcKind>(std::bit_cast<std::int32_t>(Word(raw + 12)));
                out->enabled = Word(raw + 16); out->field_14 = Word(raw + 20);
                out->pointCount = Word(raw + 24);
                out->points = Saved<AudioRpcCurvePoint>(Word(raw + 28));
                out->runtimeNode = Saved<AudioRpcRuntimeNode>(Word(raw + 32));
                break;
            }
            case Kind::Points: {
                const auto* raw = child.raw + i * 8;
                new(base + child.native_offset + i * sizeof(AudioRpcCurvePoint))
                    AudioRpcCurvePoint{Float(raw), Float(raw + 4)};
                break;
            }
            }
        }
    }
    reservation.Commit();
    if (!FindGameNativeBacking(source, source_bytes, backing))
        throw std::logic_error("RPC native backing publication is absent");
    return View(source, source_bytes, backing);
}

void* NativeAudioRpcChildData(const NativeAudioRpcView& view, nlChunk* child) {
    GameNativeBackingSpan backing{};
    if (!FindGameNativeBacking(view.source, view.source_bytes, backing)
        || backing.data != view.data || backing.bytes != view.native_bytes
        || backing.allocation.incarnation != view.incarnation)
        throw std::invalid_argument("RPC view has no live original allocation incarnation");
    View(view.source, view.source_bytes, backing);
    const auto& storage = *static_cast<const Storage*>(view.data);
    const auto* base = static_cast<const unsigned char*>(view.data);
    const auto* children = reinterpret_cast<const StoredChild*>(base + sizeof(Storage));
    for (std::uint32_t i = 0; i < storage.children; ++i) {
        if (children[i].source == child) {
            if (children[i].offset > view.native_bytes)
                throw std::logic_error("RPC native child offset leaves its backing");
            return static_cast<unsigned char*>(view.data) + children[i].offset;
        }
    }
    throw std::invalid_argument("RPC child is not part of the source parser's native view");
}
}
