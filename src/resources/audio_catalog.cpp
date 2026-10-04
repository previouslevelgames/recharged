#include "resources/audio_catalog.h"
#include "resources/chunk_reader.h"
#include <algorithm>
#include <limits>

namespace mscharged::resources
{
namespace
{
std::vector<Chunk> Children(Bytes file, Bytes parent)
{
    const auto start = std::size_t(parent.data() - file.data());
    const auto end = start + parent.size();
    std::vector<Chunk> result;
    for (auto offset = start; offset < end;)
    {
        Require(result.size() < 8192, "Audio container has too many chunks");
        const auto chunk = ReadChunk(file, offset, end);
        Require(chunk.next <= end, "Audio child padding exceeds its container");
        result.push_back(chunk);
        offset = chunk.next;
    }
    return result;
}
std::vector<Chunk> Section(Bytes file, std::uint32_t id)
{
    Require(file.size() <= MaximumAssetBytes, "Audio metadata exceeds 16 MiB");
    const auto root = ReadChunk(file, 0, file.size());
    Require(root.id == 0x80000001 && root.next == file.size(), "Invalid audio bundle root");
    Bytes found;
    for (const auto& chunk : Children(file, root.payload))
        if (chunk.id == id)
        {
            Require(found.data() == nullptr, "Duplicate audio metadata section");
            found = chunk.payload;
        }
    Require(found.data() != nullptr, "Required audio metadata section is absent");
    return Children(file, found);
}
Bytes Take(const std::vector<Chunk>& chunks, std::size_t& cursor, std::uint32_t id,
    std::size_t count, std::size_t stride)
{
    Require(cursor < chunks.size() && chunks[cursor].id == id, "Unexpected audio metadata chunk order");
    const auto bytes = chunks[cursor++].payload;
    Require(count <= 4096 && bytes.size() == count * stride, "Audio record count or extent is invalid");
    return bytes;
}
std::uint32_t SlotIndex(std::uint32_t address, std::uint32_t base, std::size_t count)
{
    Require(address >= base && (address - base) % 24 == 0, "Unaligned audio bank slot reference");
    const auto index = (address - base) / 24;
    Require(index < count, "Audio bank slot reference is out of bounds");
    return index;
}
std::string BankName(Bytes bytes)
{
    Require(!bytes.empty() && bytes.size() <= 128, "Invalid audio bank name length");
    const auto end = std::find(bytes.begin(), bytes.end(), 0);
    Require(end != bytes.begin() && end != bytes.end(), "Audio bank name is empty or unterminated");
    std::string result(bytes.begin(), end);
    // Names become relative resource paths. Reject directory traversal and host
    // path syntax before any future loader constructs audio/<name>.resbun.
    for (unsigned char c : result)
        Require((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')
            || c == '_' || c == '-' || c == ' ', "Unsupported audio bank filename");
    return result;
}
}
AudioBankCatalog::Handle ReadAudioBankCatalog(Bytes bytes)
{
    const auto chunks = Section(bytes, 0x80023500);
    std::size_t cursor = 0;
    const auto header = Take(chunks, cursor, 0x23501, 1, 24);
    const auto group_count = U32(header, 0), slot_count = U32(header, 8), name_count = U32(header, 16);
    Require(group_count && slot_count && name_count && slot_count <= 4096 && name_count <= 4096,
        "Audio bank table is empty or oversized");
    const auto old_slots = U32(header, 12);
    Require(old_slots % 4 == 0 && std::uint64_t(old_slots) + std::uint64_t(slot_count) * 24 <= 0x100000000ULL,
        "Serialized audio bank slot address range wraps");
    const auto groups = Take(chunks, cursor, 0x23502, group_count, 20);
    auto result = std::make_shared<AudioBankCatalog>();
    std::vector<std::uint32_t> owners(slot_count, std::numeric_limits<std::uint32_t>::max());
    for (std::uint32_t i = 0; i < group_count; ++i)
    {
        AudioBankGroupRecord group{U32(groups, i * 20), U32(groups, i * 20 + 4), {}};
        const auto count = U32(groups, i * 20 + 8);
        const auto refs = Take(chunks, cursor, 0x23503, count, 4);
        for (std::uint32_t j = 0; j < count; ++j)
        {
            const auto index = SlotIndex(U32(refs, j * 4), old_slots, slot_count);
            Require(owners[index] == std::numeric_limits<std::uint32_t>::max(), "Audio bank slot has multiple owners");
            owners[index] = i;
            group.slots.push_back(index);
        }
        result->groups.push_back(std::move(group));
    }
    const auto slots = Take(chunks, cursor, 0x23504, slot_count, 24);
    for (std::uint32_t i = 0; i < slot_count; ++i)
    {
        Require(owners[i] != std::numeric_limits<std::uint32_t>::max(), "Audio bank slot has no load owner");
        Require(slots[i * 24 + 21] <= 1, "Audio bank stream flag is invalid");
        result->slots.push_back({U32(slots, i * 24), U32(slots, i * 24 + 4), owners[i], slots[i * 24 + 21] != 0});
    }
    const auto names = Take(chunks, cursor, 0x23505, name_count, 8);
    for (std::uint32_t i = 0; i < name_count; ++i)
    {
        Require(cursor < chunks.size() && chunks[cursor].id == 0x23506, "Audio bank filename chunk is absent");
        result->names.push_back({U32(names, i * 8), BankName(chunks[cursor++].payload)});
    }
    Require(cursor == chunks.size(), "Unexpected trailing audio bank table records");
    return result;
}
AudioCueCatalog::Handle ReadAudioCueCatalog(Bytes bytes)
{
    const auto chunks = Section(bytes, 0x80023000);
    std::size_t cursor = 0;
    const auto header = Take(chunks, cursor, 0x23001, 1, 12);
    const auto count = U32(header, 0);
    const auto cues = Take(chunks, cursor, 0x23003, count, 20);
    Require(cursor == chunks.size(), "Unexpected trailing SoundMap records");
    auto result = std::make_shared<AudioCueCatalog>();
    for (std::uint32_t i = 0; i < count; ++i)
    {
        const auto offset = i * 20;
        const AudioCueKey key{U32(cues, offset), U32(cues, offset + 4), U32(cues, offset + 8), U32(cues, offset + 12)};
        Require(result->cues.emplace(key, U32(cues, offset + 16)).second, "Duplicate SoundMap cue key");
    }
    return result;
}
std::uint32_t AudioCueCatalog::Find(const AudioCueKey& key) const
{
    const auto it = cues.find(key);
    return it == cues.end() ? 0xffff : it->second;
}
}
