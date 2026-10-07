#include "platform/sanim_data_transport.h"
#include "Game/SAnim.h"
#include "platform/game_allocation_ownership.h"
#include "platform/host_metadata.h"
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
template <class T> struct MetadataAllocator {
  using value_type = T;
  MetadataAllocator() noexcept = default;
  template <class U> MetadataAllocator(const MetadataAllocator<U> &) noexcept {}
  T *allocate(std::size_t n) {
    if (n > std::numeric_limits<std::size_t>::max() / sizeof(T))
      throw std::bad_alloc();
    auto *p = ChargedNativeMetadataAllocate(n * sizeof(T));
    if (!p)
      throw std::bad_alloc();
    return static_cast<T *>(p);
  }
  void deallocate(T *p, std::size_t) noexcept {
    ChargedNativeMetadataRelease(p);
  }
  template <class U>
  bool operator==(const MetadataAllocator<U> &) const noexcept {
    return true;
  }
};
template <class T> using Scratch = std::vector<T, MetadataAllocator<T>>;
enum class Kind {
  Header,
  Bytes,
  Words,
  Halves,
  LongWords,
  RotPointers,
  TransPointers,
  ScalePointers,
  BytePointers,
  VoidPointers,
  RootVectors,
  TransVectors,
  Scales
};
struct Child {
  nlChunk *source;
  const unsigned char *raw;
  std::size_t raw_bytes;
  Kind kind;
  std::size_t count{}, offset{}, bytes{};
};
struct StoredChild {
  nlChunk *source;
  void *data;
  std::size_t bytes;
};
struct Storage {
  std::uint32_t marker, version;
  std::size_t count, header_offset, map_offset;
};
struct IdentifierLayout : cIdentifier {
  static constexpr std::size_t HashOffset() {
    return offsetof(IdentifierLayout, m_uHashID);
  }
};
constexpr std::uint32_t Marker = 0x53414e49;
static_assert(sizeof(unsigned int) == 4 && sizeof(unsigned short) == 2 &&
              sizeof(float) == 4);
static_assert(sizeof(nlVector3) == 12 && sizeof(PackedTrans) == 12 &&
              sizeof(PackedScale) == 6);
static_assert(alignof(Storage) <= 8 && alignof(cSAnim) <= 8 &&
              alignof(StoredChild) <= 8);
