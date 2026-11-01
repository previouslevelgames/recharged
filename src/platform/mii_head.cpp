#include "platform/mii_head.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>

// The Mii library's drawing, step by step as RFL_Icon.c, RFL_Model.c and
// RFL_MakeTex.c set up the GX, with a small CPU rasterizer in place of the
// GX: the face texture (eyes, eyebrows, mouth, mustache, mole) composed in
// 2D, then the head model drawn in 3D with the lit opaque parts and the
// textured translucent ones. The GX behaviour these steps rely on is kept:
// 8-bit TEV arithmetic, bilinear texture filtering, the 6-bit colour buffer
// (GX_PF_RGBA6_Z24), blending, the alpha test, the 24-bit depth test, and
// the RGB5A3 copies of the face texture and the icon.
namespace mscharged::platform::mii {
namespace {
std::uint16_t BE16(const std::uint8_t* p) { return std::uint16_t((p[0] << 8) | p[1]); }
std::uint32_t BE32(const std::uint8_t* p)
{
    return (std::uint32_t(p[0]) << 24) | (std::uint32_t(p[1]) << 16) | (std::uint32_t(p[2]) << 8) | p[3];
}

[[noreturn]] void Damaged(const std::string& what) { throw std::invalid_argument(what); }

// ---------------------------------------------------------------- records

// Where each face value sits in the record: a big-endian 16-bit word, its
// lowest bit and its width (RFLiCharData's bit fields, as the Wii lays them).
struct FieldBits {
    std::size_t offset;
    int shift, width;
};
constexpr FieldBits kFieldBits[] = {
    {0x00, 1, 4},                                                                   // favorite_color
    {0x20, 13, 3}, {0x20, 10, 3}, {0x20, 6, 4},                                     // face
    {0x22, 9, 7}, {0x22, 6, 3}, {0x22, 5, 1},                                       // hair
    {0x24, 11, 5}, {0x26, 13, 3}, {0x26, 9, 4}, {0x24, 6, 5}, {0x26, 0, 4}, {0x26, 4, 5}, // eyebrow
    {0x28, 10, 6}, {0x2A, 13, 3}, {0x2A, 9, 4}, {0x28, 5, 5}, {0x2A, 5, 4}, {0x28, 0, 5}, // eye
    {0x2C, 12, 4}, {0x2C, 8, 4}, {0x2C, 3, 5},                                      // nose
    {0x2E, 11, 5}, {0x2E, 9, 2}, {0x2E, 5, 4}, {0x2E, 0, 5},                        // mouth
    {0x30, 12, 4}, {0x30, 9, 3}, {0x30, 5, 4}, {0x30, 0, 5},                        // glasses
    {0x32, 14, 2}, {0x32, 12, 2}, {0x32, 9, 3}, {0x32, 5, 4}, {0x32, 0, 5},         // mustache, beard
    {0x34, 15, 1}, {0x34, 11, 4}, {0x34, 1, 5}, {0x34, 6, 5},                       // mole
};
static_assert(std::size(kFieldBits) == 39);

// ---------------------------------------------------------------- tables

// RFLiArcID.
enum Archive {
    kBeard, kEye, kEyebrow, kFaceline, kFaceTex, kForehead, kGlass, kGlassTex, kHair, kMask, kMole, kMouth,
    kMustache, kNose, kNoseline, kNoselineTex, kCap, kCapTex, kArchiveCount
};

struct Rgb {
    int r, g, b;
};
// RFL_Model.c.
constexpr Rgb kFacelineColor[] = {{240, 216, 196}, {255, 188, 128}, {216, 136, 80},
                                  {255, 176, 144}, {152, 80, 48},   {82, 46, 28}};
constexpr Rgb kHairColor[] = {{30, 26, 24},    {56, 32, 21}, {85, 38, 23},  {112, 64, 36},
                              {114, 114, 120}, {73, 54, 26}, {122, 89, 40}, {193, 159, 100}};
constexpr Rgb kGlassColor[] = {{16, 16, 16}, {96, 56, 16}, {152, 24, 16}, {32, 48, 96}, {144, 88, 0}, {96, 88, 80}};
constexpr Rgb kFavoriteColor[] = {{184, 64, 48},  {240, 120, 40}, {248, 216, 32}, {128, 200, 40},
                                  {0, 116, 40},   {32, 72, 152},  {64, 160, 216}, {232, 96, 120},
                                  {112, 44, 168}, {72, 56, 24},   {224, 224, 224}, {24, 24, 20}};
// RFLi_Config.h; the beard and eyebrow colours are the hair colours.
constexpr Rgb kMoleColor = {18, 15, 15};
constexpr Rgb kEyeColor1[] = {{0, 0, 0}, {124, 128, 128}, {112, 80, 64}, {112, 110, 64}, {88, 104, 184}, {72, 128, 104}};
constexpr Rgb kMouthColor0[] = {{190, 78, 38}, {216, 48, 40}, {207, 68, 71}};
constexpr Rgb kMouthColor1[] = {{113, 42, 4}, {120, 21, 16}, {126, 37, 40}};
constexpr int kEyeRotOffset[50] = {29, 28, 28, 28, 29, 28, 28, 28, 29, 28, 28, 28, 28, 29, 29, 28, 28,
                                   28, 29, 29, 28, 29, 28, 29, 29, 28, 29, 28, 28, 29, 28, 28, 28, 29,
                                   29, 29, 28, 28, 29, 29, 29, 28, 28, 29, 29, 29, 29, 29, 28, 28};
constexpr int kEyebrowRotOffset[24] = {26, 26, 27, 25, 26, 25, 26, 25, 28, 25, 26, 24,
                                       27, 27, 26, 26, 25, 25, 26, 26, 27, 26, 25, 27};

// Face texture layout (RFLi_Config.h; RFLi_TEX_UNIT(x) is x / 64).
constexpr float kTexScaleX = 0.88961464f, kTexScaleY = 0.9276675f;
constexpr float kEyeBaseX = 0.0f / 64.0f, kEyeBaseY = 18.451525f, kEyeBaseW = 342 / 64.0f, kEyeBaseH = 288 / 64.0f;
constexpr float kEyebrowBaseX = 0.0f / 64.0f, kEyebrowBaseY = 16.549807f, kEyebrowBaseW = 324 / 64.0f,
                kEyebrowBaseH = 288 / 64.0f;
constexpr float kMouthBaseY = 29.25885f, kMouthBaseW = 396 / 64.0f, kMouthBaseH = 288 / 64.0f;
constexpr float kMustacheBaseY = 31.763554f, kMustacheBaseW = 288 / 64.0f, kMustacheBaseH = 576 / 64.0f;
constexpr float kMoleBaseX = 17.766165f, kMoleBaseY = 17.95986f, kMoleBaseW = 0 / 64.0f, kMoleBaseH = 0 / 64.0f;
// RFLi_TEX_SCALE2DIM and RFLi_TEX_ROTATE2ANG, with their C types: the first
// is a double expression.
double ScaleToDimension(int scale) { return 1.0 + 0.4f * scale; }
float RotateToAngle(int rotate) { return (360.0f / 32.0f) * (rotate % 32); }

// ---------------------------------------------------------------- textures

struct Rgba {
    int r = 0, g = 0, b = 0, a = 0;
};

enum Wrap { kClamp = 0, kRepeat = 1, kMirror = 2 };

struct Texture {
    int width = 0, height = 0;
    int wrap_s = kClamp, wrap_t = kClamp;
    bool linear = true;
    std::vector<Rgba> texels;
};

int Expand4(int v) { return (v << 4) | v; }
int Expand5(int v) { return (v << 3) | (v >> 2); }
int Expand3(int v) { return (v << 5) | (v << 2) | (v >> 1); }
int Expand6(int v) { return (v << 2) | (v >> 4); }

Rgba DecodeRgb5a3(std::uint16_t v)
{
    if (v & 0x8000) return {Expand5((v >> 10) & 31), Expand5((v >> 5) & 31), Expand5(v & 31), 255};
    return {Expand4((v >> 8) & 15), Expand4((v >> 4) & 15), Expand4(v & 15), Expand3((v >> 12) & 7)};
}

// The colour buffer's copy to RGB5A3: opaque 5-bit colour wherever the
// 3-bit alpha would be full (alpha 224 and up), else 4-bit colour.
std::uint16_t EncodeRgb5a3(const Rgba& c)
{
    if ((c.a >> 5) == 7) return std::uint16_t(0x8000 | ((c.r >> 3) << 10) | ((c.g >> 3) << 5) | (c.b >> 3));
    return std::uint16_t(((c.a >> 5) << 12) | ((c.r >> 4) << 8) | ((c.g >> 4) << 4) | (c.b >> 4));
}

// A texture file of the parts (RFLiTexture: a 32-byte big-endian header,
// the image at its offset), decoded to 8-bit RGBA as the GX reads it.
Texture DecodeTexture(std::span<const std::uint8_t> file)
{
    if (file.size() < 32) Damaged("A Mii part texture is cut short.");
    const std::uint8_t* h = file.data();
    Texture texture;
    const int format = h[0];
    texture.width = BE16(h + 2);
    texture.height = BE16(h + 4);
    texture.wrap_s = h[6];
    texture.wrap_t = h[7];
    // minFilt/magFilt: GX_NEAR (0) or GX_LINEAR (1); no part has mipmaps.
    texture.linear = h[0x14] != 0 || h[0x15] != 0;
    const std::size_t image = BE32(h + 0x1C);
    if (texture.width < 1 || texture.width > 1024 || texture.height < 1 || texture.height > 1024
        || texture.wrap_s > kMirror || texture.wrap_t > kMirror)
        Damaged("A Mii part texture has an impossible size or wrap mode.");

    int block_w = 4, block_h = 4, block_bytes = 32;
    switch (format) {
    case 0: block_w = 8, block_h = 8; break;          // I4
    case 1: case 2: block_w = 8, block_h = 4; break;  // I8, IA4
    case 3: case 4: case 5: break;                    // IA8, RGB565, RGB5A3
    case 6: block_bytes = 64; break;                  // RGBA8
    default: Damaged("A Mii part texture has a format the parts never use.");
    }
    const int blocks_x = (texture.width + block_w - 1) / block_w, blocks_y = (texture.height + block_h - 1) / block_h;
    const std::size_t bytes = std::size_t(blocks_x) * blocks_y * block_bytes;
    if (image > file.size() || bytes > file.size() - image) Damaged("A Mii part texture is cut short.");
    const std::uint8_t* data = file.data() + image;

    texture.texels.resize(std::size_t(texture.width) * texture.height);
    for (int by = 0; by < blocks_y; ++by)
        for (int bx = 0; bx < blocks_x; ++bx) {
            const std::uint8_t* block = data + (std::size_t(by) * blocks_x + bx) * block_bytes;
            for (int y = 0; y < block_h; ++y)
                for (int x = 0; x < block_w; ++x) {
                    const int px = bx * block_w + x, py = by * block_h + y, n = y * block_w + x;
                    Rgba c;
                    switch (format) {
                    case 0: {
                        const int v = Expand4(n & 1 ? block[n / 2] & 15 : block[n / 2] >> 4);
                        c = {v, v, v, v};
                        break;
                    }
                    case 1: c = {block[n], block[n], block[n], block[n]}; break;
                    case 2: {
                        const int i = Expand4(block[n] & 15);
                        c = {i, i, i, Expand4(block[n] >> 4)};
                        break;
                    }
                    case 3: c = {block[2 * n + 1], block[2 * n + 1], block[2 * n + 1], block[2 * n]}; break;
                    case 4: {
                        const int v = BE16(block + 2 * n);
                        c = {Expand5(v >> 11), (((v >> 5) & 63) << 2) | (((v >> 5) & 63) >> 4), Expand5(v & 31), 255};
                        break;
                    }
                    case 5: c = DecodeRgb5a3(BE16(block + 2 * n)); break;
                    default: c = {block[2 * n + 1], block[32 + 2 * n], block[32 + 2 * n + 1], block[2 * n]}; break;
                    }
                    if (px < texture.width && py < texture.height)
                        texture.texels[std::size_t(py) * texture.width + px] = c;
                }
        }
    return texture;
}

int WrapCoord(int i, int size, int mode)
{
    if (mode == kClamp) return i < 0 ? 0 : i >= size ? size - 1 : i;
    switch (mode) {
    case kRepeat: return ((i % size) + size) % size;
    case kMirror: {
        int m = ((i % (2 * size)) + 2 * size) % (2 * size);
        return m < size ? m : 2 * size - 1 - m;
    }
    default: return std::clamp(i, 0, size - 1);
    }
}

// A texel at texture coordinates (s, t), filtered as the GX filters: the
// coordinates in 1/128 texels (cut toward zero), for bilinear filtering
// texel centres at half steps and weights in 1/128 steps, the sum cut.
Rgba Sample(const Texture& texture, float s, float t)
{
    const int w = texture.width, h = texture.height;
    const auto at = [&](int x, int y) -> const Rgba& {
        return texture.texels[std::size_t(WrapCoord(y, h, texture.wrap_t)) * w + WrapCoord(x, w, texture.wrap_s)];
    };
    const float fs = s * float(w) * 128.0f, ft = t * float(h) * 128.0f;
    if (!(std::abs(fs) < 1e9f) || !(std::abs(ft) < 1e9f)) return at(0, 0);
    int is = int(fs), it = int(ft);
    if (!texture.linear) return at(is >> 7, it >> 7);
    is -= 64, it -= 64;
    const int x0 = is >> 7, y0 = it >> 7, wx = is & 127, wy = it & 127;
    const Rgba& c00 = at(x0, y0);
    const Rgba& c10 = at(x0 + 1, y0);
    const Rgba& c01 = at(x0, y0 + 1);
    const Rgba& c11 = at(x0 + 1, y0 + 1);
    const int w00 = (128 - wx) * (128 - wy), w10 = wx * (128 - wy), w01 = (128 - wx) * wy, w11 = wx * wy;
    const auto mix = [&](int Rgba::*channel) {
        return (c00.*channel * w00 + c10.*channel * w10 + c01.*channel * w01 + c11.*channel * w11) >> 14;
    };
    return {mix(&Rgba::r), mix(&Rgba::g), mix(&Rgba::b), mix(&Rgba::a)};
}

// ---------------------------------------------------------------- TEV

// One TEV colour or alpha operation as the GX computes it in 8 bits:
// d + lerp(a, b, c), c's 255 standing for 1, the sum rounded, then clamped.
int TevAdd(int a, int b, int c, int d)
{
    const int cc = c + (c >> 7);
    return std::clamp(d + ((a * (256 - cc) + b * cc + 128) >> 8), 0, 255);
}
// The same with GX_CS_DIVIDE_2 (no rounding before the halving).
int TevAddHalf(int a, int b, int c, int d)
{
    const int cc = c + (c >> 7);
    return std::clamp((d + ((a * (256 - cc) + b * cc) >> 8)) >> 1, 0, 255);
}
int TevMul(int b, int c) { return TevAdd(0, b, c, 0); }

// ---------------------------------------------------------------- render target

enum class Factor { Zero, One, SrcAlpha, InvSrcAlpha, DstAlpha, InvDstAlpha };
enum class Cull { None, Back, Front };

// One draw's pixel state: depth test (GX_LEQUAL) and update, blend factors,
// colour and alpha updates. The alpha test is GX_GREATER 0 in every draw.
struct Pass {
    bool z_test = false, z_update = false;
    Factor src = Factor::One, dst = Factor::Zero;
    bool color = true, alpha = true;
};

// The embedded frame buffer: GX_PF_RGBA6_Z24 keeps 6 bits per channel.
struct Target {
    int width = 0, height = 0;
    bool six_bits = true;
    std::vector<Rgba> color;
    std::vector<std::uint32_t> depth;

