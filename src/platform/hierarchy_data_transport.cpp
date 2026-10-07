#include "platform/hierarchy_data_transport.h"
#include "platform/game_allocation_ownership.h"
#include "platform/host_metadata.h"
#include "Game/SHierarchy.h"
#include "NL/nlChunk.h"

#include <array>
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
template<class T> using Scratch = std::vector<T, MetadataAllocator<T>>;
constexpr std::uint32_t Marker = 0x53484945;
constexpr std::size_t ChildCount = 11;
struct Child {
    nlChunk* source;
    const unsigned char* raw;
    std::size_t raw_bytes, count, offset, bytes;
};
struct StoredChild { nlChunk* source; std::size_t offset, bytes; };
struct Storage { std::uint32_t marker, children; StoredChild child[ChildCount]; };
struct IdentifierLayout : cIdentifier {
    static constexpr std::size_t HashOffset() { return offsetof(IdentifierLayout, m_uHashID); }
};
static_assert(sizeof(int) == 4 && sizeof(u32) == 4 && sizeof(float) == 4);
static_assert(sizeof(nlVector3) == 12);
static_assert(alignof(Storage) <= 8 && alignof(cSHierarchy) <= 8);
static_assert(alignof(int*) <= 8 && alignof(nlVector3) <= 8);
static_assert(std::is_trivially_destructible_v<cSHierarchy>);

std::uint32_t Word(const unsigned char* p) {
    return std::uint32_t(p[0]) << 24 | std::uint32_t(p[1]) << 16
         | std::uint32_t(p[2]) << 8 | p[3];
}
std::int32_t Signed(const unsigned char* p) { return std::bit_cast<std::int32_t>(Word(p)); }
void Extent(const Child& child, std::size_t count, std::size_t stride) {
    if (count > child.raw_bytes / stride)
        throw std::out_of_range("Hierarchy native fields leave their authored child");
}
std::size_t Extend(std::size_t& bytes, std::size_t count, std::size_t stride) {
    if (bytes > std::numeric_limits<std::size_t>::max() - 7)
        throw std::overflow_error("Hierarchy native alignment overflows");
    bytes = (bytes + 7) & ~std::size_t(7);
    const auto result = bytes;
    if (count > (std::numeric_limits<std::size_t>::max() - bytes) / stride)
        throw std::overflow_error("Hierarchy native extent overflows");
    bytes += count * stride;
    return result;
}
NativeHierarchyView View(nlChunk* source, std::size_t sourceBytes,
                         const GameNativeBackingSpan& backing) {
    if (backing.bytes < sizeof(Storage))
        throw std::invalid_argument("Hierarchy native backing is incomplete");
    const auto& storage = *static_cast<const Storage*>(backing.data);
    if (storage.marker != Marker || storage.children != ChildCount)
        throw std::invalid_argument("Hierarchy source has another native backing profile");
    return {source, sourceBytes, backing.data, backing.bytes, backing.allocation.incarnation};
}
// Only prove bounded accesses by the original recursive algorithm. This is not
// a tree normalizer: duplicate children, unreachable nodes, non-preorder layout,
// arbitrary parent/mirror/special-index words and authored push flags survive.
void CheckTraversal(const std::array<Child, ChildCount>& children, std::size_t nodes) {
    Scratch<std::size_t> starts(nodes);
    std::size_t total = 0;
    for (std::size_t node = 0; node < nodes; ++node) {
        const auto count = Signed(children[4].raw + node * 4);
        starts[node] = total;
        if (count < 0) {
            const auto backward = std::size_t(-std::int64_t(count));
            if (backward > total)
                throw std::out_of_range("Hierarchy child cursor leaves its authored table");
            total -= backward;
        } else {
            if (std::size_t(count) > children[7].raw_bytes / 4 - total)
                throw std::out_of_range("Hierarchy child cursor leaves its authored table");
            total += std::size_t(count);
        }
    }
    struct Frame { std::size_t node, next; };
    Scratch<unsigned char> active(nodes, 0);
    Scratch<Frame> stack;
    stack.push_back({0, 0}); active[0] = 1;
    while (!stack.empty()) {
        auto& frame = stack.back();
        const auto count = Signed(children[4].raw + frame.node * 4);
        // A negative original count advances source depth but executes zero
        // loop iterations. Retain that quirk when its signed cursor is bounded.
        if (count <= 0 || frame.next == std::size_t(count)) {
            active[frame.node] = 2; stack.pop_back(); continue;
        }
        const auto index = starts[frame.node] + frame.next++;
        const auto child = Signed(children[7].raw + index * 4);
        if (child < 0 || std::size_t(child) >= nodes)
            throw std::out_of_range("Hierarchy recursive child index leaves its native arrays");
        if (active[child] == 1)
            throw std::invalid_argument("Hierarchy recursion revisits an active node");
        if (!active[child]) { active[child] = 1; stack.push_back({std::size_t(child), 0}); }
    }
}
}

