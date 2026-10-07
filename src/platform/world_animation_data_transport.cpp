#include "platform/world_animation_data_transport.h"
#include "platform/game_allocation_ownership.h"
#include "Game/World/WorldAnimObjects.h"

#include <cstdint>
#include <limits>
#include <new>
#include <stdexcept>
#include <type_traits>

namespace mscharged::platform {
namespace {
constexpr std::uint32_t Marker = 0x57415054; // typed world animation parent
struct Storage {
    std::uint32_t marker, version;
    std::size_t bindings, animations;
};
static_assert(sizeof(unsigned int) == 4 && sizeof(float) == 4);
static_assert(std::is_trivially_destructible_v<WorldAnimBinding>);
static_assert(sizeof(WorldAnimBinding) == 2 * sizeof(unsigned long));
static_assert(alignof(Storage) <= 8 && alignof(WorldAnimBinding) <= 8
              && alignof(unsigned long) <= 8);

std::uint32_t Word(const unsigned char* p)
{
    return std::uint32_t(p[0]) << 24 | std::uint32_t(p[1]) << 16
        | std::uint32_t(p[2]) << 8 | p[3];
}
std::size_t Add(std::size_t bytes, std::size_t count, std::size_t stride)
{
    if (count > (std::numeric_limits<std::size_t>::max() - bytes) / stride)
        throw std::overflow_error("World animation parent native extent overflows");
    return bytes + count * stride;
}
NativeWorldAnimParentView View(const void* source, std::size_t sourceBytes,
                               const GameNativeBackingSpan& backing,
                               std::size_t bindings, std::size_t animations)
{
    if (backing.bytes < sizeof(Storage))
        throw std::invalid_argument("World animation parent backing is incomplete");
    const auto& storage = *static_cast<const Storage*>(backing.data);
    if (storage.marker != Marker || storage.version != 1
        || storage.bindings != bindings || storage.animations != animations
        || backing.bytes != Add(Add(sizeof(Storage), bindings, sizeof(WorldAnimBinding)),
                                animations, sizeof(unsigned long)))
        throw std::invalid_argument("World animation parent has another typed backing profile");
    return {source, sourceBytes, backing.data, backing.bytes,
        static_cast<unsigned char*>(backing.data) + sizeof(Storage),
        backing.allocation.incarnation};
}
}

NativeWorldAnimParentView PrepareNativeWorldAnimParent(
    const void* originalParentData, int bindings, int animations)
{
    if (!originalParentData || bindings < 0 || animations < 0)
        throw std::invalid_argument("World animation parent arrays require bounded source extents");
    const auto payload = reinterpret_cast<std::uintptr_t>(originalParentData);
    if (payload < 0x20)
        throw std::invalid_argument("World animation parent prefix address underflows");
    const auto* source = reinterpret_cast<const unsigned char*>(payload - 0x20);
    GameCompletedSpan completed{};
    if (!FindGameCompletedSpan(source, 0x20, completed)
        || FindGameByteDomain(source, 0x20) != GameByteDomain::WiiSerialized)
        throw std::invalid_argument("World animation parent requires actual completed Wii source bytes");
    // GetParentData's original +0x20 is retained. Only this original parent
    // record supplies the array extent; no guessed containing-world scan.
    const auto sourceBytes = std::size_t(Word(source + 12));
    if (Word(source + 8) != 0x10 || sourceBytes < 0x20
        || !FindGameCompletedSpan(source, sourceBytes, completed)
        || FindGameByteDomain(source, sourceBytes) != GameByteDomain::WiiSerialized)
        throw std::invalid_argument("World animation parent record leaves its completed source owner");
    const auto bindingCount = std::size_t(bindings), animationCount = std::size_t(animations);
    const auto rawBytes = Add(Add(0x20, bindingCount, 8), animationCount, 4);
    if (rawBytes > sourceBytes)
        throw std::out_of_range("World animation arrays leave their authored parent record");
    GameNativeBackingSpan backing{};
    if (FindGameNativeBacking(source, sourceBytes, backing))
        return View(source, sourceBytes, backing, bindingCount, animationCount);
    const auto nativeBytes = Add(Add(sizeof(Storage), bindingCount, sizeof(WorldAnimBinding)),
                                 animationCount, sizeof(unsigned long));
    GameNativeBackingReservation reservation(source, sourceBytes, nativeBytes);
    auto* storage = new(reservation.Data()) Storage{Marker, 1, bindingCount, animationCount};
    auto* data = reinterpret_cast<unsigned char*>(storage) + sizeof(Storage);
    const auto* raw = static_cast<const unsigned char*>(originalParentData);
    for (std::size_t i = 0; i < bindingCount; ++i) {
        new(data + i * sizeof(WorldAnimBinding)) WorldAnimBinding{
            static_cast<unsigned long>(Word(raw + i * 8)),
            static_cast<unsigned long>(Word(raw + i * 8 + 4))};
    }
    const auto hashesAt = bindingCount * sizeof(WorldAnimBinding);
    const auto rawHashesAt = bindingCount * 8;
    for (std::size_t i = 0; i < animationCount; ++i)
        new(data + hashesAt + i * sizeof(unsigned long)) unsigned long(
            static_cast<unsigned long>(Word(raw + rawHashesAt + i * 4)));
    reservation.Commit();
    if (!FindGameNativeBacking(source, sourceBytes, backing))
        throw std::logic_error("World animation parent backing publication is absent");
    return View(source, sourceBytes, backing, bindingCount, animationCount);
}

void* NativeWorldAnimParentData(const NativeWorldAnimParentView& view)
{
    GameNativeBackingSpan backing{};
    if (!view.source || !FindGameNativeBacking(view.source, view.source_bytes, backing)
        || backing.data != view.backing || backing.bytes != view.native_bytes
        || backing.allocation.incarnation != view.incarnation)
        throw std::invalid_argument("World animation parent view has no live source incarnation");
    if (backing.bytes < sizeof(Storage))
        throw std::invalid_argument("World animation parent backing is incomplete");
    const auto& storage = *static_cast<const Storage*>(backing.data);
    const auto current = View(view.source, view.source_bytes, backing,
                              storage.bindings, storage.animations);
    if (current.data != view.data)
        throw std::invalid_argument("World animation parent view data is foreign");
    return current.data;
}
}