    Target(int w, int h, bool six) : width(w), height(h), six_bits(six), color(std::size_t(w) * h), depth(color.size())
    {
    }
    int Store(int v) const { return six_bits ? Expand6(v >> 2) : v; }
    // A clear as GXCopyTex's clear does it: only what the updates allow.
    void Clear(const Rgba& c, bool update_color, bool update_alpha)
    {
        for (auto& p : color) {
            if (update_color) p.r = Store(c.r), p.g = Store(c.g), p.b = Store(c.b);
            if (update_alpha) p.a = Store(c.a);
        }
        std::fill(depth.begin(), depth.end(), 0xFFFFFFu);
    }
};

int FactorValue(Factor f, const Rgba& src, const Rgba& dst)
{
    switch (f) {
    case Factor::Zero: return 0;
    case Factor::One: return 255;
    case Factor::SrcAlpha: return src.a;
    case Factor::InvSrcAlpha: return 255 - src.a;
    case Factor::DstAlpha: return dst.a;
    case Factor::InvDstAlpha: return 255 - dst.a;
    }
    return 0;
}

void WritePixel(Target& target, int x, int y, std::uint32_t z, const Rgba& src, const Pass& pass)
{
    if (src.a <= 0) return; // GXSetAlphaCompare(GX_GREATER, 0, ...)
    const std::size_t at = std::size_t(y) * target.width + x;
    if (pass.z_test && z > target.depth[at]) return;
    Rgba& dst = target.color[at];
    const int sf = FactorValue(pass.src, src, dst), df = FactorValue(pass.dst, src, dst);
    const int sw = sf + (sf >> 7), dw = df + (df >> 7);
    const auto blend = [&](int s, int d) { return std::min(255, (s * sw + d * dw) >> 8); };
    const Rgba out{blend(src.r, dst.r), blend(src.g, dst.g), blend(src.b, dst.b), blend(src.a, dst.a)};
    if (pass.color) dst.r = target.Store(out.r), dst.g = target.Store(out.g), dst.b = target.Store(out.b);
    if (pass.alpha) dst.a = target.Store(out.a);
    if (pass.z_update) target.depth[at] = z;
}

// ---------------------------------------------------------------- rasterizer

// A vertex in screen space: pixels from the top left, depth in 24-bit
// units, 1/w and the texture coordinates divided by w for perspective, and
// the lit colour (0..255).
struct Vertex {
    float x = 0, y = 0, depth = 0, inv_w = 1, s_w = 0, t_w = 0;
    float r = 0, g = 0, b = 0;
};

// What a fragment shader gets.
struct Fragment {
    float s, t;
    int r, g, b;
};

// Where a pixel is covered, in sixteenths of a pixel from its top left
// corner, and where its colours, depth and texture coordinates are taken:
// the GX's sample points as Dolphin's software renderer measured them.
constexpr std::int64_t kCoverageSample = 9;
constexpr double kAttributeSample = 0.495;

// A value across a triangle: its plane in screen space.
struct Plane {
    double at0 = 0, dx = 0, dy = 0, x0 = 0, y0 = 0;
    double operator()(double x, double y) const { return at0 + dx * (x - x0) + dy * (y - y0); }
};

// One triangle, with the GX's front faces clockwise on the screen and a
// top-left fill rule. Colours and depth are interpolated in screen space,
// texture coordinates with perspective.
template <class Shade>
void DrawTriangle(Target& target, const Vertex* v0, const Vertex* v1, const Vertex* v2, Cull cull, const Pass& pass,
                  const Shade& shade)
{
    const auto fixed = [](float v) { return std::int64_t(std::llround(double(v) * 16.0)); };
    for (const Vertex* v : {v0, v1, v2})
        if (!std::isfinite(v->x) || !std::isfinite(v->y) || std::abs(v->x) > 1e6f || std::abs(v->y) > 1e6f) return;
    std::int64_t x0 = fixed(v0->x), y0 = fixed(v0->y), x1 = fixed(v1->x), y1 = fixed(v1->y), x2 = fixed(v2->x),
                 y2 = fixed(v2->y);
    std::int64_t area = (x1 - x0) * (y2 - y0) - (x2 - x0) * (y1 - y0);
    if (area == 0) return;
    const bool front = area > 0;
    if ((cull == Cull::Back && !front) || (cull == Cull::Front && front)) return;
    if (area < 0) {
        std::swap(v1, v2);
        std::swap(x1, x2);
        std::swap(y1, y2);
        area = -area;
    }
    const auto top_left = [](std::int64_t dx, std::int64_t dy) { return dy < 0 || (dy == 0 && dx > 0); };
    const bool tl0 = top_left(x2 - x1, y2 - y1), tl1 = top_left(x0 - x2, y0 - y2), tl2 = top_left(x1 - x0, y1 - y0);

    const auto lo = [&](std::int64_t a, std::int64_t b, std::int64_t c, int limit) {
        const std::int64_t m = std::min({a, b, c}) - kCoverageSample;
        return int(std::clamp<std::int64_t>(m >= 0 ? m / 16 : -((-m + 15) / 16), 0, limit));
    };
    const auto hi = [&](std::int64_t a, std::int64_t b, std::int64_t c, int limit) {
        const std::int64_t m = std::max({a, b, c}) - kCoverageSample;
        return int(std::clamp<std::int64_t>(m >= 0 ? (m + 15) / 16 : -(-m / 16), -1, limit));
    };
    const int min_x = lo(x0, x1, x2, target.width), max_x = hi(x0, x1, x2, target.width - 1);
    const int min_y = lo(y0, y1, y2, target.height), max_y = hi(y0, y1, y2, target.height - 1);
    // Planes through the three vertices (their exact positions).
    const double dx10 = double(v1->x) - v0->x, dx20 = double(v2->x) - v0->x;
    const double dy10 = double(v1->y) - v0->y, dy20 = double(v2->y) - v0->y;
    const double det = dx10 * dy20 - dx20 * dy10;
    if (det == 0) return;
    const auto plane = [&](float Vertex::*value) {
        const double d10 = double(v1->*value) - v0->*value, d20 = double(v2->*value) - v0->*value;
        return Plane{v0->*value, (d10 * dy20 - d20 * dy10) / det, (dx10 * d20 - dx20 * d10) / det, v0->x, v0->y};
    };
    const Plane depth = plane(&Vertex::depth), inv_w = plane(&Vertex::inv_w), s_w = plane(&Vertex::s_w),
                t_w = plane(&Vertex::t_w), red = plane(&Vertex::r), green = plane(&Vertex::g),
                blue = plane(&Vertex::b);

    // The edge functions step by these from one pixel to the next.
    const std::int64_t step0 = -(y2 - y1) * 16, step1 = -(y0 - y2) * 16, step2 = -(y1 - y0) * 16;
    for (int py = min_y; py <= max_y; ++py) {
        const std::int64_t sy = std::int64_t(py) * 16 + kCoverageSample, sx = std::int64_t(min_x) * 16 + kCoverageSample;
        std::int64_t e0 = (x2 - x1) * (sy - y1) - (y2 - y1) * (sx - x1) - step0;
        std::int64_t e1 = (x0 - x2) * (sy - y2) - (y0 - y2) * (sx - x2) - step1;
        std::int64_t e2 = (x1 - x0) * (sy - y0) - (y1 - y0) * (sx - x0) - step2;
        for (int px = min_x; px <= max_x; ++px) {
            e0 += step0, e1 += step1, e2 += step2;
            if (e0 < 0 || e1 < 0 || e2 < 0 || (e0 == 0 && !tl0) || (e1 == 0 && !tl1) || (e2 == 0 && !tl2)) continue;
            const double ax = px + kAttributeSample, ay = py + kAttributeSample;
            const auto z = std::uint32_t(std::clamp(depth(ax, ay), 0.0, 16777215.0));
            // Behind what is drawn: whatever the texture gives, nothing changes.
            if (pass.z_test && z > target.depth[std::size_t(py) * target.width + px]) continue;
            const double w = inv_w(ax, ay);
            Fragment fragment{};
            if (w > 0) {
                fragment.s = float(s_w(ax, ay) / w);
                fragment.t = float(t_w(ax, ay) / w);
            }
            const auto colour = [&](const Plane& c) { return int(std::clamp(c(ax, ay), 0.0, 255.0)); };
            fragment.r = colour(red);
            fragment.g = colour(green);
            fragment.b = colour(blue);
            WritePixel(target, px, py, z, shade(fragment), pass);
        }
    }
}

// ---------------------------------------------------------------- the face texture

// RFL_ORIGIN.
enum class Origin { Center, Left, Right };

struct FacePart {
    float x, y, width, height, angle;
    Origin origin;
};

// RFLiMakeTexture's changes for an expression.
MiiFace ExpressionFace(MiiFace f, Expression expression)
{
    const auto clamp_add = [](int value, int change, int max) { return std::clamp(value + change, 0, max); };
    const auto new_eye = [&](int type) {
        const int change = kEyeRotOffset[f.eye_type] - kEyeRotOffset[type];
        f.eye_rotate = clamp_add(f.eye_rotate, change, 7);
        f.eye_type = type;
    };
    switch (expression) {
    case Expression::Normal: break;
    case Expression::Smile: new_eye(48); break;
    case Expression::Anger:
        f.eyebrow_rotate = clamp_add(f.eyebrow_rotate, 2, 11);
        f.eye_rotate = clamp_add(f.eye_rotate, 2, 7);
        f.mouth_type = 10;
        break;
    case Expression::Sorrow:
        f.eyebrow_rotate = clamp_add(f.eyebrow_rotate, -2, 11);
        f.eye_rotate = clamp_add(f.eye_rotate, -2, 7);
        f.mouth_type = 12;
        break;
    case Expression::Surprise:
        f.eyebrow_y -= 2; // an unsigned 5-bit field on the Wii
        f.eyebrow_y &= 31;
        new_eye(49);
        break;
    case Expression::Blink: new_eye(26); break;
    case Expression::OpenMouth: f.mouth_type = 24; break;
    }
    return f;
}

// RFLiDrawQuad: a part's quad, scaled, turned and placed.
void DrawFacePart(Target& target, const FacePart& part, const Pass& pass, const auto& shade)
{
    // PSMTXScaleApply, PSMTXRotRad (M_PI as the Wii's MSL defines it, a
    // float), PSMTXConcat, PSMTXScaleApply, PSMTXTransApply.
    const float angle = (3.1415927f / 180.0f) * part.angle;
    const float sn = std::sin(angle), cs = std::cos(angle);
    const float m00 = kTexScaleX * (cs * part.width), m01 = kTexScaleX * (-sn * part.height);
    const float m10 = kTexScaleY * (sn * part.width), m11 = kTexScaleY * (cs * part.height);
    float base = 0;
    float s0 = 1, s1 = 0;
    switch (part.origin) {
    case Origin::Center: base = -0.5f; break;
    case Origin::Right: base = -1.0f; break;
    case Origin::Left: base = 0.0f, s0 = 0, s1 = 1; break;
    }
    const float right = float(1.0 + base);
    const float corners[4][4] = {{right, -0.5f, s0, 0}, {right, 0.5f, s0, 1}, {base, 0.5f, s1, 1}, {base, -0.5f, s1, 0}};
    Vertex v[4];
    for (int n = 0; n < 4; ++n) {
        v[n].x = m00 * corners[n][0] + m01 * corners[n][1] + part.x;
        v[n].y = m10 * corners[n][0] + m11 * corners[n][1] + part.y;
        v[n].s_w = corners[n][2];
        v[n].t_w = corners[n][3];
    }
    DrawTriangle(target, &v[0], &v[1], &v[2], Cull::Back, pass, shade);
    DrawTriangle(target, &v[0], &v[2], &v[3], Cull::Back, pass, shade);
}

struct PartTextures {
    Texture eye, eyebrow, mouth, mustache, mole;
};

// RFLiMakeTexture and RFLiCapture for one resolution: the face texture of
// eyes, eyebrows, mouth, mustache and mole, as RGBA texels of its RGB5A3
// copy.
Texture MakeFaceTexture(const MiiParts& parts, const MiiFace& f, int resolution, bool wii_precision)
{
    const auto load = [&](Archive archive, int index) {
        const auto file = parts.File(archive, index);
        if (!file.size) Damaged("The Mii parts lack a face part texture.");
        return DecodeTexture({file.data, file.size});
    };
    PartTextures textures{load(kEye, f.eye_type), load(kEyebrow, f.eyebrow_type), load(kMouth, f.mouth_type),
                          load(kMustache, f.mustache_type), load(kMole, f.mole_type)};
    // GX_CLAMP for every part (RFLiInitRFLTexture).
    for (Texture* t : {&textures.eye, &textures.eyebrow, &textures.mouth, &textures.mustache, &textures.mole})
        t->wrap_s = t->wrap_t = kClamp;

    // RFLiSetFaceParts, with its C expression types.
    const float eyeX = kEyeBaseX + kTexScaleX * f.eye_x;
    const float eyeY = kEyeBaseY + 1.1600001f * kTexScaleY * f.eye_y;
    const float eyeW = float(kEyeBaseW * ScaleToDimension(f.eye_scale));
    const float eyeH = float(kEyeBaseH * ScaleToDimension(f.eye_scale));
    const float eyeA = RotateToAngle(f.eye_rotate + kEyeRotOffset[f.eye_type]);
    const float browX = kEyebrowBaseX + kTexScaleX * f.eyebrow_x;
    const float browY = kEyebrowBaseY + 1.1600001f * kTexScaleY * f.eyebrow_y;
    const float browW = float(kEyebrowBaseW * ScaleToDimension(f.eyebrow_scale));
    const float browH = float(kEyebrowBaseH * ScaleToDimension(f.eyebrow_scale));
    const float browA = RotateToAngle(f.eyebrow_rotate + kEyebrowRotOffset[f.eyebrow_type]);
    const float mouthY = kMouthBaseY + 1.1600001f * kTexScaleY * f.mouth_y;
    const float mouthW = float(kMouthBaseW * ScaleToDimension(f.mouth_scale));
    const float mouthH = float(kMouthBaseH * ScaleToDimension(f.mouth_scale));
    const float mustacheY = kMustacheBaseY + 1.1600001f * kTexScaleY * f.beard_y;
    const float mustacheW = float(kMustacheBaseW * ScaleToDimension(f.beard_scale));
    const float mustacheH = float(kMustacheBaseH * ScaleToDimension(f.beard_scale));
    const float moleX = kMoleBaseX + 2.0f * kTexScaleX * f.mole_x;
    const float moleY = kMoleBaseY + 1.1600001f * kTexScaleY * f.mole_y;
    const float moleW = float(kMoleBaseW + ScaleToDimension(f.mole_scale));
    const float moleH = float(kMoleBaseH + ScaleToDimension(f.mole_scale));
    const float unit = resolution / 64.0f;

    const FacePart eyeR{unit * (32.0f - eyeX), eyeY * unit, eyeW * unit, eyeH * unit, eyeA, Origin::Right};
    const FacePart eyeL{unit * (32.0f + eyeX), eyeY * unit, eyeW * unit, eyeH * unit, 360.0f - eyeA, Origin::Left};
    const FacePart browR{unit * (32.0f - browX), browY * unit, browW * unit, browH * unit, browA, Origin::Right};
    const FacePart browL{unit * (32.0f + browX), browY * unit, browW * unit, browH * unit, 360.0f - browA, Origin::Left};
    const FacePart mouth{32.0f * unit, mouthY * unit, mouthW * unit, mouthH * unit, 0.0f, Origin::Center};
    const FacePart mustacheR{32.0f * unit, mustacheY * unit, mustacheW * unit, mustacheH * unit, 0.0f, Origin::Right};
    const FacePart mustacheL{32.0f * unit, mustacheY * unit, mustacheW * unit, mustacheH * unit, 0.0f, Origin::Left};
    const FacePart mole{moleX * unit, moleY * unit, moleW * unit, moleH * unit, 0.0f, Origin::Center};

    // The TEV setups (RFLiSetTev4*). The texture swaps pick one channel of
    // the eye and mouth textures per stage.
    const auto constant = [](const Texture& texture, Rgb colour) {
        return [&texture, colour](const Fragment& in) {
            const Rgba t = Sample(texture, in.s, in.t);
            return Rgba{colour.r, colour.g, colour.b, t.a};
        };
    };
    const Rgb eye0 = f.eye_type == 9 ? Rgb{255, 130, 0} : f.eye_type == 20 ? Rgb{0, 255, 255} : Rgb{0, 0, 0};
    const Rgb eye1 = kEyeColor1[f.eye_color];
    const auto eye = [&](const Fragment& in) {
        const Rgba t = Sample(textures.eye, in.s, in.t);
        // Stage 0: C0 x red; stage 1: + C1 x blue; stage 2: + green.
        const auto channel = [&](int c0, int c1) { return TevAdd(0, 255, t.g, TevAdd(0, c1, t.b, TevMul(c0, t.r))); };
        return Rgba{channel(eye0.r, eye1.r), channel(eye0.g, eye1.g), channel(eye0.b, eye1.b), t.a};
    };
    const Rgb mouth0 = kMouthColor0[f.mouth_color], mouth1 = kMouthColor1[f.mouth_color];
    const auto mouth_shade = [&](const Fragment& in) {
        const Rgba t = Sample(textures.mouth, in.s, in.t);
        // Stage 0: C0 x red; stage 1: + C1 x green; stage 2: + blue.
        const auto channel = [&](int c0, int c1) { return TevAdd(0, 255, t.b, TevAdd(0, c1, t.g, TevMul(c0, t.r))); };
        return Rgba{channel(mouth0.r, mouth1.r), channel(mouth0.g, mouth1.g), channel(mouth0.b, mouth1.b), t.a};
    };
    const auto mustache_shade = constant(textures.mustache, kHairColor[f.beard_color]);
    const auto brow_shade = constant(textures.eyebrow, kHairColor[f.eyebrow_color]);
    const auto mole_shade = constant(textures.mole, kMoleColor);

    // RFLiSetupCopyTex clears to (0, 0, 0, 0). RFLiCapture draws every part
    // twice: first the colours, each part only where those before it left
    // room (GX_BL_INVDSTALPHA, GX_BL_DSTALPHA), then, after a copy that
    // clears the alpha (the colour update is off), the alpha alone
    // (GX_BL_INVDSTALPHA, GX_BL_ONE).
    Target target(resolution, resolution, wii_precision);
    target.Clear({0, 0, 0, 0}, true, true);
    const auto draw_all = [&](const Pass& pass) {
        DrawFacePart(target, mustacheR, pass, mustache_shade);
        DrawFacePart(target, mustacheL, pass, mustache_shade);
        DrawFacePart(target, mouth, pass, mouth_shade);
        DrawFacePart(target, browR, pass, brow_shade);
        DrawFacePart(target, browL, pass, brow_shade);
        DrawFacePart(target, eyeR, pass, eye);
        DrawFacePart(target, eyeL, pass, eye);
        DrawFacePart(target, mole, pass, mole_shade);
    };
    draw_all(Pass{false, false, Factor::InvDstAlpha, Factor::DstAlpha, true, true});
    target.Clear({0, 0, 0, 0}, false, true);
    draw_all(Pass{false, false, Factor::InvDstAlpha, Factor::One, false, true});

    Texture texture;
    texture.width = texture.height = resolution;
    texture.wrap_s = texture.wrap_t = kClamp;
    texture.linear = true;
    texture.texels = std::move(target.color);
    if (wii_precision)
        for (auto& t : texture.texels) t = DecodeRgb5a3(EncodeRgb5a3(t));
    return texture;
}

// ---------------------------------------------------------------- shapes

struct Shape {
    struct Index {
        int position, normal, texcoord;
    };
    struct Primitive {
        int type;
        std::vector<Index> vertices;
    };
    std::vector<std::array<int, 3>> positions, normals; // s16 values
    std::vector<std::array<int, 2>> texcoords;
    std::vector<Primitive> primitives;
    bool Empty() const { return primitives.empty(); }
};

struct ShapeTransform {
    bool transform = false, flip = false;
    float scale = 1;
    std::array<float, 3> translation{};
};

class Reader {
public:
    explicit Reader(std::span<const std::uint8_t> bytes) : bytes_(bytes) {}
    const std::uint8_t* Take(std::size_t n)
    {
        if (n > bytes_.size() - at_) Damaged("A Mii part shape is cut short.");
        const std::uint8_t* p = bytes_.data() + at_;
        at_ += n;
        return p;
    }
    int U8() { return *Take(1); }
    int U16() { return BE16(Take(2)); }
    int S16() { return std::int16_t(BE16(Take(2))); }
    float F32() { return std::bit_cast<float>(BE32(Take(4))); }

private:
    std::span<const std::uint8_t> bytes_;
    std::size_t at_ = 0;
};

// RFLiInitShapeRes: a shape file (a tag; for the faceline the nose, beard
// and hair placements; positions, normals, texture coordinates; the
// primitives of 8-bit indices), with its scale, placement and mirroring.
Shape ReadShape(std::span<const std::uint8_t> file, bool texcoords, const ShapeTransform& transform,
                std::array<std::array<float, 3>, 3>* faceline_placements = nullptr)
{
    Shape shape;
    if (file.empty()) return shape;
    Reader in(file);
    in.Take(4);
    if (faceline_placements)
        for (auto& placement : *faceline_placements)
            for (float& v : placement) {
                v = in.F32();
                if (!std::isfinite(v) || std::abs(v) > 1000.0f) Damaged("A Mii faceline has an impossible placement.");
            }
    const int position_count = in.U16();
    if (position_count == 0) return shape;

    const auto to_fixed = [](float v) { return std::int32_t(256.0f * v); };
    const std::int32_t s = to_fixed(transform.scale), tx = to_fixed(transform.translation[0]),
                       ty = to_fixed(transform.translation[1]), tz = to_fixed(transform.translation[2]);
    for (int n = 0; n < position_count; ++n) {
        const int x = in.S16(), y = in.S16(), z = in.S16();
        if (transform.transform)
            shape.positions.push_back({std::int16_t(tx + (((transform.flip ? -x : x) * s) >> 8)),
                                       std::int16_t(ty + ((y * s) >> 8)), std::int16_t(tz + ((z * s) >> 8))});
        else
            shape.positions.push_back({std::int16_t(transform.flip ? -x : x), y, z});
    }
    const int normal_count = in.U16();
    for (int n = 0; n < normal_count; ++n) {
        const int x = in.S16(), y = in.S16(), z = in.S16();
        shape.normals.push_back({std::int16_t(transform.flip ? -x : x), y, z});
    }
    if (texcoords) {
        const int count = in.U16();
        for (int n = 0; n < count; ++n) {
            const int s_ = in.S16(), t_ = in.S16();
            shape.texcoords.push_back({s_, t_});
        }
    }
    const int primitive_count = in.U8();
    for (int p = 0; p < primitive_count; ++p) {
        Shape::Primitive primitive;
        const int vertex_count = in.U8();
        primitive.type = in.U8() & 0xF8;
        for (int v = 0; v < vertex_count; ++v) {
            Shape::Index index{in.U8(), in.U8(), 0};
            if (texcoords) index.texcoord = in.U8();
            if (index.position >= int(shape.positions.size()) || index.normal >= int(shape.normals.size())
                || (texcoords && index.texcoord >= int(shape.texcoords.size())))
                Damaged("A Mii part shape has an index past its vertices.");
            primitive.vertices.push_back(index);
        }
        shape.primitives.push_back(std::move(primitive));
    }
    return shape;
}

// ---------------------------------------------------------------- the head

struct Camera {
    float view[3][4];
    float projection[4][4];
    float light[3];
    int width, height;
};

// C_MTXLookAt, C_MTXPerspective and the light of RFLiMakeIcon.
Camera IconCamera(int width, int height)
{
    Camera camera{};
    camera.width = width;
    camera.height = height;
    const float eye[3] = {0.0f, 34.5f, 600.0f}, up[3] = {0.0f, 1.0f, 0.0f}, target[3] = {0.0f, 34.5f, 0.0f};
    float look[3] = {eye[0] - target[0], eye[1] - target[1], eye[2] - target[2]};
    const auto normalize = [](float* v) {
        const float length = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
        for (int n = 0; n < 3; ++n) v[n] /= length;
    };
    const auto cross = [](const float* a, const float* b, float* out) {
        out[0] = a[1] * b[2] - a[2] * b[1];
        out[1] = a[2] * b[0] - a[0] * b[2];
        out[2] = a[0] * b[1] - a[1] * b[0];
    };
    normalize(look);
    float right[3], new_up[3];
    cross(up, look, right);
    normalize(right);
    cross(look, right, new_up);
    const float* rows[3] = {right, new_up, look};
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) camera.view[r][c] = rows[r][c];
        camera.view[r][3] = -(eye[0] * rows[r][0] + eye[1] * rows[r][1] + eye[2] * rows[r][2]);
    }

