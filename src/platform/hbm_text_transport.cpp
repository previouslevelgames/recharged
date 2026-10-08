#include "platform/hbm_text_transport.h"
#include "platform/game_allocation_ownership.h"
#include <cstdint>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace mscharged::platform {
void* PrepareNativeHBMMessage(void* message) {
    GameCompletedSpan completed{};
    if (!message || reinterpret_cast<std::uintptr_t>(message) % alignof(std::uint16_t)
        || !FindGameCompletedSpan(message, 2, completed) || completed.base != message
        || completed.bytes < 2 || completed.bytes % 2)
        throw std::invalid_argument("HBM text requires a complete aligned BE16 file owner");
    const auto bytes = completed.bytes;
    GameAllocationSpan allocation{};
    if (bytes > std::numeric_limits<std::size_t>::max() - 2
        || !FindGameAllocationSpan(message, bytes + 2, allocation)
        || allocation.incarnation != completed.allocation.incarnation)
        throw std::out_of_range("HBM text lacks its original source-zeroed terminator storage");
    const auto* raw = static_cast<const unsigned char*>(message);
    // These two caller-initialized padding bytes are checked, not published as
    // authored/completed file data. Actual Game/HBMManager memset/read owns them.
    if (raw[bytes] || raw[bytes + 1])
        throw std::invalid_argument("HBM text source-zeroed terminator is absent");
    const auto domain = FindGameByteDomain(message, bytes);
    if (domain == GameByteDomain::NativePayload) {
        std::uint16_t bom; std::memcpy(&bom, message, sizeof(bom));
        if (bom != 0xfeff)
            throw std::invalid_argument("HBM native text lacks the retained Wii16 BOM");
        return message;
    }
    if (domain != GameByteDomain::WiiSerialized || raw[0] != 0xfe || raw[1] != 0xff)
        throw std::invalid_argument("HBM text requires its completed BOM-bearing BE16 profile");
    GameByteWriteReservation written(message, bytes);
    if (!written.Tracked())
        throw std::logic_error("HBM text lost its actual completed allocation");
    auto* destination = static_cast<unsigned char*>(message);
    for (std::size_t i = 0; i < bytes; i += 2) {
        const std::uint16_t cell = std::uint16_t(std::uint16_t(destination[i]) << 8
                                              | destination[i + 1]);
        std::memcpy(destination + i, &cell, sizeof(cell));
    }
    written.Complete(GameByteDomain::NativePayload);
    return message;
}
}
