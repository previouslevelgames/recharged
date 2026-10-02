#include "resources/texture_bundle.h"
#include <algorithm>
#include <set>

namespace mscharged::resources
{
std::vector<Texture> ReadTextureBundle(Bytes data)
{
    Require(data.size() <= MaximumAssetBytes, "RLT exceeds the static preview size limit");
    Require(U32(data, 0) == 0x50544c47, "Invalid RLT signature");
    const auto count = U32(data, 4);
    Require(count && count <= 4096, "Invalid RLT dictionary count");
    const auto dictionary = Slice(data, 16, std::size_t(count) * 16);
    const auto contents = Slice(data, 16 + dictionary.size(), data.size() - 16 - dictionary.size());
    const std::uint8_t formats[] = {4, 5, 14, 6, 1, 0, 1, 3, 9};
    std::vector<Texture> result;
    std::set<std::uint32_t> ids;
    std::size_t copied = 0;
    for (std::size_t i = 0; i < count; ++i)
    {
        Texture texture;
        texture.id = U32(dictionary, i * 16);
        Require(ids.insert(texture.id).second, "Duplicate RLT texture ID");
        const auto entry = Slice(contents, U32(dictionary, i * 16 + 4), U32(dictionary, i * 16 + 8));
        Require(U32(entry, 0) != 0x5f6c6669, "Animated RLT textures are not supported by the static preview");
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
        Require((copied += pixels.size() + colours.size()) <= MaximumAssetBytes, "RLT decoded data budget exceeded");
        texture.pixels.assign(pixels.begin(), pixels.end()); texture.palette.assign(colours.begin(), colours.end());
        result.push_back(std::move(texture));
    }
    return result;
}
}