    const float aspect = float(width) / float(height);
    const float fovy = width < height ? 2 * (57.2957801818848f * std::atan2(43.2f / aspect, 500.0f))
                                      : 2 * (57.2957801818848f * std::atan2(43.2f, 500.0f));
    const float angle = (fovy * 0.5f) * 0.01745329252f;
    const float cot = 1.0f / std::tan(angle);
    const float near_z = 500.0f, far_z = 700.0f, tmp = 1.0f / (far_z - near_z);
    camera.projection[0][0] = cot / aspect;
    camera.projection[1][1] = cot;
    camera.projection[2][2] = -near_z * tmp;
    camera.projection[2][3] = -(far_z * near_z) * tmp;
    camera.projection[3][2] = -1.0f;

    const float light[3] = {1600.0f, 1500.0f, 6000.0f};
    for (int r = 0; r < 3; ++r)
        camera.light[r] = camera.view[r][0] * light[0] + camera.view[r][1] * light[1] + camera.view[r][2] * light[2]
                        + camera.view[r][3];
    return camera;
}

// The GX transform and lighting of one vertex: the view matrix (the model
// matrix of the icon), the perspective, the viewport, and the colour
// channel (one light, GX_DF_CLAMP, no attenuation, ambient 160, material
// white).
Vertex TransformVertex(const Camera& camera, const Shape& shape, const Shape::Index& index)
{
    const auto& p = shape.positions[std::size_t(index.position)];
    const auto& n = shape.normals[std::size_t(index.normal)];
    const float model[3] = {p[0] / 256.0f, p[1] / 256.0f, p[2] / 256.0f};
    float view[3], normal[3];
    for (int r = 0; r < 3; ++r) {
        view[r] = camera.view[r][0] * model[0] + camera.view[r][1] * model[1] + camera.view[r][2] * model[2]
                + camera.view[r][3];
        // The normal matrix of a rigid view matrix is its rotation.
        normal[r] = camera.view[r][0] * (n[0] / 16384.0f) + camera.view[r][1] * (n[1] / 16384.0f)
                  + camera.view[r][2] * (n[2] / 16384.0f);
    }
    float clip[4];
    for (int r = 0; r < 4; ++r)
        clip[r] = camera.projection[r][0] * view[0] + camera.projection[r][1] * view[1]
                + camera.projection[r][2] * view[2] + camera.projection[r][3];
    Vertex v;
    const float w = clip[3];
    v.inv_w = w > 1e-6f ? 1.0f / w : 0.0f;
    v.x = camera.width * 0.5f + clip[0] * v.inv_w * (camera.width * 0.5f);
    v.y = camera.height * 0.5f - clip[1] * v.inv_w * (camera.height * 0.5f);
    v.depth = 16777215.0f * (1.0f + clip[2] * v.inv_w);
    if (index.texcoord < int(shape.texcoords.size())) {
        const auto& t = shape.texcoords[std::size_t(index.texcoord)];
        v.s_w = t[0] / 8192.0f * v.inv_w;
        v.t_w = t[1] / 8192.0f * v.inv_w;
    }

    float to_light[3] = {camera.light[0] - view[0], camera.light[1] - view[1], camera.light[2] - view[2]};
    const float light_length = std::sqrt(to_light[0] * to_light[0] + to_light[1] * to_light[1] + to_light[2] * to_light[2]);
    const float normal_length = std::sqrt(normal[0] * normal[0] + normal[1] * normal[1] + normal[2] * normal[2]);
    float diffuse = 0.0f;
    if (light_length > 0 && normal_length > 0)
        diffuse = std::max(0.0f, (to_light[0] * normal[0] + to_light[1] * normal[1] + to_light[2] * normal[2])
                                     / (light_length * normal_length));
    // Ambient 160 plus the white light, cut to 8 bits, times the white
    // material (GX colour arithmetic: 255 counts as 256).
    const int light = std::clamp(int(160.0f + 255.0f * diffuse), 0, 255);
    v.r = v.g = v.b = float((255 * (light + (light >> 7))) >> 8);
    return v;
}

