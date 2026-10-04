#include "resources/texture_bundle.h"
#include <algorithm>
#include <set>
#include <map>

namespace mscharged::resources
{
Texture ReadTexture(Bytes entry, std::uint32_t id)
{
    Require(entry.size() <= MaximumAssetBytes, "Texture exceeds the asset size limit");
    const std::uint8_t formats[] = {4, 5, 14, 6, 1, 0, 1, 3, 9};
    Texture texture; texture.id = id;
    const auto header = Slice(entry, 0, 32);
    const auto levels = U32(header, 0), format = U32(header, 4), palette = U32(header, 20);
    texture.width = U16(header, 14); texture.height = U16(header, 16);
    Require(texture.width && texture.width <= 1024 && texture.height && texture.height <= 1024, "Invalid RLT texture dimensions");
    unsigned max_levels = 1;
    for (unsigned size = std::max(texture.width, texture.height); size > 1; size >>= 1) ++max_levels;
    Require(levels && levels <= max_levels && format < std::size(formats), "Invalid RLT mip count or texture format");
    Require(!header[12], "Missing RLT texture cannot be displayed by the static preview");
    Require((format == 8 && palette && palette <= 256) || (format != 8 && !palette), "Invalid RLT palette count");
    texture.gx_format = formats[format]; texture.levels = levels; texture.palette_entries = palette;
    texture.game_format = format;
    std::copy_n(header.begin() + 8, 4, texture.bits.begin());
    std::size_t size = 0;
    for (unsigned level = 0; level < levels; ++level)
    {
        const auto w = std::max(1u, unsigned(texture.width) >> level), h = std::max(1u, unsigned(texture.height) >> level);
        const unsigned block_w = (format == 2 || format == 4 || format == 5 || format == 6 || format == 8) ? 8 : 4;
        const unsigned block_h = (format == 2 || format == 5) ? 8 : 4;
        size += ((w + block_w - 1) / block_w) * ((h + block_h - 1) / block_h) * (format == 3 ? 64 : 32);
    }
    const auto pixels = Slice(entry, 32, size), colours = Slice(entry, 32 + size, std::size_t(palette) * 2);
    texture.pixels.assign(pixels.begin(), pixels.end()); texture.palette.assign(colours.begin(), colours.end());
    return texture;
}
TextureBundle ReadTextureBundle(Bytes data, const std::vector<std::uint32_t>& selected)
{
    Require(data.size() <= MaximumAssetBytes, "RLT exceeds the static preview size limit");
    Require(U32(data, 0) == 0x50544c47, "Invalid RLT signature");
    const auto count = U32(data, 4);
    Require(count && count <= 4096, "Invalid RLT dictionary count");
    const auto dictionary = Slice(data, 16, std::size_t(count) * 16);
    const auto contents = Slice(data, 16 + dictionary.size(), data.size() - 16 - dictionary.size());
    TextureBundle result;
    std::map<std::uint32_t, Bytes> entries;
    std::set<std::uint32_t> ids;
    std::size_t copied = 0;
    for (std::size_t i = 0; i < count; ++i)
    {
        const auto id = U32(dictionary, i * 16);
        Require(ids.insert(id).second, "Duplicate RLT texture ID");
        entries.emplace(id, Slice(contents, U32(dictionary, i * 16 + 4), U32(dictionary, i * 16 + 8)));
    }
    std::set<std::uint32_t> required(selected.begin(), selected.end());
    if (selected.empty()) required = ids;
    for (auto id : required) Require(entries.count(id), "Requested texture is absent from RLT dictionary");
    // Keep authored dictionary order when constructing records. Dependencies are
    // included once, even when several animations share the same static frame.
    for (std::size_t i = 0; i < count; ++i)
    {
        const auto id = U32(dictionary, i * 16);
        if (!required.count(id)) continue;
        const auto entry = entries.at(id);
        if (U32(entry, 0) != 0x5f6c6669) continue;
        Slice(entry, 0, 36); // Fixed Wii header; never use native sizeof/pointers.
        Require(U32(entry, 4) == id, "RLT animation hash disagrees with its dictionary");
        const auto frames = U32(entry, 8), mode = U32(entry, 12);
        const auto direction = std::bit_cast<std::int32_t>(U32(entry, 16));
        Require(frames && frames <= 4096 && mode < 3 && direction >= -1 && direction <= 1
            && entry[20] <= 1, "Invalid RLT animation state");
        TextureAnimation animation{id, mode, direction, entry[20] != 0, F32(entry, 28), {}};
        Require(animation.elapsed >= 0, "Negative RLT animation elapsed time");
        const auto records = Slice(entry, 36, std::size_t(frames) * 8);
        Require((copied += std::size_t(frames) * sizeof(TextureAnimationFrame)) <= MaximumAssetBytes,
            "RLT decoded data budget exceeded");
        for (unsigned j = 0; j < frames; ++j)
        {
            const auto frame_id = U32(records, j * 8);
            const float duration = F32(records, j * 8 + 4);
            Require(duration >= 0, "Negative RLT animation frame duration");
            const auto frame = entries.find(frame_id);
            Require(frame != entries.end(), "RLT animation frame texture is missing");
            Require(U32(frame->second, 0) != 0x5f6c6669, "Nested RLT texture animations are unsupported");
            required.insert(frame_id);
            animation.frames.push_back({frame_id, duration});
        }
        result.animations.push_back(std::move(animation));
    }
    for (std::size_t i = 0; i < count; ++i)
    {
        Texture texture;
        texture.id = U32(dictionary, i * 16);
        if (!required.count(texture.id)) continue;
        const auto entry = entries.at(texture.id);
        if (U32(entry, 0) == 0x5f6c6669) continue;
        texture = ReadTexture(entry, texture.id);
        Require((copied += texture.pixels.size() + texture.palette.size()) <= MaximumAssetBytes, "RLT decoded data budget exceeded");
        result.textures.push_back(std::move(texture));
    }
    for (auto id : selected) Require(ids.count(id), "Requested texture is absent from RLT dictionary");
    return result;
}
}
