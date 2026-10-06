#if !defined(MSCHARGED_NATIVE) || !defined(MSCHARGED_GAME_MODULE)
#error Camera word transport belongs beneath the original game-module loader
#endif

#include "platform/camera_data_transport.h"
#include "platform/game_allocation_ownership.h"
#include "platform/host_metadata.h"
#include "NL/nlChunk.h"

#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <list>
#include <new>
#include <stdexcept>

namespace mscharged::platform
{
namespace
{
static_assert(sizeof(nlChunk) == 8 && alignof(nlChunk) == 4);
static_assert(sizeof(float) == 4 && std::numeric_limits<float>::is_iec559);
static_assert(std::endian::native == std::endian::little
    || std::endian::native == std::endian::big);

template<class T> struct HostAllocator
{
    using value_type = T;
    HostAllocator() = default;
    template<class U> HostAllocator(const HostAllocator<U>&) noexcept {}
    T* allocate(std::size_t count)
    {
        static_assert(alignof(T) <= alignof(std::max_align_t));
        if (count > std::numeric_limits<std::size_t>::max() / sizeof(T))
            throw std::bad_alloc();
        return static_cast<T*>(ChargedNativeMetadataAllocate(count * sizeof(T)));
    }
    void deallocate(T* pointer, std::size_t) noexcept
    {
        ChargedNativeMetadataRelease(pointer);
    }
    template<class U> bool operator==(const HostAllocator<U>&) const noexcept { return true; }
};

struct WordPlan
{
    void* address;
    std::size_t bytes;
    GameByteDomain target;
    GameByteWriteReservation ticket;
    WordPlan(void* value, std::size_t count, GameByteDomain domain)
        : address(value), bytes(count), target(domain) {}
};
using Plans = std::list<WordPlan, HostAllocator<WordPlan>>;

void RequireCompleted(const void* pointer, std::size_t bytes)
{
    GameCompletedSpan span;
    if (bytes && !FindGameCompletedSpan(pointer, bytes, span))
        throw std::invalid_argument("Camera words are outside a completed source byte span");
}

void PlanWords(Plans& plans, void* address, std::size_t bytes, GameByteDomain target)
{
    if (!bytes) return;
    RequireCompleted(address, bytes);
    const auto current = FindGameByteDomain(address, bytes);
    if (current == target) return;
    if (current != GameByteDomain::WiiSerialized)
        throw std::invalid_argument("Camera words have an incompatible native byte domain");
    plans.emplace_back(address, bytes, target);
}

bool CameraFloatWords(std::uint32_t type)
{
    // Original loader's serialized position/target/quaternion/FOV/focal fields
    // are IEEE binary32 arrays. Ancillary channels and names stay untouched.
    return type == 0x25003 || type == 0x25006 || type == 0x25004
        || type == 0x25009 || type == 0x2500A;
}
}

void PrepareCameraChunkRange(nlChunk* first, nlChunk* end)
{
    const auto begin = reinterpret_cast<std::uintptr_t>(first);
    const auto limit = reinterpret_cast<std::uintptr_t>(end);
    if (begin == limit) return;
    if (limit < begin || begin % alignof(nlChunk) != 0)
        throw std::invalid_argument("Invalid native camera chunk range");
    RequireCompleted(first, limit - begin);
    Plans plans;
    auto* chunk = first;
    while (reinterpret_cast<std::uintptr_t>(chunk) < limit)
    {
        const auto address = reinterpret_cast<std::uintptr_t>(chunk);
        if (limit - address < sizeof(nlChunk))
            throw std::invalid_argument("Truncated camera chunk header");
        const auto id = ReadGameChunkWord(chunk, 0);
        const auto bytes = ReadGameChunkWord(chunk, 1);
        if (bytes > limit - address - sizeof(nlChunk))
            throw std::invalid_argument("Camera chunk exceeds its original range");
        RequireCompleted(chunk, sizeof(nlChunk) + std::size_t(bytes));
        // Header size is bounded before using the original pointer/alignment
        // getters. Domain conversion never changes those getters' decisions.
        auto* data = static_cast<std::byte*>(chunk->GetData());
        const auto payload = reinterpret_cast<std::uintptr_t>(data);
        const auto unaligned = address + sizeof(nlChunk);
        if (payload < unaligned || payload - unaligned > bytes)
            throw std::invalid_argument("Camera alignment exceeds serialized payload");
        const auto payloadBytes = std::size_t(bytes) - (payload - unaligned);
        const auto type = id & 0x80FFFFFF;
        PlanWords(plans, chunk, sizeof(nlChunk), GameByteDomain::NativeHeader);
        if (type == 0x2500C)
        {
            if (payloadBytes < 4)
                throw std::invalid_argument("Truncated camera key-count word");
            PlanWords(plans, data, 4, GameByteDomain::NativePayload);
        }
        else if (CameraFloatWords(type))
        {
            // Convert complete serialized float words, preserving any unused
            // bytes/tails. Counts and original copying stay in game source.
            PlanWords(plans, data, payloadBytes / 4 * 4, GameByteDomain::NativePayload);
        }
        auto* next = chunk->GetNextChunk();
        const auto following = reinterpret_cast<std::uintptr_t>(next);
        if (following <= address || following > limit)
            throw std::invalid_argument("Camera chunk padding exceeds its original range");
        chunk = next;
    }
    // Reserve every native metadata node before modifying a source byte. This
    // uses host C storage; it makes no original game allocation or ownership.
    for (auto& plan : plans)
    {
        plan.ticket = GameByteWriteReservation(plan.address, plan.bytes);
        if (!plan.ticket.Tracked())
            throw std::invalid_argument("Camera conversion lost its source allocation");
    }
    for (auto& plan : plans)
    {
        if constexpr (std::endian::native == std::endian::little)
        {
            auto* words = static_cast<std::byte*>(plan.address);
            for (std::size_t offset = 0; offset < plan.bytes; offset += 4)
            {
                std::uint32_t value;
                std::memcpy(&value, words + offset, 4);
                value = ((value & 0x000000FFu) << 24) | ((value & 0x0000FF00u) << 8)
                    | ((value & 0x00FF0000u) >> 8) | ((value & 0xFF000000u) >> 24);
                std::memcpy(words + offset, &value, 4);
            }
        }
        plan.ticket.Complete(plan.target);
    }
}
}