template <class Shade>
void DrawShape(Target& target, const Camera& camera, const Shape& shape, Cull cull, const Pass& pass, const Shade& shade)
{
    std::vector<Vertex> vertices;
    for (const auto& primitive : shape.primitives) {
        vertices.clear();
        for (const auto& index : primitive.vertices) vertices.push_back(TransformVertex(camera, shape, index));
        const int count = int(vertices.size());
        const auto tri = [&](int a, int b, int c) {
            if (vertices[a].inv_w > 0 && vertices[b].inv_w > 0 && vertices[c].inv_w > 0)
                DrawTriangle(target, &vertices[a], &vertices[b], &vertices[c], cull, pass, shade);
        };
        switch (primitive.type) {
        case 0x80: // GX_QUADS
            for (int n = 0; n + 3 < count; n += 4) tri(n, n + 1, n + 2), tri(n, n + 2, n + 3);
            break;
        case 0x90: // GX_TRIANGLES
            for (int n = 0; n + 2 < count; n += 3) tri(n, n + 1, n + 2);
            break;
        case 0x98: // GX_TRIANGLESTRIP
            for (int n = 0; n + 2 < count; ++n) n % 2 ? tri(n + 1, n, n + 2) : tri(n, n + 1, n + 2);
            break;
        case 0xA0: // GX_TRIANGLEFAN
            for (int n = 1; n + 1 < count; ++n) tri(0, n, n + 1);
            break;
        default: break; // lines and points: no part has them
        }
    }
}

