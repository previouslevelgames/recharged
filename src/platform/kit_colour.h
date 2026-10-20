#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

// The colour arithmetic of Mirror match's away kits (src/platform/away_kits.cpp):
// OKLab/OKLCh, HSV and the recolour of a kit colour to the away colour. Shared
// by the game module (the game's own textures) and the host (a texture pack's
// HD images of them, src/platform/hd_recolour.cpp), so both map colours alike.
// `C` is any colour type with float r, g, b members (sRGB, 0..1).
namespace mscharged::platform::kit_colour
{
struct Lab
{
    float L, a, b;
};

struct LchStats
{
    float L, C, h; // medians; h in radians
};

inline float ToLinear(float c) { return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f); }
inline float ToSrgb(float c) { return c <= 0.0031308f ? 12.92f * c : 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f; }

// OKLab (Björn Ottosson's published matrices).
template<class C> Lab ToLab(C c)
{
    float r = ToLinear(c.r), g = ToLinear(c.g), b = ToLinear(c.b);
    float l = std::cbrt(0.4122214708f * r + 0.5363325363f * g + 0.0514459929f * b);
    float m = std::cbrt(0.2119034982f * r + 0.6806995451f * g + 0.1073969566f * b);
    float s = std::cbrt(0.0883024619f * r + 0.2817188376f * g + 0.6299787005f * b);
    return {0.2104542553f * l + 0.7936177850f * m - 0.0040720468f * s,
        1.9779984951f * l - 2.4285922050f * m + 0.4505937099f * s,
        0.0259040371f * l + 0.7827717662f * m - 0.8086757660f * s};
}

// Linear sRGB; may lie outside 0..1.
inline void LabToLinear(Lab c, float out[3])
{
    float l = c.L + 0.3963377774f * c.a + 0.2158037573f * c.b;
    float m = c.L - 0.1055613458f * c.a - 0.0638541728f * c.b;
    float s = c.L - 0.0894841775f * c.a - 1.2914855480f * c.b;
    l = l * l * l;
    m = m * m * m;
    s = s * s * s;
    out[0] = 4.0767416621f * l - 3.3077115913f * m + 0.2309699292f * s;
    out[1] = -1.2684380046f * l + 2.6097574011f * m - 0.3413193965f * s;
    out[2] = -0.0041960863f * l - 0.7034186147f * m + 1.7076147010f * s;
}

// From lightness, chroma and hue (radians), reducing chroma until the colour
// fits sRGB.
template<class C> C FromLch(float L, float chroma, float h)
{
    L = std::clamp(L, 0.0f, 1.0f);
    float lo = 0.0f, hi = chroma;
    float rgb[3];
    for (int i = 0; i < 16; ++i)
    {
        float c = i == 0 ? chroma : 0.5f * (lo + hi);
        LabToLinear({L, c * std::cos(h), c * std::sin(h)}, rgb);
        bool inside = rgb[0] >= -0.0001f && rgb[0] <= 1.0001f && rgb[1] >= -0.0001f && rgb[1] <= 1.0001f
            && rgb[2] >= -0.0001f && rgb[2] <= 1.0001f;
        if (i == 0 && inside) break;
        if (inside)
            lo = c;
        else
            hi = c;
        if (i == 15) LabToLinear({L, lo * std::cos(h), lo * std::sin(h)}, rgb);
    }
    return {ToSrgb(std::clamp(rgb[0], 0.0f, 1.0f)), ToSrgb(std::clamp(rgb[1], 0.0f, 1.0f)),
        ToSrgb(std::clamp(rgb[2], 0.0f, 1.0f))};
}

template<class C> void ToHsv(C c, float& h, float& s, float& v)
{
    float mx = std::max({c.r, c.g, c.b}), mn = std::min({c.r, c.g, c.b});
    v = mx;
    s = mx > 0.0f ? (mx - mn) / mx : 0.0f;
    float d = mx - mn;
    if (d <= 0.0f)
        h = 0.0f;
    else if (mx == c.r)
        h = 60.0f * std::fmod((c.g - c.b) / d + 6.0f, 6.0f);
    else if (mx == c.g)
        h = 60.0f * ((c.b - c.r) / d + 2.0f);
    else
        h = 60.0f * ((c.r - c.g) / d + 4.0f);
}

// A kit colour as the away colour: the kit's median `from` becomes `to`.
template<class C> C Recolour(C colour, const LchStats& from, const LchStats& to)
{
    Lab c = ToLab(colour);
    float chroma = std::sqrt(c.a * c.a + c.b * c.b);
    // Shading darker than the kit's own colour scales towards black, so
    // shadows next to black lines stay dark; highlights share what lightness
    // remains above the new colour.
    float L;
    if (c.L <= from.L)
        L = from.L > 0.0f ? c.L * (to.L / from.L) : to.L;
    else
        L = to.L + (c.L - from.L) * (1.0f - to.L) / std::max(1.0f - from.L, 0.05f);
    float scaled = from.C > 0.01f ? chroma * (to.C / from.C) : to.C;
    return FromLch<C>(L, std::max(scaled, 0.0f), to.h);
}

// The away colour (0xRRGGBB) in OKLCh.
template<class C> LchStats Target(std::uint32_t rgb)
{
    Lab c = ToLab(C{((rgb >> 16) & 255) / 255.0f, ((rgb >> 8) & 255) / 255.0f, (rgb & 255) / 255.0f});
    return {c.L, std::sqrt(c.a * c.a + c.b * c.b), std::atan2(c.b, c.a)};
}
} // namespace mscharged::platform::kit_colour
