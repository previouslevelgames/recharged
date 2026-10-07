#include "platform/sanim_replay_projection.h"
#include "platform/game_allocation_ownership.h"
#include "platform/sanim_data_transport.h"
#include "Game/SAnim.h"
#include <dolphin/os.h>
#include <stdexcept>

namespace mscharged::platform {
namespace {
constexpr std::size_t RawHeaderBytes = 88;
void RequireRawHeader(const void* header, const GameNativeBackingSourceSpan& origin)
{
    GameCompletedSpan completed{};
    if (!FindGameCompletedSpan(header, RawHeaderBytes, completed)
        || completed.allocation.incarnation != origin.backing.allocation.incarnation
        || completed.allocation.base != origin.backing.allocation.base
        || FindGameByteDomain(header, RawHeaderBytes) != GameByteDomain::WiiSerialized)
        throw std::invalid_argument("Replay animation header has no retained serialized owner");
}
NativeSAnimView View(const GameNativeBackingSourceSpan& origin)
{
    return {static_cast<nlChunk*>(const_cast<void*>(origin.source)), origin.source_bytes,
            origin.backing.data, origin.backing.bytes, origin.backing.allocation.incarnation};
}
}
std::uint32_t EncodeSAnimReplayAddress(const cSAnim* animation)
{
    if (!animation) return 0;
    GameNativeBackingSourceSpan origin{};
    if (!FindGameNativeBackingSource(animation, sizeof(cSAnim), origin))
        throw std::invalid_argument("Replay animation has no live original native twin");
    auto view = NativeSAnimObjectView(animation, View(origin).source);
    auto* child = view.source->GetFirstChunk();
    auto* rawHeader = child->GetData();
    RequireRawHeader(rawHeader, origin);
    if (NativeSAnimChunkData(view, child) != animation)
        throw std::invalid_argument("Replay animation is not the original raw header projection");
    const auto physical = OSCachedToPhysical(rawHeader);
    if ((physical & 0xe0000001u) || OSPhysicalToCached(physical) != rawHeader)
        throw std::out_of_range("Replay animation header has no exact even cached word");
    return physical | 0x80000000u;
}
cSAnim* DecodeSAnimReplayAddress(std::uint32_t addressWord)
{
    if (!addressWord) return nullptr;
    if ((addressWord & 0xe0000001u) != 0x80000000u)
        throw std::out_of_range("Replay animation word is not an even Wii cached address");
    auto* rawHeader = OSPhysicalToCached(addressWord & 0x1fffffffu);
    GameNativeBackingSourceSpan origin{};
    if (!FindGameNativeBackingForSource(rawHeader, RawHeaderBytes, origin))
        throw std::invalid_argument("Replay animation word has no current raw/native owner");
    RequireRawHeader(rawHeader, origin);
    auto view = View(origin);
    auto* child = view.source->GetFirstChunk();
    if (child->GetData() != rawHeader)
        throw std::invalid_argument("Replay animation word does not identify the raw first header");
    auto* animation = static_cast<cSAnim*>(NativeSAnimChunkData(view, child));
    NativeSAnimObjectView(animation, view.source);
    return animation;
}
}