const Texture* NonEmpty(const std::optional<Texture>& texture) { return texture ? &*texture : nullptr; }

int MaskResolution(int width, int height)
{
    // RFLiMakeIcon, with the large textures (RFLiGetUseDeluxTex) the game asks for.
    if (width > 128 || height > 128) return 256;
    if (width > 64 || height > 64) return 128;
    return 64;
}
} // namespace

// ---------------------------------------------------------------- public

const std::array<MiiFaceField, 39> kMiiFaceFields = {{
    {"favorite_color", &MiiFace::favorite_color, 0, 11},
    {"face_type", &MiiFace::face_type, 0, 7},
    {"face_color", &MiiFace::face_color, 0, 5},
    {"face_texture", &MiiFace::face_texture, 0, 11},
    {"hair_type", &MiiFace::hair_type, 0, 71},
    {"hair_color", &MiiFace::hair_color, 0, 7},
    {"hair_flip", &MiiFace::hair_flip, 0, 1},
    {"eyebrow_type", &MiiFace::eyebrow_type, 0, 23},
    {"eyebrow_color", &MiiFace::eyebrow_color, 0, 7},
    {"eyebrow_scale", &MiiFace::eyebrow_scale, 0, 8},
    {"eyebrow_rotate", &MiiFace::eyebrow_rotate, 0, 11},
    {"eyebrow_x", &MiiFace::eyebrow_x, 0, 12},
    {"eyebrow_y", &MiiFace::eyebrow_y, 3, 18},
    {"eye_type", &MiiFace::eye_type, 0, 47},
    {"eye_color", &MiiFace::eye_color, 0, 5},
    {"eye_scale", &MiiFace::eye_scale, 0, 7},
    {"eye_rotate", &MiiFace::eye_rotate, 0, 7},
    {"eye_x", &MiiFace::eye_x, 0, 12},
    {"eye_y", &MiiFace::eye_y, 0, 18},
    {"nose_type", &MiiFace::nose_type, 0, 11},
    {"nose_scale", &MiiFace::nose_scale, 0, 8},
    {"nose_y", &MiiFace::nose_y, 0, 18},
    {"mouth_type", &MiiFace::mouth_type, 0, 23},
    {"mouth_color", &MiiFace::mouth_color, 0, 2},
    {"mouth_scale", &MiiFace::mouth_scale, 0, 8},
    {"mouth_y", &MiiFace::mouth_y, 0, 18},
    {"glasses_type", &MiiFace::glasses_type, 0, 8},
    {"glasses_color", &MiiFace::glasses_color, 0, 5},
    {"glasses_scale", &MiiFace::glasses_scale, 0, 7},
    {"glasses_y", &MiiFace::glasses_y, 0, 20},
    {"mustache_type", &MiiFace::mustache_type, 0, 3},
    {"beard_type", &MiiFace::beard_type, 0, 3},
    {"beard_color", &MiiFace::beard_color, 0, 7},
    {"beard_scale", &MiiFace::beard_scale, 0, 8},
    {"beard_y", &MiiFace::beard_y, 0, 16},
    {"mole_type", &MiiFace::mole_type, 0, 1},
    {"mole_scale", &MiiFace::mole_scale, 0, 8},
    {"mole_x", &MiiFace::mole_x, 0, 16},
    {"mole_y", &MiiFace::mole_y, 0, 30},
}};

