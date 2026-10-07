#include "platform/transport_connection_address.h"
#include "platform/game_allocation_ownership.h"
#include "NL/plat/TransportConnection.h"
#include "NL/nlSlotPool.h"
#include <dolphin/os.h>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>

extern SlotPool<TransportConnection> gTransportConnectionPool;

namespace mscharged::platform {
namespace {
static_assert(sizeof(std::uint32_t) == 4);
static_assert(sizeof(TransportConnection) % alignof(SlotPoolEntry) == 0);
static_assert(alignof(TransportConnection) <= alignof(SlotPoolEntry));

void ValidateSourceSlot(const TransportConnection* connection)
{
    const auto address = reinterpret_cast<std::uintptr_t>(connection);
    if (address % alignof(TransportConnection))
        throw std::invalid_argument("Connection address is not native-slot aligned");
    GameAllocationSpan allocation{};
    if (!FindGameAllocationSpan(connection, sizeof(TransportConnection), allocation))
        throw std::invalid_argument("Connection has no live original allocation");

    // Source SlotPool stores its block header after its physical slots. For this
    // actual T, native sizeof already has the free-list alignment used by363.
    // Query those original owners directly, without registering another lifetime.
    std::size_t total = 0;
    bool slot = false;
    for (auto* block = gTransportConnectionPool.m_BlockList; block; block = block->next) {
        const std::size_t count = block->next ? gTransportConnectionPool.m_Delta
                                             : gTransportConnectionPool.m_Initial;
        if (!count || count > std::numeric_limits<std::size_t>::max() / sizeof(TransportConnection))
            throw std::invalid_argument("Original connection pool block is invalid");
        const auto bytes = count * sizeof(TransportConnection);
        const auto end = reinterpret_cast<std::uintptr_t>(block);
        if (end < bytes) throw std::invalid_argument("Original connection block address underflows");
        const auto start = end - bytes;
        if (address >= start && address < end && (address - start) % sizeof(TransportConnection) == 0) {
            if (reinterpret_cast<std::uintptr_t>(allocation.base) != start ||
                allocation.bytes < bytes + sizeof(SlotPoolBlock))
                throw std::invalid_argument("Connection slot is outside its original block owner");
            slot = true;
        }
        if (count > std::numeric_limits<std::size_t>::max() - total)
            throw std::length_error("Original connection slot count overflows");
        total += count;
    }
    if (!slot) throw std::invalid_argument("Address is not an original connection slot");
    std::size_t free = 0;
    for (auto* entry = gTransportConnectionPool.m_FreeList; entry; entry = entry->next) {
        if (++free > total) throw std::invalid_argument("Original connection free list exceeds its slots");
        if (reinterpret_cast<std::uintptr_t>(entry) == address)
            throw std::invalid_argument("Original connection slot has already been freed");
    }
}
}

std::uint32_t EncodeTransportConnectionAddress(const TransportConnection* connection)
{
    if (!connection) return 0;
    ValidateSourceSlot(connection);
    const auto physical = OSCachedToPhysical(const_cast<TransportConnection*>(connection));
    if ((physical & 0xe0000000u) || OSPhysicalToCached(physical) != connection)
        throw std::out_of_range("Connection has no canonical Wii cached address");
    return physical | 0x80000000u;
}

TransportConnection* DecodeTransportConnectionAddress(std::uint32_t word)
{
    if (!word) return nullptr;
    if ((word & 0xe0000000u) != 0x80000000u)
        throw std::out_of_range("Connection word is outside Wii cached memory");
    auto* connection = static_cast<TransportConnection*>(OSPhysicalToCached(word & 0x1fffffffu));
    ValidateSourceSlot(connection);
    return connection;
}
}
