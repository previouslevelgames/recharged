#pragma once

// A texture Mirror match recoloured in the game module (src/platform/away_kits.cpp).
// When texture packs are in use the host is told about each one, so it can
// recolour the pack's HD image of the original alike (src/runtime/texture_pack_loading.cpp).
// The pointers are valid only during the call.
extern "C" {
struct ChargedRecolouredTexture
{
    unsigned gx_format;                 // the texture's GX format: 5 RGB5A3, 6 RGBA8, 14 CMPR
    unsigned width, height;             // its largest level
    unsigned levels;                    // its mip levels
    const unsigned char* original;      // the largest level's Wii bytes before the recolour
    const unsigned char* recoloured;    // and after it
    unsigned long bytes;                // of each
    const unsigned char* mask;          // width x height texels: 1 where the away colour was applied
    const unsigned char* original_rgba; // width x height texels before the recolour, 8-bit sRGB and alpha
    float to[3];                        // the away colour in OKLCh: L, C, h (radians)
    int kind;                           // 0 kit texture, 1 team texture, 2 picture (painted portrait)
};
typedef void (*ChargedRecolouredTextureHook)(const ChargedRecolouredTexture* texture);
}