MiiFace ReadMiiFace(std::string_view record)
{
    if (record.size() < kMiiRecordBytes) throw std::invalid_argument("A Mii record has 74 bytes.");
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(record.data());
    MiiFace face;
    for (std::size_t n = 0; n < kMiiFaceFields.size(); ++n) {
        const FieldBits& bits = kFieldBits[n];
        face.*kMiiFaceFields[n].value = (BE16(bytes + bits.offset) >> bits.shift) & ((1 << bits.width) - 1);
    }
    return face;
}

void WriteMiiFace(const MiiFace& face, std::string& record)
{
    if (record.size() < kMiiRecordBytes) throw std::invalid_argument("A Mii record has 74 bytes.");
    for (std::size_t n = 0; n < kMiiFaceFields.size(); ++n) {
        const FieldBits& bits = kFieldBits[n];
        const unsigned mask = ((1u << bits.width) - 1) << bits.shift;
        auto* p = reinterpret_cast<std::uint8_t*>(record.data()) + bits.offset;
        const unsigned word = (BE16(p) & ~mask) | ((unsigned(face.*kMiiFaceFields[n].value) << bits.shift) & mask);
        p[0] = std::uint8_t(word >> 8);
        p[1] = std::uint8_t(word);
    }
}

bool ValidMiiFace(const MiiFace& face)
{
    for (const auto& field : kMiiFaceFields)
        if (face.*field.value < field.min || face.*field.value > field.max) return false;
    return true;
}