static_assert(std::is_trivially_destructible_v<cSAnim>);
unsigned Word(const unsigned char *p) {
  return unsigned(p[0]) << 24 | unsigned(p[1]) << 16 | unsigned(p[2]) << 8 |
         p[3];
}
unsigned short Half(const unsigned char *p) {
  return unsigned(p[0]) << 8 | p[1];
}
std::size_t Extend(std::size_t &bytes, std::size_t count, std::size_t stride) {
  if (bytes > std::numeric_limits<std::size_t>::max() - 7)
    throw std::overflow_error("SAnim native alignment overflows");
  bytes = (bytes + 7) & ~std::size_t(7);
  auto offset = bytes;
  if (count > (std::numeric_limits<std::size_t>::max() - bytes) / stride)
    throw std::overflow_error("SAnim native extent overflows");
  bytes += count * stride;
  return offset;
}
Child Describe(nlChunk *chunk, std::uintptr_t limit, Kind kind) {
  const auto at = reinterpret_cast<std::uintptr_t>(chunk);
  GameCompletedSpan completed{};
  if (at > limit || limit - at < 8 ||
      !FindGameCompletedSpan(chunk, 8, completed))
    throw std::out_of_range("Original SAnim required chunk is absent");
  const auto size = std::size_t(chunk->GetSize());
  if (size > limit - at - 8)
    throw std::out_of_range("SAnim child leaves its original parent");
  const auto end = at + 8 + size;
  if (end > std::numeric_limits<std::uintptr_t>::max() - 3 ||
      ((end + 3) & ~std::uintptr_t(3)) > limit)
    throw std::out_of_range("SAnim child padding leaves its parent");
  const auto *raw = static_cast<const unsigned char *>(chunk->GetData());
  const auto start = reinterpret_cast<std::uintptr_t>(raw);
  const auto bytes = std::size_t(chunk->GetDataSize());
  if (start < at + 8 || start > end || bytes > end - start ||
      (bytes && !FindGameCompletedSpan(raw, bytes, completed)))
    throw std::out_of_range("SAnim data leaves its completed child");
  return {chunk, raw, bytes, kind};
}
void AtLeast(const Child &child, std::size_t count, std::size_t stride) {
  if (count > child.raw_bytes / stride)
    throw std::out_of_range("SAnim native fields leave their serialized child");
}
bool PointerKind(Kind k) {
  return k == Kind::RotPointers || k == Kind::TransPointers ||
         k == Kind::ScalePointers || k == Kind::BytePointers ||
         k == Kind::VoidPointers;
}
std::size_t Stride(Kind k) {
  switch (k) {
  case Kind::Header:
    return sizeof(cSAnim);
  case Kind::Words:
    return 4;
  case Kind::Halves:
    return 2;
  case Kind::LongWords:
    return sizeof(unsigned long);
  case Kind::RootVectors:
    return sizeof(nlVector3);
  case Kind::TransVectors:
    return sizeof(PackedTrans);
  case Kind::Scales:
    return sizeof(PackedScale);
  default:
    return PointerKind(k) ? sizeof(void *) : 1;
  }
}
NativeSAnimView View(nlChunk *source, std::size_t bytes,
                     const GameNativeBackingSpan &backing) {
  if (backing.bytes < sizeof(Storage))
    throw std::invalid_argument("SAnim backing prefix is incomplete");
  const auto &s = *static_cast<const Storage *>(backing.data);
  if (s.marker != Marker || s.version != 1 || s.map_offset > backing.bytes ||
      s.count > (backing.bytes - s.map_offset) / sizeof(StoredChild) ||
      s.header_offset > backing.bytes ||
      sizeof(cSAnim) > backing.bytes - s.header_offset)
    throw std::invalid_argument(
        "SAnim source has another or incomplete backing profile");
  return {source, bytes, backing.data, backing.bytes,
          backing.allocation.incarnation};
}
} // namespace
NativeSAnimView PrepareNativeSAnim(nlChunk *source) {
  GameCompletedSpan complete{};
  if (!source || !FindGameCompletedSpan(source, 8, complete))
    throw std::invalid_argument("SAnim requires actual completed NL bytes");
  const auto payloadBytes = std::size_t(source->GetSize()) + 8;
  if (!FindGameCompletedSpan(source, payloadBytes, complete))
    throw std::invalid_argument("SAnim root leaves completed NL bytes");
  const auto sourceEnd =
      reinterpret_cast<std::uintptr_t>(source) + payloadBytes;
  if (sourceEnd > std::numeric_limits<std::uintptr_t>::max() - 3)
    throw std::overflow_error("SAnim parent padding overflows");
  const auto bytes = ((sourceEnd + 3) & ~std::uintptr_t(3)) -
                     reinterpret_cast<std::uintptr_t>(source);
  if (!FindGameCompletedSpan(source, bytes, complete) ||
      FindGameByteDomain(source, bytes) != GameByteDomain::WiiSerialized)
    throw std::invalid_argument(
        "SAnim requires one original completed serialized span");
  GameNativeBackingSpan backing{};
  if (FindGameNativeBacking(source, bytes, backing))
    return View(source, bytes, backing);
  const auto limit = reinterpret_cast<std::uintptr_t>(source) + bytes;
  Scratch<Child> children;
  Scratch<nlChunk *> nodes;
  auto *cursor = source->GetFirstChunk();
  constexpr Kind prefix[] = {
      Kind::Header,        Kind::Bytes,        Kind::Words,
      Kind::Words,         Kind::RotPointers,  Kind::TransPointers,
      Kind::ScalePointers, Kind::BytePointers, Kind::VoidPointers,
      Kind::Halves,        Kind::RootVectors};
  for (auto kind : prefix) {
    children.push_back(Describe(cursor, limit, kind));
    cursor = cursor->GetNextChunk();
  }
  AtLeast(children[0], 1, 88);
  const auto *header = children[0].raw;
  const auto nodeCount = std::size_t(Word(header + 12)),
             rootCount = std::size_t(Word(header + 52)),
             morphCount = std::size_t(Word(header + 16));
  // The original initializer selects consecutive node IDs and then its four
  // positional suffix children. Unknown sub-IDs are ignored, duplicates retain
  // their original later-assignment behavior. No ordering/count policy is
  // added.
  while (reinterpret_cast<std::uintptr_t>(cursor) != limit) {
    Describe(cursor, limit, Kind::Bytes);
    if (cursor->GetID() != 0x80017100)
      break;
    nodes.push_back(cursor);
    cursor = cursor->GetNextChunk();
  }
  const auto suffixStart = children.size();
  for (auto kind : {Kind::Words, Kind::LongWords, Kind::Bytes, Kind::Words}) {
    children.push_back(Describe(cursor, limit, kind));
    cursor = cursor->GetNextChunk();
  }
  AtLeast(children[2], nodeCount, 4);
  AtLeast(children[3], nodeCount, 4);
  for (std::size_t i = 4; i < 9; ++i) {
    AtLeast(children[i], nodeCount, 4);
    AtLeast(children[i], nodes.size(), 4);
  }
  AtLeast(children[9], rootCount, 2);
  AtLeast(children[10], rootCount, 12);
  AtLeast(children[suffixStart], morphCount, 4);
  AtLeast(children[suffixStart + 1], morphCount, 4);
  AtLeast(children[suffixStart + 3], nodeCount, 4);
  Scratch<std::array<bool, 5>> assigned;
  const auto capacity = children[4].raw_bytes / 4;
  assigned.resize(capacity);
  for (std::size_t n = 0; n < nodes.size(); ++n) {
    Describe(nodes[n], limit, Kind::Bytes);
    auto *sub = nodes[n]->GetFirstChunk();
    const auto subEnd =
        reinterpret_cast<std::uintptr_t>(nodes[n]->GetLastChunk());
    while (reinterpret_cast<std::uintptr_t>(sub) != subEnd) {
      auto c = Describe(sub, subEnd, Kind::Bytes);
      const auto id = sub->GetID();
      int table = -1;
      switch (id) {
      case 0x17101:
        table = 0;
        // The original initializer also assigns extra node chunks beyond
        // NumNodes; bounded BlendRot never decodes those channels. Preserve
        // their raw bytes without requiring an absent property word.
        if (n < nodeCount && (Word(children[suffixStart + 3].raw + n * 4) & 1))
          c.kind = Kind::Halves;
        break;
      case 0x17102:
        table = 1;
        c.kind = Kind::TransVectors;
        break;
      case 0x17103:
        table = 2;
        c.kind = Kind::Scales;
        break;
      case 0x17112:
        table = 3;
        break;
      case 0x17115:
        table = 4;
        break;
      default:
        break;
      }
      if (table >= 0) {
        assigned[n][table] = true;
        children.push_back(c);
      }
      sub = sub->GetNextChunk();
    }
  }
  for (std::size_t t = 0; t < 5; ++t) {
    const auto &c = children[t + 4];
    for (std::size_t n = 0; n < c.raw_bytes / 4; ++n)
      if (Word(c.raw + n * 4) != 0 && (n >= assigned.size() || !assigned[n][t]))
        throw std::invalid_argument("Unassigned serialized SAnim pointer has "
                                    "no evidenced native owner");
  }
  std::size_t nativeBytes = sizeof(Storage);
  const auto mapOffset =
      Extend(nativeBytes, children.size(), sizeof(StoredChild));
  for (std::size_t i = 0; i < children.size(); ++i) {
    auto &c = children[i];
    if (c.kind == Kind::Bytes) {
      c.bytes = c.raw_bytes;
      continue;
    }
    auto wireStride = c.kind == Kind::Header ? 88
                      : PointerKind(c.kind) || c.kind == Kind::LongWords
                          ? 4
                          : Stride(c.kind);
    c.count = c.kind == Kind::Header ? 1 : c.raw_bytes / wireStride;
    // Native half/vector/scale node sampling can fetch the following key even
    // with zero interpolation weight. Preserve actual adjacent source bytes;
    // never synthesize endpoint padding or split original byte channels.
    if (i >= suffixStart + 4 &&
        (c.kind == Kind::Halves || c.kind == Kind::TransVectors ||
         c.kind == Kind::Scales)) {
      auto available =
          (limit - reinterpret_cast<std::uintptr_t>(c.raw)) / wireStride;
      if (c.count < available)
        ++c.count;
    }
    c.offset = Extend(nativeBytes, c.count ? c.count : 1, Stride(c.kind));
    c.bytes = c.count * Stride(c.kind);
  }
  GameNativeBackingReservation reservation(source, bytes, nativeBytes);
  auto *base = static_cast<unsigned char *>(reservation.Data());
  new (base) Storage{Marker, 1, children.size(), children[0].offset, mapOffset};
  auto *map = reinterpret_cast<StoredChild *>(base + mapOffset);
  for (std::size_t i = 0; i < children.size(); ++i) {
    auto &c = children[i];
    auto *data = c.kind == Kind::Bytes ? const_cast<unsigned char *>(c.raw)
                                       : base + c.offset;
    new (map + i) StoredChild{c.source, data, c.bytes};
    if (c.kind == Kind::Bytes)
      continue;
    if (c.kind == Kind::Header) {
      auto *h = new (data) cSAnim{};
      const auto hash = Word(c.raw + 4);
      std::memcpy(data + IdentifierLayout::HashOffset(), &hash, 4);
      h->m_nNumKeys = Word(c.raw + 8);
      h->m_nNumNodes = Word(c.raw + 12);
      h->m_nNumMorphChannels = Word(c.raw + 16);
      h->m_nNumRootKeys = Word(c.raw + 52);
      h->m_nHierarchySignature = Word(c.raw + 84);
      const auto speed = Word(c.raw + 80);
      std::memcpy(&h->m_fLinearSpeed, &speed, 4);
    } else
      for (std::size_t j = 0; j < c.count; ++j) {
        switch (c.kind) {
        case Kind::Words:
          new (data + j * 4) unsigned int(Word(c.raw + j * 4));
          break;
        case Kind::Halves:
          new (data + j * 2) unsigned short(Half(c.raw + j * 2));
          break;
        case Kind::LongWords:
          new (data +
               j * sizeof(unsigned long)) unsigned long(Word(c.raw + j * 4));
          break;
        case Kind::RotPointers:
        case Kind::VoidPointers: {
          using P = void *;
          new (data + j * sizeof(P)) P(nullptr);
          break;
        }
        case Kind::TransPointers: {
          using P = PackedTrans *;
          new (data + j * sizeof(P)) P(nullptr);
          break;
        }
        case Kind::ScalePointers: {
          using P = PackedScale *;
          new (data + j * sizeof(P)) P(nullptr);
          break;
        }
        case Kind::BytePointers: {
          using P = unsigned char *;
          new (data + j * sizeof(P)) P(nullptr);
          break;
        }
        case Kind::RootVectors: {
          auto *v = new (data + j * 12) nlVector3{};
          for (unsigned k = 0; k < 3; ++k) {
            const auto bits = Word(c.raw + j * 12 + k * 4);
            std::memcpy(&v->e[k], &bits, 4);
          }
          break;
        }
        case Kind::TransVectors: {
          auto *v = new (data + j * 12) PackedTrans{};
          float *members[] = {&v->x, &v->y, &v->z};
          for (unsigned k = 0; k < 3; ++k) {
            const auto bits = Word(c.raw + j * 12 + k * 4);
            std::memcpy(members[k], &bits, 4);
          }
          break;
        }
        case Kind::Scales: {
          auto *v = new (data + j * 6) PackedScale{};
          v->x = Half(c.raw + j * 6);
          v->y = Half(c.raw + j * 6 + 2);
          v->z = Half(c.raw + j * 6 + 4);
          break;
        }
        default:
          throw std::logic_error("SAnim transport kind is unrecognized");
        }
      }
  }
  reservation.Commit();
  if (!FindGameNativeBacking(source, bytes, backing))
    throw std::logic_error("SAnim native backing publication is absent");
  return View(source, bytes, backing);
}
NativeSAnimView NativeSAnimObjectView(const cSAnim *object, nlChunk *probe) {
  GameNativeBackingSourceSpan origin{};
  if (!FindGameNativeBackingSource(object, sizeof(cSAnim), probe, 8, origin))
    throw std::invalid_argument(
        "SAnim method has no retained original object/source backing");
  auto view = View(static_cast<nlChunk *>(const_cast<void *>(origin.source)),
                   origin.source_bytes, origin.backing);
  const auto &s = *static_cast<const Storage *>(view.data);
  if (reinterpret_cast<const unsigned char *>(object) !=
      static_cast<const unsigned char *>(view.data) + s.header_offset)
    throw std::invalid_argument(
        "SAnim member does not identify its original native header");
  return view;
}
void *NativeSAnimChunkData(const NativeSAnimView &view, nlChunk *child) {
  GameNativeBackingSpan backing{};
  if (!FindGameNativeBacking(view.source, view.source_bytes, backing) ||
      backing.data != view.data || backing.bytes != view.native_bytes ||
      backing.allocation.incarnation != view.incarnation)
    throw std::invalid_argument(
        "SAnim view has no live original source incarnation");
  View(view.source, view.source_bytes, backing);
  const auto &s = *static_cast<const Storage *>(view.data);
  const auto *map = reinterpret_cast<const StoredChild *>(
      static_cast<const unsigned char *>(view.data) + s.map_offset);
  for (std::size_t i = 0; i < s.count; ++i)
    if (map[i].source == child)
      return map[i].data;
  throw std::invalid_argument(
      "SAnim child is outside the original parser's native view");
}
} // namespace mscharged::platform
