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
std::vector<Texture> ReadTextureBundle(Bytes data);
}