std::array<std::uint8_t, 3> FavoriteColor(int index)
{
    const Rgb& c = kFavoriteColor[((index % 12) + 12) % 12];
    return {std::uint8_t(c.r), std::uint8_t(c.g), std::uint8_t(c.b)};
}

MiiParts::MiiParts(std::vector<std::uint8_t> resource) : bytes_(std::move(resource))
{
    const std::size_t size = bytes_.size();
    const std::uint8_t* data = bytes_.data();
    if (size < 4 + 4 * kArchiveCount || BE16(data) != kArchiveCount)
        Damaged("This is not the Mii parts file (RFL_Res.dat).");
    for (int a = 0; a < kArchiveCount; ++a) {
        const std::size_t section = BE32(data + 4 + 4 * a);
        if (section > size || size - section < 4) Damaged("The Mii parts file is damaged.");
        Archive archive;
        archive.files = BE16(data + section);
        archive.table = section + 4;
        if ((size - archive.table) / 4 < std::size_t(archive.files) + 1) Damaged("The Mii parts file is damaged.");
        archive.data = archive.table + 4 * (std::size_t(archive.files) + 1);
        std::uint32_t previous = 0;
        for (int f = 0; f <= archive.files; ++f) {
            const std::uint32_t offset = BE32(data + archive.table + 4 * f);
            if (offset < previous || offset > size - archive.data) Damaged("The Mii parts file is damaged.");
            previous = offset;
        }
        archives_[std::size_t(a)] = archive;
    }
}

int MiiParts::FileCount(int archive) const
{
    return archive >= 0 && archive < kArchiveCount ? archives_[std::size_t(archive)].files : 0;
}

MiiParts::Bytes MiiParts::File(int archive, int index) const
{
    if (archive < 0 || archive >= kArchiveCount) return {};
    const Archive& a = archives_[std::size_t(archive)];
    if (index < 0 || index >= a.files) return {};
    const std::uint32_t begin = BE32(bytes_.data() + a.table + 4 * std::size_t(index));
    const std::uint32_t end = BE32(bytes_.data() + a.table + 4 * (std::size_t(index) + 1));
    return {bytes_.data() + a.data + begin, std::size_t(end - begin)};
}

MiiParts MiiParts::FromArchive(const std::vector<std::uint8_t>& archive)
{
    auto resource = ReadU8File(archive, "RFL_Res.dat");
    if (!resource) Damaged("The Mii resource archive has no RFL_Res.dat.");
    return MiiParts(std::move(*resource));
}

std::optional<std::vector<std::uint8_t>> ReadU8File(const std::vector<std::uint8_t>& archive, std::string_view path)
{
    // A U8 archive: magic, the offset of its nodes, their size with the
    // names, the data offset; nodes of 12 bytes (type, name offset, data
    // offset or parent, size or end of the directory), the root first.
    const std::size_t size = archive.size();
    const std::uint8_t* data = archive.data();
    if (size < 0x20 || BE32(data) != 0x55AA382Du) Damaged("This is not a U8 archive.");
    const std::size_t nodes = BE32(data + 4), nodes_size = BE32(data + 8);
    if (nodes > size || nodes_size > size - nodes || nodes_size < 12) Damaged("The U8 archive is damaged.");
    const std::uint8_t* root = data + nodes;
    if (root[0] != 1) Damaged("The U8 archive is damaged.");
    const std::size_t count = BE32(root + 8);
    if (count == 0 || count > nodes_size / 12) Damaged("The U8 archive is damaged.");
    const std::size_t names = nodes + count * 12, names_size = nodes_size - count * 12;
    const auto name_of = [&](std::size_t node) {
        const std::size_t offset = BE32(data + nodes + node * 12) & 0xFFFFFF;
        if (offset >= names_size) Damaged("The U8 archive is damaged.");
        const char* first = reinterpret_cast<const char*>(data + names + offset);
        const auto* end = static_cast<const char*>(std::memchr(first, 0, names_size - offset));
        if (!end) Damaged("The U8 archive is damaged.");
        return std::string_view(first, std::size_t(end - first));
    };

    while (!path.empty() && path.front() == '/') path.remove_prefix(1);
    // Directories open on the walk: their names and where they end.
    std::vector<std::pair<std::string, std::size_t>> open;
    for (std::size_t node = 1; node < count; ++node) {
        while (!open.empty() && node >= open.back().second) open.pop_back();
        const std::uint8_t* entry = data + nodes + node * 12;
        std::string full;
        for (const auto& dir : open) full += dir.first + "/";
        full += name_of(node);
        if (entry[0] == 1) {
            const std::size_t end = BE32(entry + 8);
            if (end <= node || end > count) Damaged("The U8 archive is damaged.");
            open.emplace_back(std::string(name_of(node)), end);
            continue;
        }
        if (entry[0] != 0) Damaged("The U8 archive is damaged.");
        if (full != path) continue;
        const std::size_t offset = BE32(entry + 4), length = BE32(entry + 8);
        if (offset > size || length > size - offset) Damaged("The U8 archive is damaged.");
        return std::vector<std::uint8_t>(data + offset, data + offset + length);
    }
    return std::nullopt;
}

RgbaImage RenderMiiHead(const MiiParts& parts, std::string_view record, const MiiHeadOptions& options)
{
    return RenderMiiHead(parts, ReadMiiFace(record), options);
}

