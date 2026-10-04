#pragma once
#include "resources/binary_reader.h"
#include <array>
#include <vector>

namespace mscharged::resources
{
struct Texture
{
    std::uint32_t id = 0;
    std::uint16_t width = 0, height = 0;
    std::uint8_t gx_format = 0, levels = 0;
    std::uint8_t game_format = 0;
    std::array<std::uint8_t, 4> bits{};
    std::uint16_t palette_entries = 0;
    // Pixel tiles and RGB5A3 palette words retain their original Wii byte order.
    std::vector<std::uint8_t> pixels, palette;
};
struct TextureAnimationFrame { std::uint32_t texture = 0; float duration = 0; };
struct TextureAnimation
{
    std::uint32_t id = 0, mode = 0;
    std::int32_t direction = 0;
    bool paused = false;
    float elapsed = 0;
    std::vector<TextureAnimationFrame> frames;
};
struct TextureBundle
{
    std::vector<Texture> textures;
    std::vector<TextureAnimation> animations;
};
// A single original PlatTexture record (also used by font .res bundles).
Texture ReadTexture(Bytes data, std::uint32_t id);
// Selected animations include their static frame dependencies. Animation-to-
// animation references are rejected; original bundles register static frames first.
TextureBundle ReadTextureBundle(Bytes data, const std::vector<std::uint32_t>& selected = {});
}
