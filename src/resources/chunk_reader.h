#pragma once
#include "resources/binary_reader.h"

namespace mscharged::resources
{
struct Chunk { std::uint32_t id; Bytes payload; std::size_t next; };
// Alignment is relative to the entire decompressed file, including parent chunks.
inline Chunk ReadChunk(Bytes file, std::size_t offset, std::size_t end)
{
    Require(end <= file.size() && offset <= end && end - offset >= 8, "Truncated RLG chunk header");
    const auto raw = U32(file, offset), size = U32(file, offset + 4);
    const unsigned exponent = (raw >> 24) & 0x7f;
    Require(exponent <= 5, "Unsupported RLG alignment above the original 32-byte file alignment");
    Require(size <= end - offset - 8, "RLG chunk exceeds its container");
    const auto stop = offset + 8 + size;
    const auto start = Align(offset + 8, std::size_t(1) << exponent);
    Require(start <= stop, "RLG alignment exceeds its payload");
    return {raw & 0x80ffffff, Slice(file, start, stop - start), Align(stop, 4)};
}
}