RgbaImage RenderMiiHead(const MiiParts& parts, const MiiFace& original, const MiiHeadOptions& options)
{
    // The Mii library never checks the favourite colour (a 4-bit field of
    // which 12 values are colours): draw others as the first, red.
    MiiFace f = original;
    if (f.favorite_color < 0 || f.favorite_color > 11) f.favorite_color = 0;
    if (!ValidMiiFace(f)) throw std::invalid_argument("The Mii's face has a value out of range.");
    if (options.width < 1 || options.width > 1024 || options.height < 1 || options.height > 1024)
        throw std::invalid_argument("A Mii head is 1 to 1024 pixels wide and high.");
    const int width = options.width, height = options.height;
    const bool wii = options.wii_precision;

    // RFLiInitCharModelRes: the shapes and their textures.
    const auto file = [&](Archive archive, int index) {
        if (index >= parts.FileCount(archive)) Damaged("The Mii parts lack a file the face uses.");
        const auto bytes = parts.File(archive, index);
        return std::span<const std::uint8_t>(bytes.data, bytes.size);
    };
    std::array<std::array<float, 3>, 3> placements{}; // nose, beard, hair
    const Shape face_shape = ReadShape(file(kFaceline, f.face_type), true, {}, &placements);
    const auto& [nose_place, beard_place, hair_place] = placements;
    const ShapeTransform hair{true, f.hair_flip != 0, 1.0f, hair_place};
    const Shape cap = ReadShape(file(kCap, f.hair_type), true, hair);
    const Shape hair_shape = ReadShape(file(kHair, f.hair_type), false, hair);
    const Shape forehead = ReadShape(file(kForehead, f.hair_type), false, hair);
    const Shape beard = ReadShape(file(kBeard, f.beard_type), false, {true, false, 1.0f, beard_place});
    const float nose_scale = 0.4f + 0.175f * f.nose_scale;
    const ShapeTransform nose_transform{
        true, false, nose_scale, {nose_place[0], +nose_place[1] + -1.5f * (f.nose_y - 8), nose_place[2]}};
    const Shape nose = ReadShape(file(kNose, f.nose_type), false, nose_transform);
    const Shape noseline = ReadShape(file(kNoseline, f.nose_type), true, nose_transform);
    const Shape mask = ReadShape(file(kMask, f.face_type), true, {});
    const ShapeTransform glasses_transform{true, false, 0.15f * f.glasses_scale + 0.4f,
                                           {nose_place[0], 5.0f + nose_place[1] + -1.5f * (f.glasses_y - 11),
                                            2.0f + nose_place[2]}};
    const Shape glasses = ReadShape(file(kGlass, 0), true, glasses_transform);

    const Texture face_texture = DecodeTexture(file(kFaceTex, f.face_texture));
    std::optional<Texture> cap_texture, noseline_texture;
    if (!cap.Empty()) cap_texture = DecodeTexture(file(kCapTex, f.hair_type));
    if (!noseline.Empty()) noseline_texture = DecodeTexture(file(kNoselineTex, f.nose_type));
    const Texture glasses_texture = DecodeTexture(file(kGlassTex, f.glasses_type));
    const Texture mask_texture =
        MakeFaceTexture(parts, ExpressionFace(f, options.expression), MaskResolution(width, height), wii);

    // RFLiMakeIcon.
    const Camera camera = IconCamera(width, height);
    Target target(width, height, wii);
    target.Clear({0, 0, 0, 0}, true, true);

    const Rgb skin = kFacelineColor[f.face_color];
    const auto lit = [](int c, int light) { return TevMul(c, light); };
    // RFLDrawOpaCore: stage 0 makes the colour, stage 1 multiplies it by the
    // lit colour; the alpha is the konstant's 8/8.
    const auto solid = [&](Rgb colour) {
        return [colour, &lit](const Fragment& in) {
            return Rgba{lit(colour.r, in.r), lit(colour.g, in.g), lit(colour.b, in.b), 255};
        };
    };
    const Pass opaque{true, true, Factor::SrcAlpha, Factor::InvSrcAlpha, true, true};
    const Cull hair_cull = f.hair_flip ? Cull::Front : Cull::Back;

    if (!beard.Empty()) DrawShape(target, camera, beard, Cull::Back, opaque, solid(kHairColor[f.beard_color]));
    if (!nose.Empty()) DrawShape(target, camera, nose, Cull::Back, opaque, solid(skin));
    if (!forehead.Empty()) DrawShape(target, camera, forehead, hair_cull, opaque, solid(skin));
    if (!hair_shape.Empty()) DrawShape(target, camera, hair_shape, hair_cull, opaque, solid(kHairColor[f.hair_color]));
    if (const Texture* t = NonEmpty(cap_texture)) {
        // (konst + konst x texture) / 2: the favourite colour, lighter where
        // the cap's texture is light.
        const Rgb hat = kFavoriteColor[f.favorite_color];
        DrawShape(target, camera, cap, hair_cull, opaque, [&, t](const Fragment& in) {
            const Rgba c = Sample(*t, in.s, in.t);
            return Rgba{lit(TevAddHalf(0, hat.r, c.r, hat.r), in.r), lit(TevAddHalf(0, hat.g, c.g, hat.g), in.g),
                        lit(TevAddHalf(0, hat.b, c.b, hat.b), in.b), 255};
        });
    }
    // The face: skin, with the face texture over it by the texture's green
    // (the swap table puts green in the alpha and alpha in the green).
    DrawShape(target, camera, face_shape, Cull::Back, opaque, [&](const Fragment& in) {
        const Rgba c = Sample(face_texture, in.s, in.t);
        return Rgba{lit(TevAdd(skin.r, c.r, c.g, 0), in.r), lit(TevAdd(skin.g, c.a, c.g, 0), in.g),
                    lit(TevAdd(skin.b, c.b, c.g, 0), in.b), 255};
    });

    // RFLDrawXluCore, twice: the colours over what is there, then the
    // alpha added up, both behind what is nearer.
    const auto mask_shade = [&](const Fragment& in) {
        const Rgba c = Sample(mask_texture, in.s, in.t);
        return Rgba{lit(c.r, in.r), lit(c.g, in.g), lit(c.b, in.b), c.a};
    };
    const auto noseline_shade = [&](const Fragment& in) {
        return Rgba{0, 0, 0, Sample(*noseline_texture, in.s, in.t).a};
    };
    const Rgb lens = kGlassColor[f.glasses_color];
    const auto glasses_shade = [&](const Fragment& in) {
        const Rgba c = Sample(glasses_texture, in.s, in.t);
        return Rgba{lit(TevMul(lens.r, c.r), in.r), lit(TevMul(lens.g, c.g), in.g), lit(TevMul(lens.b, c.b), in.b), c.a};
    };
    for (const Pass& pass : {Pass{true, false, Factor::SrcAlpha, Factor::InvSrcAlpha, true, false},
                             Pass{true, false, Factor::One, Factor::InvSrcAlpha, false, true}}) {
        DrawShape(target, camera, mask, Cull::Back, pass, mask_shade);
        if (!noseline.Empty()) DrawShape(target, camera, noseline, Cull::Back, pass, noseline_shade);
        DrawShape(target, camera, glasses, Cull::None, pass, glasses_shade);
    }

    // GXCopyTex to RGB5A3, then the background.
    RgbaImage image;
    image.width = width;
    image.height = height;
    image.pixels.resize(std::size_t(width) * height * 4);
    std::array<std::uint8_t, 3> background = options.background_color;
    if (options.background == MiiHeadOptions::Background::Favorite) background = FavoriteColor(f.favorite_color);
    for (std::size_t n = 0; n < target.color.size(); ++n) {
        Rgba c = target.color[n];
        if (wii) c = DecodeRgb5a3(EncodeRgb5a3(c));
        std::uint8_t* out = image.pixels.data() + n * 4;
        const int rgb[3] = {c.r, c.g, c.b};
        if (options.background != MiiHeadOptions::Background::Transparent) {
            // The colours are already blended over black where the head is
            // not opaque: add the background behind them.
            for (int k = 0; k < 3; ++k)
                out[k] = std::uint8_t(std::min(255, rgb[k] + (background[std::size_t(k)] * (255 - c.a) + 127) / 255));
            out[3] = 255;
        } else if (c.a == 0) {
            out[0] = out[1] = out[2] = out[3] = 0;
        } else {
            // Straight alpha for hosts: undo the blend over black.
            for (int k = 0; k < 3; ++k) out[k] = std::uint8_t(std::min(255, (rgb[k] * 255 + c.a / 2) / c.a));
            out[3] = std::uint8_t(c.a);
        }
    }
    return image;
}
} // namespace mscharged::platform::mii
