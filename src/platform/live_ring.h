#pragma once

// A network match's live stream on its way out of the game module
// (match_replay.cpp): the game's thread puts whole messages in, the host's
// sender (online_replays.h) takes them out, and neither ever waits for the
// other. One producer, one consumer, a fixed buffer: a message that does not
// fit is refused whole, never half written, and the game goes on.
//
// A message is its kind (u8), the stream it belongs to (u32: the module counts
// the network matches it records) and its size (u32), little-endian, then its
// bytes.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace mscharged::replay
{
enum LiveKind : std::uint8_t
{
    kLiveBegin = 1, // a game's stream begins: LiveBegin
    kLiveData = 2,  // its next bytes: its replay stream (match_replay_stream.h) as it is written
    kLiveEnd = 3,   // its stream is complete
};

// What a game's stream begins with: the game of the series it belongs to.
struct LiveBegin
{
    std::uint32_t module_series = 0; // the module's count (ChargedOnlineMatchSnapshot::series)
    std::uint32_t game = 0;          // from 1
    std::uint32_t profile_id = 0;    // the WFC profile playing it
};

inline constexpr std::size_t kLiveHeaderSize = 9;
// A data message at most, so that the host's buffer always takes one.
inline constexpr std::size_t kLiveMostData = 16u << 10;

template <std::size_t Capacity>
class LiveRing
{
    static_assert(Capacity && (Capacity & (Capacity - 1)) == 0, "the capacity is a power of two");

public:
    // The producer: the whole message, or false (nothing written) when the
    // ring has no room for it.
    bool Put(std::uint8_t kind, std::uint32_t stream, const void* data, std::uint32_t size)
    {
        const std::uint64_t head = head_.load(std::memory_order_relaxed);
        const std::uint64_t tail = tail_.load(std::memory_order_acquire);
        if (size > kLiveMostData || Capacity - (head - tail) < kLiveHeaderSize + size) return false;
        std::uint8_t header[kLiveHeaderSize] = {kind};
        for (int n = 0; n < 4; ++n)
        {
            header[1 + n] = std::uint8_t(stream >> (8 * n));
            header[5 + n] = std::uint8_t(size >> (8 * n));
        }
        CopyIn(head, header, kLiveHeaderSize);
        if (size) CopyIn(head + kLiveHeaderSize, data, size);
        head_.store(head + kLiveHeaderSize + size, std::memory_order_release);
        return true;
    }

    // The consumer: whole messages, as many as fit in `capacity` (at least
    // kLiveHeaderSize + kLiveMostData takes any); the bytes copied.
    std::size_t Take(std::uint8_t* out, std::size_t capacity)
    {
        std::uint64_t tail = tail_.load(std::memory_order_relaxed);
        const std::uint64_t head = head_.load(std::memory_order_acquire);
        std::size_t copied = 0;
        while (head - tail >= kLiveHeaderSize)
        {
            std::uint8_t header[kLiveHeaderSize];
            CopyOut(tail, header, kLiveHeaderSize);
            const std::uint32_t size = std::uint32_t(header[5]) | std::uint32_t(header[6]) << 8
                | std::uint32_t(header[7]) << 16 | std::uint32_t(header[8]) << 24;
            const std::size_t whole = kLiveHeaderSize + size;
            if (copied + whole > capacity) break;
            CopyOut(tail, out + copied, whole);
            copied += whole;
            tail += whole;
        }
        tail_.store(tail, std::memory_order_release);
        return copied;
    }

private:
    void CopyIn(std::uint64_t at, const void* data, std::size_t size)
    {
        const std::size_t start = std::size_t(at & (Capacity - 1));
        const std::size_t first = size < Capacity - start ? size : Capacity - start;
        std::memcpy(data_ + start, data, first);
        std::memcpy(data_, static_cast<const std::uint8_t*>(data) + first, size - first);
    }

    void CopyOut(std::uint64_t at, std::uint8_t* out, std::size_t size) const
    {
        const std::size_t start = std::size_t(at & (Capacity - 1));
        const std::size_t first = size < Capacity - start ? size : Capacity - start;
        std::memcpy(out, data_ + start, first);
        std::memcpy(out + first, data_, size - first);
    }

    alignas(64) std::atomic<std::uint64_t> head_{0}; // written up to (the producer's)
    alignas(64) std::atomic<std::uint64_t> tail_{0}; // read up to (the consumer's)
    alignas(64) std::uint8_t data_[Capacity];
};

// One message of what LiveRing::Take copied; false once `bytes` has none left
// whole.
struct LiveMessage
{
    std::uint8_t kind = 0;
    std::uint32_t stream = 0;
    const std::uint8_t* data = nullptr;
    std::uint32_t size = 0;
};

inline bool NextLiveMessage(const std::uint8_t* bytes, std::size_t size, std::size_t& at, LiveMessage& message)
{
    if (size - at < kLiveHeaderSize) return false;
    const std::uint8_t* header = bytes + at;
    message.kind = header[0];
    message.stream = std::uint32_t(header[1]) | std::uint32_t(header[2]) << 8 | std::uint32_t(header[3]) << 16
        | std::uint32_t(header[4]) << 24;
    message.size = std::uint32_t(header[5]) | std::uint32_t(header[6]) << 8 | std::uint32_t(header[7]) << 16
        | std::uint32_t(header[8]) << 24;
    if (size - at - kLiveHeaderSize < message.size) return false;
    message.data = header + kLiveHeaderSize;
    at += kLiveHeaderSize + message.size;
    return true;
}
} // namespace mscharged::replay
