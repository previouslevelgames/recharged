#include "resources/sanim.h"
#include "resources/chunk_reader.h"
#include <algorithm>
#include <map>

namespace mscharged::resources
{
namespace
{
std::vector<std::array<float, 3>> Vectors(Bytes bytes)
{
    Require(bytes.size() % 12 == 0, "Invalid SAnim vector channel size");
    std::vector<std::array<float, 3>> result(bytes.size() / 12);
    for (std::size_t i = 0; i < result.size(); ++i)
        for (unsigned j = 0; j < 3; ++j) result[i][j] = F32(bytes, i * 12 + j * 4);
    return result;
}
std::vector<std::uint16_t> Halves(Bytes bytes)
{
    Require(bytes.size() % 2 == 0, "Invalid SAnim U16 channel size");
    std::vector<std::uint16_t> result(bytes.size() / 2);
    for (std::size_t i = 0; i < result.size(); ++i) result[i] = U16(bytes, i * 2);
    return result;
}
}
SAnimation ReadSAnimation(Bytes file, std::size_t offset, std::size_t end)
{
    Require(file.size() <= MaximumAssetBytes, "SAnim exceeds the 16 MiB asset limit");
    const auto root = ReadChunk(file, offset, end);
    Require(root.id == 0x80017000 && root.next == end, "Invalid SAnim root or range");
    auto cursor = std::size_t(root.payload.data() - file.data());
    const auto stop = cursor + root.payload.size();
    auto next = [&](unsigned id) {
        const auto c = ReadChunk(file, cursor, stop);
        Require(c.id == id && c.next <= stop, "Missing, reordered or invalid SAnim chunk");
        cursor = c.next; return c.payload;
    };
    const auto header = next(0x17001);
    Require(header.size() == 88, "Invalid 88-byte Wii SAnim header");
    SAnimation result;
    result.hash = U32(header, 4); result.frames = U32(header, 8);
    const auto nodes = U32(header, 12), morphs = U32(header, 16), roots = U32(header, 52);
    result.hierarchy_signature = U32(header, 84);
    Require(result.frames > 0 && result.frames <= MaximumSAnimKeys && nodes <= MaximumSAnimNodes
        && morphs <= MaximumSAnimMorphs && roots <= MaximumSAnimKeys, "SAnim counts exceed the selected native profile");
    const auto name = next(0x17002);
    const auto nul = std::find(name.begin(), name.end(), 0);
    Require(nul != name.end() && nul != name.begin() && nul - name.begin() <= 255,
            "SAnim name is empty, unterminated or too long");
    Require(std::all_of(name.begin(), nul, [](auto c) { return c >= 32 && c < 127; })
        && name.size() == Align(std::size_t(nul - name.begin()) + 1, 4)
        && std::all_of(nul, name.end(), [](auto c) { return c == 0; }), "Invalid SAnim name encoding/padding");
    result.name.assign(name.begin(), nul);
    const auto weight_counts = next(0x17110), auxiliary_metadata = next(0x17113);
    Require(weight_counts.size() == nodes * 4 && auxiliary_metadata.size() == nodes * 4,
            "SAnim channel metadata size disagrees with its node count");
    // These five exported pointer arrays are loader scratch, never file offsets.
    for (unsigned id : {0x17004u, 0x17005u, 0x17006u, 0x17111u, 0x17114u})
        Require(next(id).size() == nodes * 4, "Invalid Wii SAnim pointer table size");
    const auto root_rotation = next(0x17007), root_translation = next(0x17008);
    Require(root_rotation.size() == roots * 2 && root_translation.size() == roots * 12,
            "SAnim root channel size disagrees with its key count");
    result.root_rotation = Halves(root_rotation); result.root_translation = Vectors(root_translation);
    std::vector<std::map<unsigned, Bytes>> node_chunks(nodes);
    for (unsigned i = 0; i < nodes; ++i)
    {
        const auto node = next(0x80017100);
        auto at = std::size_t(node.data() - file.data()); const auto node_end = at + node.size();
        while (at < node_end)
        {
            const auto c = ReadChunk(file, at, node_end);
            Require(c.next <= node_end && (c.id == 0x17101 || c.id == 0x17102 || c.id == 0x17103
                || c.id == 0x17112 || c.id == 0x17115), "Unsupported SAnim node channel");
            Require(c.id == 0x17115 || !c.payload.empty(), "Empty authored SAnim key channel");
            Require(node_chunks[i].emplace(c.id, c.payload).second, "Duplicate SAnim node channel");
            at = c.next;
        }
    }
    const auto morph_counts = next(0x17009), morph_ids = next(0x1700a), morph_keys = next(0x1700b);
    const auto properties = next(0x17003);
    Require(cursor == stop, "Unexpected SAnim chunk or trailing data");
    Require(properties.size() == nodes * 4 && morph_counts.size() == morphs * 4 && morph_ids.size() == morphs * 4,
            "SAnim property/morph table size differs from its count");
    std::size_t total_morph_keys = 0;
    for (unsigned i = 0; i < morphs; ++i)
    {
        const auto count = U32(morph_counts, i * 4);
        Require(count > 0 && count <= MaximumSAnimKeys, "Invalid SAnim morph key count");
        Require(count <= morph_keys.size() - total_morph_keys, "SAnim morph keys exceed their channel");
        total_morph_keys += count; result.morph_counts.push_back(count); result.morph_ids.push_back(U32(morph_ids, i * 4));
    }
    Require(total_morph_keys == morph_keys.size(), "SAnim morph key counts leave unused bytes");
    result.morph_keys.assign(morph_keys.begin(), morph_keys.end());
    result.nodes.resize(nodes);
    for (unsigned i = 0; i < nodes; ++i)
    {
        auto& node = result.nodes[i]; node.properties = U32(properties, i * 4);
        node.auxiliary_metadata = U32(auxiliary_metadata, i * 4);
        auto get = [&](unsigned id) { const auto f = node_chunks[i].find(id); return f == node_chunks[i].end() ? Bytes{} : f->second; };
        const auto rot = get(0x17101), trans = get(0x17102), scale = get(0x17103), weights = get(0x17112);
        const auto rot_count = node.properties & 2 ? 1 : result.frames;
        const auto rot_width = node.properties & 1 ? 2 : node.properties & 0x10 ? 8 : node.properties & 0x20 ? 6 : 4;
        for (auto [bytes, width] : {std::pair{rot, std::size_t(rot_count * rot_width)},
                std::pair{trans, std::size_t((node.properties & 4 ? 1 : result.frames) * 12)},
                std::pair{scale, std::size_t((node.properties & 8 ? 1 : result.frames) * 6)}})
            Require(bytes.empty() || bytes.size() == width, "SAnim node keys disagree with their property flags/frame count");
        const auto weights_count = U32(weight_counts, i * 4);
        Require(weights_count <= MaximumSAnimKeys && weights.size() == weights_count,
                "SAnim weight keys disagree with their count");
        if (node.properties & 1) node.angles = Halves(rot);
        else node.rotation.assign(rot.begin(), rot.end());
        node.translation = Vectors(trans);
        node.scale.resize(scale.size() / 6);
        for (std::size_t k = 0; k < node.scale.size(); ++k)
            for (unsigned component = 0; component < 3; ++component)
                node.scale[k][component] = U16(scale, k * 6 + component * 2);
        node.weights.assign(weights.begin(), weights.end());
        const auto auxiliary = get(0x17115);
        // No selected source interprets this payload or its metadata. Retain
        // bounded bytes without inventing a record format or sampling behavior.
        node.auxiliary.assign(auxiliary.begin(), auxiliary.end());
        node.auxiliary_present = node_chunks[i].contains(0x17115);
    }
    return result;
}
std::vector<SAnimation> ReadSAnimations(Bytes file)
{
    Require(!file.empty() && file.size() <= MaximumAssetBytes, "SAnim inventory is empty or exceeds 16 MiB");
    std::vector<SAnimation> result;
    for (std::size_t at = 0; at < file.size();)
    {
        Require(result.size() < MaximumSAnimTracks, "SAnim inventory exceeds 256 tracks");
        const auto c = ReadChunk(file, at, file.size());
        Require(c.next <= file.size(), "SAnim root padding exceeds the inventory");
        result.push_back(ReadSAnimation(file, at, c.next)); at = c.next;
    }
    return result;
}
}