NativeHierarchyView PrepareNativeHierarchy(nlChunk* source) {
    GameCompletedSpan complete{};
    if (!source || !FindGameCompletedSpan(source, 8, complete))
        throw std::invalid_argument("Hierarchy metadata requires actual completed NL bytes");
    const auto sourceBytes = std::size_t(source->GetSize()) + 8;
    if (!FindGameCompletedSpan(source, sourceBytes, complete)
        || FindGameByteDomain(source, sourceBytes) != GameByteDomain::WiiSerialized)
        throw std::invalid_argument("Hierarchy metadata requires one original serialized span");
    GameNativeBackingSpan backing{};
    if (FindGameNativeBacking(source, sourceBytes, backing)) return View(source, sourceBytes, backing);

    std::array<Child, ChildCount> children{};
    auto* cursor = static_cast<nlChunk*>(source->GetData());
    const auto limit = reinterpret_cast<std::uintptr_t>(source) + sourceBytes;
    for (auto& child : children) {
        const auto here = reinterpret_cast<std::uintptr_t>(cursor);
        if (here > limit || limit - here < 8 || !FindGameCompletedSpan(cursor, 8, complete))
            throw std::out_of_range("Original hierarchy required child is absent");
        const auto payload = std::size_t(cursor->GetSize());
        if (payload > limit - here - 8)
            throw std::out_of_range("Hierarchy serialized child leaves its parent");
        const auto end = here + 8 + payload;
        if (end > std::numeric_limits<std::uintptr_t>::max() - 3
            || ((end + 3) & ~std::uintptr_t(3)) > limit)
            throw std::out_of_range("Hierarchy child padding leaves its parent");
        auto* next = cursor->GetNextChunk();
        const auto* raw = static_cast<const unsigned char*>(cursor->GetData());
        const auto rawBytes = std::size_t(cursor->GetDataSize());
        const auto start = reinterpret_cast<std::uintptr_t>(raw);
        if (start < here + 8 || start > end || rawBytes > end - start
            || (rawBytes && !FindGameCompletedSpan(raw, rawBytes, complete)))
            throw std::out_of_range("Hierarchy data leaves its completed child");
        child = {cursor, raw, rawBytes, 0, 0, 0}; cursor = next;
    }
    // Initialize consumes positional children, without checking their IDs or
    // rejecting trailing chunks. Keep that exact original parser decision.
    Extent(children[0], 1, 52);
    const auto signedNodes = Signed(children[0].raw + 8);
    if (signedNodes <= 0)
        throw std::out_of_range("Hierarchy root access requires a positive native array extent");
    const auto nodes = std::size_t(signedNodes);
    for (auto index : {2, 3, 4, 5, 6, 8}) Extent(children[index], nodes, 4);
    Extent(children[9], nodes, 12); Extent(children[10], nodes, 1);
    if (!children[1].raw_bytes || !std::memchr(children[1].raw, 0, children[1].raw_bytes))
        throw std::out_of_range("Hierarchy name has no terminator in its completed child");
    CheckTraversal(children, nodes);

    std::size_t bytes = sizeof(Storage);
    children[0].count = 1; children[1].count = children[1].raw_bytes;
    for (auto index : {2, 3, 4, 5, 6, 8, 9, 10}) children[index].count = nodes;
    children[7].count = children[7].raw_bytes / 4;
    for (std::size_t i = 0; i < ChildCount; ++i) {
        const auto stride = i == 0 ? sizeof(cSHierarchy) : i == 1 || i == 10 ? 1
            : i == 5 ? sizeof(int*) : i == 9 ? sizeof(nlVector3) : 4;
        children[i].offset = Extend(bytes, children[i].count, stride);
        children[i].bytes = children[i].count * stride;
    }
    GameNativeBackingReservation reservation(source, sourceBytes, bytes);
    auto* base = static_cast<unsigned char*>(reservation.Data());
    auto* storage = new(base) Storage{};
    storage->marker = Marker; storage->children = ChildCount;
    for (std::size_t i = 0; i < ChildCount; ++i) {
        const auto& child = children[i];
        storage->child[i] = {child.source, child.offset, child.bytes};
        auto* data = base + child.offset;
        if (i == 0) {
            auto* header = new(data) cSHierarchy{};
            const auto hash = Word(child.raw + 4);
            std::memcpy(data + IdentifierLayout::HashOffset(), &hash, sizeof(hash));
            header->m_nNumNodes = signedNodes;
            header->m_nPelvisNodeIndex = Signed(child.raw + 36);
            header->m_nSpineNodeIndex = Signed(child.raw + 40);
            // All serialized pointer fields are overwritten before use by
            // the literal original Initialize. They are never live tokens.
        } else if (i == 1 || i == 10) {
            std::memcpy(data, child.raw, child.count);
        } else if (i == 5) {
            for (std::size_t j = 0; j < child.count; ++j)
                new(data + j * sizeof(int*)) int*(nullptr);
            // Original loop initializes every pointer before BuildPushPop.
        } else if (i == 9) {
            for (std::size_t j = 0; j < child.count; ++j) {
                auto* vector = new(data + j * sizeof(nlVector3)) nlVector3{};
                for (unsigned component = 0; component < 3; ++component) {
                    const auto bits = Word(child.raw + j * 12 + component * 4);
                    std::memcpy(&vector->e[component], &bits, sizeof(bits));
                }
            }
        } else {
            for (std::size_t j = 0; j < child.count; ++j) {
                const auto word = Word(child.raw + j * 4);
                // Native words retain exactly the authored u32/int bit
                // representation; no numeric casts normalize signed values.
                if (i == 2) new(data + j * 4) u32(word);
                else new(data + j * 4) int(std::bit_cast<std::int32_t>(word));
            }
        }
    }
    reservation.Commit();
    if (!FindGameNativeBacking(source, sourceBytes, backing))
        throw std::logic_error("Hierarchy native backing publication is absent");
    return View(source, sourceBytes, backing);
}

void* NativeHierarchyChunkData(const NativeHierarchyView& view, nlChunk* child) {
    GameNativeBackingSpan backing{};
    if (!FindGameNativeBacking(view.source, view.source_bytes, backing)
        || backing.data != view.data || backing.bytes != view.native_bytes
        || backing.allocation.incarnation != view.incarnation)
        throw std::invalid_argument("Hierarchy view has no live original allocation incarnation");
    View(view.source, view.source_bytes, backing);
    const auto& storage = *static_cast<const Storage*>(view.data);
    for (const auto& saved : storage.child) {
        if (saved.source == child) {
            if (saved.offset > view.native_bytes || saved.bytes > view.native_bytes - saved.offset)
                throw std::logic_error("Hierarchy native child leaves its backing");
            return static_cast<unsigned char*>(view.data) + saved.offset;
        }
    }
    throw std::invalid_argument("Hierarchy child is outside the original parser's native view");
}
}
