#include "platform/world_record_wire.h"
#include "platform/game_allocation_ownership.h"

#include <stdexcept>

namespace mscharged::platform {
std::uint32_t ReadWorldRecordWireWord(const void* source)
{
    GameCompletedSpan completed{};
    if (!source || !FindGameCompletedSpan(source, 4, completed)
        || FindGameByteDomain(source, 4) != GameByteDomain::WiiSerialized)
        throw std::invalid_argument("World record word requires completed Wii source bytes");
    const auto* p = static_cast<const unsigned char*>(source);
    return std::uint32_t(p[0]) << 24 | std::uint32_t(p[1]) << 16
        | std::uint32_t(p[2]) << 8 | std::uint32_t(p[3]);
}
}
