#pragma once

#include <cstdint>
#include <string>
#include <vector>

// Mirror match's away kits in HD: a texture pack's image of a texture the game
// module recoloured (src/platform/away_kits.cpp) is recoloured alike. Which
// texels are kit is decided on the game's own texture, where the away kit's
// colour bands are tuned; the mask is scaled up and its edges follow the HD
// image's own colours. Each kit pixel gets the away colour with its own
// shading.
namespace mscharged::platform
{
struct RgbaImage
{
    std::uint32_t width = 0, height = 0;
    std::vector<std::uint8_t> rgba; // 8-bit sRGB and alpha, rows top to bottom
};

struct KitRecolour
{
    std::uint32_t width = 0, height = 0; // the game's texture (largest level)
    std::vector<std::uint8_t> mask;      // width x height: 1 where it was recoloured
    std::vector<std::uint8_t> rgba;      // width x height: its texels before
    float to[3] = {};                    // the away colour in OKLCh
    // A painted picture: when the pack drew it anew (another framing), its
    // kit is found by colour instead.
    bool picture = false;
};

// Recolours `image`, the pack's HD image of that texture. False (and `image`
// unchanged, `why` set) when it does not fit: another shape, a picture that
// differs from the game's, or a kit area without the kit's colour (a
// greyscale image). A picture whose kit lies elsewhere (drawn anew) is
// recoloured by the kit's colour alone; `why` then says so. Up to `threads`
// threads work on it (the calling one among them), with 0 as many as the
// machine has; the result is the same.
bool RecolourHdImage(const KitRecolour& kit, RgbaImage& image, std::string* why = nullptr, unsigned threads = 1);

// A DDS file of the image (uncompressed RGBA8), with its full mip chain made
// by box filtering when `mips` is set.
std::vector<std::uint8_t> EncodeDds(const RgbaImage& image, bool mips);
}
