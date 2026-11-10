#include "ui/rui_kit.h"

#include "imgui_internal.h"
#include "overlay/flags.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <iterator>
#include <string>
#include <utility>

namespace mscharged::ui
{
namespace
{
// The game's own "LINE": a dark line sweeping down the screen every 4.8 s.
constexpr float kSweepPeriod = 4.8f, kSweepDelay = 0.8f;
// The screen's rounded corners; the frame's follow them, so its edge is as
// thin there as along the sides.
constexpr float kScreenRound = 14.0f, kFrameRound = kScreenRound + kBezel;
float Noise(std::uint32_t n)
{
    n = (n ^ 61u) ^ (n >> 16);
    n *= 9u;
    n ^= n >> 4;
    n *= 0x27d4eb2du;
    n ^= n >> 15;
    return float(n & 0xFFFFu) / 65536.0f;
}

// The game's TITLE shake (pausemenu_v3's 12 keys over 0.574 s, looping), in
// front end units, eased between its keys.
ImVec2 TitleShake(float seconds)
{
    struct Key
    {
        float t, x, y;
    };
    constexpr Key kKeys[] = {{0.0f, 0, 0},   {0.045f, 0, 1},  {0.082f, -1, 0}, {0.135f, -1, 0},
                             {0.205f, 0, 0}, {0.251f, -1, 0}, {0.33f, 0, 1},   {0.371f, 1, 1},
                             {0.429f, 0, 0}, {0.476f, 0, 1},  {0.539f, 1, 1},  {0.574f, 1, 1}};
    const float t = std::fmod(std::max(0.0f, seconds), 0.574f);
    for (std::size_t n = 1; n < std::size(kKeys); ++n)
        if (t <= kKeys[n].t)
        {
            const Key& a = kKeys[n - 1];
            const Key& b = kKeys[n];
            const float f = (t - a.t) / std::max(0.001f, b.t - a.t);
            return {a.x + (b.x - a.x) * f, a.y + (b.y - a.y) * f};
        }
    return {kKeys[std::size(kKeys) - 1].x, kKeys[std::size(kKeys) - 1].y};
}
} // namespace

std::string Upper(const char* text)
{
    std::string upper = text ? text : "";
    for (char& c : upper)
        if (c >= 'a' && c <= 'z') c = char(c - 'a' + 'A');
    return upper;
}

void Ease(float& value, float target)
{
    value += (target - value) * std::min(1.0f, std::clamp(ImGui::GetIO().DeltaTime, 0.0f, 0.1f) * 14.0f);
    if (std::abs(target - value) < 0.5f) value = target;
}

ImVec2 Painter::At(float x, float y) const { return {std::round(c.x + x * s), std::round(c.y + y * s)}; }

ImU32 Painter::Col(int r, int g, int b, int a) const
{
    return IM_COL32(r, g, b, int(std::lround(float(a) * std::clamp(fade, 0.0f, 1.0f))));
}

void Painter::Rect(float x0, float y0, float x1, float y1, ImU32 col, float round) const
{
    draw->AddRectFilled(At(x0, y0), At(x1, y1), col, round * s);
}

void Painter::Frame(float x0, float y0, float x1, float y1, ImU32 col, float round, float thickness) const
{
    draw->AddRect(At(x0, y0), At(x1, y1), col, round * s, 0, std::max(1.0f, thickness * s));
}

void Painter::Gradient(float x0, float y0, float x1, float y1, ImU32 top, ImU32 bottom) const
{
    draw->AddRectFilledMultiColor(At(x0, y0), At(x1, y1), top, top, bottom, bottom);
}

void Painter::Text(ImFont* font, float size, float x, float y, const char* text, ImU32 fill, ImU32 edge, float outline,
                   float align) const
{
    if (!font || !text || !*text) return;
    const float pixels = size * s;
    const ImVec2 extent = font->CalcTextSizeA(pixels, FLT_MAX, 0.0f, text);
    const ImVec2 at{std::round(c.x + x * s - extent.x * align), std::round(c.y + y * s - extent.y * 0.5f)};
    if (outline > 0.0f && (edge >> IM_COL32_A_SHIFT) != 0)
    {
        const float o = std::max(1.0f, outline * s);
        // The outline: the text all around.
        const int directions = o > 2.5f ? 16 : 8;
        for (int n = 0; n < directions; ++n)
        {
            const float angle = float(n) * 6.2831853f / float(directions);
            draw->AddText(font, pixels, {at.x + std::cos(angle) * o, at.y + std::sin(angle) * o}, edge, text);
        }
    }
    draw->AddText(font, pixels, at, fill, text);
}

void Painter::RoundGradient(float x0, float y0, float x1, float y1, ImU32 top, ImU32 bottom, float round) const
{
    // The rounded shape in the top colour (its alpha kept), then its corners
    // shaded from the top colour to the bottom one.
    const int first = draw->VtxBuffer.Size;
    const ImVec2 a = At(x0, y0), b = At(x1, y1);
    draw->AddRectFilled(a, b, top, round * s);
    ImGui::ShadeVertsLinearColorGradientKeepAlpha(draw, first, draw->VtxBuffer.Size, a, {a.x, b.y}, top, bottom);
}

float Painter::TextWidth(ImFont* font, float size, const char* text) const
{
    if (!font || !text) return 0.0f;
    return font->CalcTextSizeA(size * s, FLT_MAX, 0.0f, text).x / s;
}

float Painter::WrappedHeight(ImFont* font, float size, float width, const char* text, float none) const
{
    if (!font) return none;
    return font->CalcTextSizeA(size * s, FLT_MAX, width * s, text).y / s;
}

float Painter::Wrapped(ImFont* font, float size, float x, float y, float width, const char* text, ImU32 fill) const
{
    if (!font || !text || !*text) return 0.0f;
    const float pixels = size * s;
    const ImVec2 at = At(x, y);
    draw->AddText(font, pixels, {at.x + 1.0f, at.y + 1.0f}, IM_COL32(0, 0, 0, (fill >> IM_COL32_A_SHIFT) * 2 / 3), text,
                  nullptr, width * s);
    draw->AddText(font, pixels, at, fill, text, nullptr, width * s);
    return WrappedHeight(font, size, width, text);
}

namespace
{
// A well-mixed hash of a texel (x, y) and a seed: no rows or columns in it.
std::uint32_t Hash2(std::uint32_t x, std::uint32_t y, std::uint32_t seed)
{
    std::uint32_t h = x * 0x8DA6B343u ^ y * 0xD8163841u ^ seed * 0xCB1AB31Fu;
    h ^= h >> 16;
    h *= 0x7FEB352Du;
    h ^= h >> 15;
    h *= 0x846CA68Bu;
    h ^= h >> 16;
    return h;
}
} // namespace

std::vector<unsigned char> NoiseTexels()
{
    constexpr int size = kNoiseSize;
    // Fine grain, a few percent either way: dark and light texels at random.
    std::vector<unsigned char> rgba(std::size_t(size) * std::size_t(size) * 4);
    for (int y = 0; y < size; ++y)
        for (int x = 0; x < size; ++x)
        {
            const std::uint32_t h = Hash2(std::uint32_t(x), std::uint32_t(y), 0x9E37u);
            const bool light = (h & 1u) != 0;
            const float amount = float((h >> 8) & 0xFFu) / 255.0f;
            const std::size_t n = (std::size_t(y) * std::size_t(size) + std::size_t(x)) * 4;
            rgba[n + 0] = rgba[n + 1] = rgba[n + 2] = light ? 255 : 0;
            rgba[n + 3] = static_cast<unsigned char>(3.0f + amount * (light ? 11.0f : 15.0f));
        }
    return rgba;
}

void DrawTv(const Painter& p, const Painter& c, const TvLook& look)
{
    const float fw = look.half_width, iw = fw - kBezel;
    const float screenBottom = look.screen_bottom, frameBottom = look.frame_bottom;
    // The frame: dark metal with rounded, bevelled edges and a soft shadow.
    for (int n = 4; n >= 1; --n)
    {
        const float spread = float(n) * 6.0f;
        p.Rect(-fw - spread, kFrameTop - spread + 12.0f, fw + spread, frameBottom + spread + 12.0f,
               p.Col(0, 0, 0, 34), kFrameRound + spread);
    }
    p.Rect(-fw - 2, kFrameTop - 2, fw + 2, frameBottom + 2, p.Col(12, 12, 14), kFrameRound + 2);
    p.Rect(-fw, kFrameTop, fw, frameBottom, p.Col(56, 56, 59), kFrameRound);
    p.Frame(-fw + 1, kFrameTop + 1, fw - 1, frameBottom - 1, p.Col(112, 112, 116, 150), kFrameRound, 2.0f);
    const std::uint32_t speckles = std::uint32_t(160.0f * fw / kStandardHalfWidth);
    for (std::uint32_t n = 0; n < speckles; ++n)
    { // worn metal
        const float x = -fw + 8 + Noise(n * 2 + 1) * (2 * fw - 16);
        const float y = kFrameTop + 8 + Noise(n * 2 + 2) * (frameBottom - kFrameTop - 16);
        const int shade = Noise(n + 977) < 0.5f ? 0 : 255;
        p.Rect(x, y, x + 1.0f + Noise(n + 41) * 2.5f, y + 1.0f, p.Col(shade, shade, shade, 22));
    }
    // The bezel's dark recess around the screen.
    p.Rect(-iw - 4, kScreenTop - 4, iw + 4, screenBottom + 4, p.Col(14, 16, 18), kScreenRound + 4);

    // The screen, an old TV: deep teal to light cyan, darker towards its
    // corners, a fine flicker of noise, scanlines and the game's dark
    // interference bar sweeping down it.
    p.Gradient(-iw, kScreenTop, iw, screenBottom, p.Col(26, 50, 58), p.Col(108, 168, 184));
    p.Gradient(-iw, screenBottom - 80, iw, screenBottom, p.Col(140, 210, 228, 0), p.Col(150, 222, 238, 70));
    if (look.noise)
    {
        // A new grain about 15 times a second: the picture in tiles of about
        // a texel per 1.4 screen pixels across the screen, each frame from
        // another place in it and mirrored or turned another way (never
        // stretched past its edges).
        const int step = int(look.elapsed * 15.0f);
        const std::uint32_t roll = Hash2(std::uint32_t(step), 7u, 3u);
        const int variant = int(roll & 7u);
        const float tile = float(kNoiseSize) * 1.4f / p.s; // design pixels per tile
        const float shiftX = float((roll >> 8) & 0xFFu) / 255.0f * tile, shiftY = float((roll >> 16) & 0xFFu) / 255.0f * tile;
        const ImVec2 clipMin = p.At(-iw, kScreenTop), clipMax = p.At(iw, screenBottom);
        p.draw->PushClipRect(clipMin, clipMax, true);
        for (float y = kScreenTop - shiftY; y < screenBottom; y += tile)
            for (float x = -iw - shiftX; x < iw; x += tile)
            {
                ImVec2 uv[4] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
                if (variant & 1)
                    for (ImVec2& v : uv) v.x = 1.0f - v.x;
                if (variant & 2)
                    for (ImVec2& v : uv) v.y = 1.0f - v.y;
                if (variant & 4)
                    for (ImVec2& v : uv) std::swap(v.x, v.y);
                p.draw->AddImageQuad(look.noise, p.At(x, y), p.At(x + tile, y), p.At(x + tile, y + tile),
                                     p.At(x, y + tile), uv[0], uv[1], uv[2], uv[3], p.Col(255, 255, 255, 255));
            }
        p.draw->PopClipRect();
    }
    // The tube's darker corners: shade creeping in from each edge.
    const float edge = 110.0f;
    const ImU32 shade = p.Col(0, 0, 0, 70), clear = p.Col(0, 0, 0, 0);
    p.draw->AddRectFilledMultiColor(p.At(-iw, kScreenTop), p.At(-iw + edge, screenBottom), shade, clear, clear, shade);
    p.draw->AddRectFilledMultiColor(p.At(iw - edge, kScreenTop), p.At(iw, screenBottom), clear, shade, shade, clear);
    p.Gradient(-iw, kScreenTop, iw, kScreenTop + edge, shade, clear);
    p.Gradient(-iw, screenBottom - edge * 0.6f, iw, screenBottom, clear, p.Col(0, 0, 0, 40));
    for (float y = kScreenTop + 6; y < screenBottom; y += 18.0f) c.Rect(-iw, y, iw, y + 2, c.Col(255, 255, 255, 14));
    const float sweep = std::fmod(look.elapsed, kSweepPeriod) - kSweepDelay;
    if (sweep >= 0.0f && c.fade > 0.0f)
    {
        const float y = kScreenTop + (screenBottom - kScreenTop) * sweep / (kSweepPeriod - kSweepDelay);
        const float glowTop = std::max(kScreenTop, y - 34.0f), barBottom = std::min(screenBottom, y + 6.0f);
        if (glowTop < y) c.Gradient(-iw, glowTop, iw, y, c.Col(255, 255, 255, 0), c.Col(220, 250, 255, 22));
        if (y < barBottom) c.Rect(-iw, y, iw, barBottom, c.Col(0, 0, 0, 56));
    }
}

void DrawScreenMask(const Painter& p, float half_width, float screenBottom)
{
    // Over what the screen shows, so its square corners don't: at each
    // corner the frame's grey out to its dark rim, then the rim itself (as
    // thin as DrawTv's, inside the frame) all the way round.
    const float iw = half_width - kBezel, r = kScreenRound;
    struct Corner
    {
        float x, y, sx, sy; // the square corner, and which way is out
        float start;        // the arc's first angle (screen y points down)
    };
    const float pi = 3.14159265f;
    const Corner corners[4] = {{-iw, kScreenTop, -1, -1, pi},
                               {iw, kScreenTop, 1, -1, 1.5f * pi},
                               {iw, screenBottom, 1, 1, 0.0f},
                               {-iw, screenBottom, -1, 1, 0.5f * pi}};
    constexpr int kSteps = 12;
    const float rim = r + 3.5f; // just inside the rim's outer edge
    for (const Corner& c : corners)
    {
        ImVec2 points[kSteps + 2];
        points[0] = p.At(c.x, c.y);
        const float cx = c.x - c.sx * r, cy = c.y - c.sy * r;
        for (int n = 0; n <= kSteps; ++n)
        {
            const float a = c.start + 0.5f * pi * float(n) / float(kSteps);
            points[n + 1] = p.At(cx + std::cos(a) * rim, cy + std::sin(a) * rim);
        }
        // A fan from the square corner (ImGui fills a convex polygon so).
        p.draw->AddConvexPolyFilled(points, kSteps + 2, p.Col(56, 56, 59));
    }
    p.draw->AddRect(p.At(-iw - 2.0f, kScreenTop - 2.0f), p.At(iw + 2.0f, screenBottom + 2.0f), p.Col(14, 16, 18),
                    (r + 2.0f) * p.s, 0, 4.0f * p.s);
}

void DrawHeader(const Painter& c, float half_width, ImTextureID header, const ImVec2* corners)
{
    const float iw = half_width - kBezel;
    if (header)
    {
        // A wider screen: the picture's edges drawn on to its sides.
        const ImVec2 left = c.At(-iw, 0.0f), right = c.At(iw, 0.0f);
        const ImU32 white = c.Col(255, 255, 255);
        if (left.x < corners[0].x)
            c.draw->AddImageQuad(header, {left.x, corners[0].y}, corners[0], corners[3], {left.x, corners[3].y},
                                 {0.0f, 0.0f}, {0.004f, 0.0f}, {0.004f, 1.0f}, {0.0f, 1.0f}, white);
        if (right.x > corners[1].x)
            c.draw->AddImageQuad(header, corners[1], {right.x, corners[1].y}, {right.x, corners[2].y}, corners[2],
                                 {0.996f, 0.0f}, {1.0f, 0.0f}, {1.0f, 1.0f}, {0.996f, 1.0f}, white);
        c.draw->AddImageQuad(header, corners[0], corners[1], corners[2], corners[3], {0, 0}, {1, 0}, {1, 1}, {0, 1},
                             white);
        return;
    }
    // Drawn alike: an orange bar, dark red at its top, yellow at its foot.
    const float mid = (kScreenTop + kHeaderBottom) * 0.5f;
    c.Gradient(-iw, kScreenTop, iw, mid, c.Col(98, 36, 30), c.Col(212, 132, 74));
    c.Gradient(-iw, mid, iw, kHeaderBottom, c.Col(212, 132, 74), c.Col(226, 200, 92));
    c.Rect(-iw, kHeaderBottom, iw, kHeaderBottom + 5, c.Col(16, 18, 18));
}

void DrawTitle(const Painter& c, ImFont* font, const char* title, float elapsed)
{
    if (!title || !*title) return;
    struct Ghost
    {
        float scale, phase;
        int alpha;
    };
    constexpr Ghost kGhosts[] = {{1.09f, 0.19f, 51}, {0.91f, 0.38f, 38}};
    for (const Ghost& ghost : kGhosts)
    {
        const ImVec2 shake = TitleShake(elapsed + ghost.phase);
        c.Text(font, kTitleSize * ghost.scale, shake.x * kDesignPerUnit, kTitleY - shake.y * kDesignPerUnit, title,
               c.Col(255, 255, 255, ghost.alpha), c.Col(0, 0, 0, ghost.alpha / 3), 2.0f);
    }
    const ImVec2 shake = TitleShake(elapsed);
    const float tx = shake.x * kDesignPerUnit, ty = kTitleY - shake.y * kDesignPerUnit;
    c.Text(font, kTitleSize, tx, ty + 3, title, c.Col(0, 0, 0, 150), c.Col(0, 0, 0, 150), 5.0f);
    c.Text(font, kTitleSize, tx, ty, title, c.Col(250, 248, 244), c.Col(8, 8, 8), 4.0f);
}



namespace
{
// The game's pages' colours: their values' orange, their bars' green
// (OptionsAudioMenuV2::UpdateVolumeBars), their arrows' gold.
constexpr int kOrangeR = 255, kOrangeG = 186, kOrangeB = 24;
constexpr int kBarR = 0xA9, kBarG = 0xD0, kBarB = 0x46;
// The lists' items: a plate's and a row's height, a caption's, the gap; an
// arrow button's and a choice's size.
constexpr float kPlateHeight = 90.0f, kRowHeight = 78.0f, kCaptionHeight = 48.0f, kGap = 8.0f;
constexpr float kArrowSize = 54.0f, kPillHeight = 50.0f;
// A list's plates a little smaller than the pause menu's (which keep the game's).
constexpr float kListPlate = 0.94f;
// The help line's box at the bottom of the rows (without a help area).
constexpr float kHelpHeight = 96.0f;
// The scroll rail's width (its arrows are squares of it).
constexpr float kRailWidth = 46.0f;
// A plate's top and bottom around its text's middle (the game's 480x70 plate).
constexpr float kPlateTop = -39.0f, kPlateBottom = 45.0f;

// The game's orange (its values, captions, the lit category).
ImU32 Orange(const Painter& p, int alpha = 255) { return p.Col(kOrangeR, kOrangeG, kOrangeB, alpha); }

// The hover sound when the hand comes over another of a strip's items.
void HoverSound(int& last, int hover, const std::function<void(Sound)>& sound)
{
    if (hover != last && hover >= 0 && sound) sound(Sound::Hover);
    last = hover;
}

bool Inside(ImVec2 at, float x0, float y0, float x1, float y1)
{
    return at.x >= x0 && at.x <= x1 && at.y >= y0 && at.y <= y1;
}

// A square arrow button as the game's (gold, its black chevron pointing
// `dir`: 0 left, 1 right, 2 up, 3 down; silver and blank when it does nothing).
void ArrowSquare(const Painter& p, float x0, float y0, float size, int dir, bool enabled, bool over)
{
    const float x1 = x0 + size, y1 = y0 + size;
    p.Rect(x0 - 3, y0 - 3, x1 + 3, y1 + 3, p.Col(16, 18, 20, 230), 9.0f);
    if (enabled)
        p.RoundGradient(x0, y0, x1, y1, over ? p.Col(255, 236, 140) : p.Col(255, 216, 86),
                        over ? p.Col(232, 172, 40) : p.Col(204, 140, 16), 7.0f);
    else p.RoundGradient(x0, y0, x1, y1, p.Col(236, 238, 242), p.Col(150, 156, 164), 7.0f);
    p.Rect(x0 + 6, y0 + 3, x1 - 6, y0 + 6, p.Col(255, 255, 255, 90), 2.0f);
    if (!enabled) return;
    const float cx = (x0 + x1) * 0.5f, cy = (y0 + y1) * 0.5f, s = size * 0.2f;
    const ImU32 ink = p.Col(12, 10, 6);
    switch (dir)
    {
    case 0: p.draw->AddTriangleFilled(p.At(cx + s * 0.6f, cy - s), p.At(cx + s * 0.6f, cy + s), p.At(cx - s * 0.9f, cy), ink); break;
    case 1: p.draw->AddTriangleFilled(p.At(cx - s * 0.6f, cy - s), p.At(cx - s * 0.6f, cy + s), p.At(cx + s * 0.9f, cy), ink); break;
    case 2: p.draw->AddTriangleFilled(p.At(cx - s, cy + s * 0.6f), p.At(cx + s, cy + s * 0.6f), p.At(cx, cy - s * 0.9f), ink); break;
    default: p.draw->AddTriangleFilled(p.At(cx - s, cy - s * 0.6f), p.At(cx + s, cy - s * 0.6f), p.At(cx, cy + s * 0.9f), ink); break;
    }
}

// A bar as the rows' (the game's pages' dark title bar), lighter when
// chosen, its edge lit.
void Bar(const Painter& p, float x0, float y0, float x1, float y1, bool chosen, bool lit)
{
    p.Gradient(x0, y0, x1, y1, chosen ? p.Col(42, 56, 62, 240) : p.Col(18, 24, 28, 215),
               chosen ? p.Col(22, 32, 38, 240) : p.Col(6, 9, 11, 215));
    p.Rect(x0, y0, x1, y0 + 2, p.Col(255, 255, 255, chosen ? 40 : 18));
    if (lit) p.Frame(x0 - 3, y0 - 3, x1 + 3, y1 + 3, p.Col(130, 226, 255, 210), 6.0f, 2.5f);
}

// A plate centred on (cx, mid), `half` to each side.
void PlateAt(const Painter& c, ImFont* font, float cx, float half, float mid, const char* label, bool lit,
             bool chosen = false, float textSize = kPlateSize)
{
    // Lit, the plate grows and turns solid as in the game ("over").
    const float grow = lit ? 1.042f : 1.0f, tall = lit ? 1.10f : 1.0f;
    const float x0 = cx - half * grow, x1 = cx + half * grow, centreY = mid + (kPlateTop + kPlateBottom) * 0.5f;
    const float top = centreY + (kPlateTop - kPlateBottom) * 0.5f * tall;
    const float bottom = centreY + (kPlateBottom - kPlateTop) * 0.5f * tall;
    const int alpha = lit || chosen ? 255 : 196;
    c.Rect(x0 - 2, top - 2, x1 + 2, bottom + 2, c.Col(8, 12, 14, alpha), 7.0f);
    c.Gradient(x0, top, x1, top + (bottom - top) * 0.32f, c.Col(22, 30, 34, alpha), c.Col(66, 78, 84, alpha));
    c.Gradient(x0, top + (bottom - top) * 0.32f, x1, bottom, c.Col(84, 96, 102, alpha), c.Col(96, 108, 114, alpha));
    c.Rect(x0 + 6, top + 2, x1 - 6, top + 4, c.Col(255, 255, 255, 26));
    if (chosen)
    {
        // The category shown: a gold edge at its left and a gold rim.
        c.Rect(x0, top, x0 + 9, bottom, c.Col(255, 200, 60), 3.0f);
        c.Frame(x0 - 2, top - 2, x1 + 2, bottom + 2, c.Col(255, 196, 50, 200), 7.0f, 2.0f);
    }
    if (lit) c.Frame(x0 - 3, top - 3, x1 + 3, bottom + 3, c.Col(130, 226, 255, 200), 8.0f, 2.5f);
    const float size = textSize * (lit ? kLitPlateScale : 1.0f);
    c.Text(font, size, cx, mid, label, chosen ? c.Col(255, 230, 150) : c.Col(247, 247, 247), c.Col(6, 6, 6), 3.0f);
}

} // namespace

void DrawPlate(const Painter& c, ImFont* font, float half, float mid, const char* label, bool lit)
{
    PlateAt(c, font, 0.0f, half, mid, label, lit);
}

int DrawFooter(const Painter& p, const Fonts& fonts, float half_width, const FooterButton* buttons, int count,
               ImVec2 pointer, bool pointer_shown)
{
    const ImVec2 at = p.ToDesign(pointer);
    int under = -1;
    for (int n = 0; n < count; ++n)
    {
        // The game's BACK: a white arrow and the word, big; the way on alike
        // at the right, its arrow after the word.
        const FooterButton& button = buttons[n];
        const float size = kPlateSize * 0.86f;
        const float text = p.TextWidth(fonts.plate, size, button.label);
        const float width = 38.0f + text;
        const float x0 = button.right ? half_width - 40.0f - width : -half_width + 40.0f;
        const bool over = pointer_shown && Inside(at, x0 - 16, kFooterY - 44, x0 + width + 16, kFooterY + 44);
        if (over) under = n;
        const ImU32 fill = over ? p.Col(255, 214, 80) : p.Col(250, 250, 250);
        const float ax = button.right ? x0 + width : x0;              // the arrow's point
        const float dir = button.right ? 1.0f : -1.0f;                // where it points
        const float base = ax - dir * 22.0f;
        p.draw->AddTriangleFilled(p.At(base, kFooterY - 15), p.At(base, kFooterY + 15), p.At(ax, kFooterY),
                                  p.Col(0, 0, 0, 160));
        p.draw->AddTriangleFilled(p.At(base + dir, kFooterY - 14), p.At(base + dir, kFooterY + 14),
                                  p.At(ax - dir, kFooterY), fill);
        p.Text(fonts.plate, size, button.right ? x0 : x0 + 38.0f, kFooterY, button.label, fill, p.Col(6, 6, 6), 3.0f,
               0.0f);
    }
    return under;
}

int DrawCategories(const Painter& c, const Fonts& fonts, const Input& in, Area area,
                   const std::vector<std::string>& names, int selected, const std::function<void(Sound)>& sound,
                   float* height)
{
    // As the rows' bars: the one shown lighter, with a gold edge, its name in
    // orange and an arrow towards its rows.
    static int lastHover = -1;
    const ImVec2 at = c.ToDesign(in.pointer);
    float y = area.top;
    int clicked = -1, hover = -1;
    for (std::size_t n = 0; n < names.size(); ++n)
    {
        const float top = y, bottom = y + kRowHeight, mid = (top + bottom) * 0.5f;
        const bool chosen = int(n) == selected;
        const bool over = in.pointer_shown && Inside(at, area.x0, top, area.x1, bottom);
        if (over) hover = int(n);
        Bar(c, area.x0, top, area.x1, bottom, chosen, over);
        if (chosen)
        {
            c.Rect(area.x0, top, area.x0 + 7, bottom, c.Col(255, 200, 60), 2.0f);
            const float ax = area.x1 - 26.0f;
            c.draw->AddTriangleFilled(c.At(ax - 8, mid - 11), c.At(ax - 8, mid + 11), c.At(ax + 6, mid), Orange(c));
        }
        c.Text(fonts.row, kRowSize * 0.92f, area.x0 + 28.0f, mid, names[n].c_str(),
               chosen ? Orange(c) : over ? c.Col(255, 255, 255) : c.Col(228, 232, 234),
               c.Col(6, 6, 6), 2.5f, 0.0f);
        if (over && in.click && !chosen) clicked = int(n);
        y = bottom + kGap;
    }
    if (height) *height = y - area.top;
    HoverSound(lastHover, hover, sound);
    if (clicked >= 0 && sound) sound(Sound::Switch);
    return clicked;
}

int DrawCrumbs(const Painter& c, const Fonts& fonts, const Input& in, float half_width,
               const std::vector<std::string>& crumbs, const std::function<void(Sound)>& sound)
{
    static int lastHover = -1;
    const float iw = half_width - kBezel;
    // A strip across the screen as the rows' bars, an orange line at its foot.
    c.Gradient(-iw, kStripTop, iw, kStripBottom, c.Col(14, 20, 24, 210), c.Col(4, 7, 9, 210));
    c.Rect(-iw, kStripBottom - 2, iw, kStripBottom, Orange(c, 110));
    const float mid = (kStripTop + kStripBottom) * 0.5f, size = kSmallSize * 0.95f;
    const ImVec2 at = c.ToDesign(in.pointer);
    float x = -iw + 26.0f;
    int clicked = -1, hover = -1;
    for (std::size_t n = 0; n < crumbs.size(); ++n)
    {
        const bool last = n + 1 == crumbs.size();
        const float w = c.TextWidth(fonts.small, size, crumbs[n].c_str());
        const bool over = !last && in.pointer_shown && Inside(at, x - 12, kStripTop, x + w + 12, kStripBottom);
        if (over) hover = int(n);
        const ImU32 colour = last ? Orange(c) : over ? c.Col(255, 255, 255) : c.Col(190, 204, 210);
        c.Text(fonts.small, size, x, mid, crumbs[n].c_str(), colour, c.Col(4, 12, 16, 220), 1.5f, 0.0f);
        if (over)
        {
            c.Rect(x, mid + 13, x + w, mid + 15, c.Col(255, 255, 255, 190));
            if (in.click) clicked = int(n);
        }
        x += w;
        if (!last)
        {
            // Between the steps: a small arrow.
            const float ax = x + 20.0f;
            c.draw->AddTriangleFilled(c.At(ax - 5, mid - 8), c.At(ax - 5, mid + 8), c.At(ax + 5, mid), Orange(c, 190));
            x = ax + 22.0f;
        }
    }
    HoverSound(lastHover, hover, sound);
    if (clicked >= 0 && sound) sound(Sound::Back);
    return clicked;
}

List::List(const Painter& content, const Fonts& fonts, const Input& input, ListState& state,
           std::function<void(Sound)> sound, Area rows, std::optional<Area> help)
    : p_(content), fonts_(fonts), in_(input), state_(state), sound_(std::move(sound)), rows_(rows), help_area_(help),
      listBottom_(!help && state.help ? rows.bottom - kHelpHeight : rows.bottom)
{
    // The D-pad moves the lit item, over last frame's items.
    if (state_.count > 0)
    {
        const int before = state_.lit;
        if (in_.down) state_.lit = (state_.lit + 1) % state_.count;
        if (in_.up) state_.lit = (state_.lit + state_.count - 1) % state_.count;
        if (state_.lit != before)
        {
            Play(Sound::Hover);
            state_.reveal = true;
        }
    }
    const float view = listBottom_ - rows_.top;
    // More than the screen shows: the rail at the right takes its place.
    if (state_.height > view + 1.0f)
    {
        rail_ = true;
        railX0_ = rows_.x1 - kRailWidth;
        rows_.x1 = railX0_ - 16.0f;
    }
    if (in_.wheel != 0.0f) state_.target -= in_.wheel * 90.0f;
    state_.target = std::clamp(state_.target, 0.0f, std::max(0.0f, state_.height - view));
    Ease(state_.scroll, state_.target);
    // The items stay in their part of the screen (their lit edge included).
    p_.draw->PushClipRect(p_.At(rows_.x0 - 8.0f, rows_.top - 4.0f), p_.At(rows_.x1 + 8.0f, listBottom_), true);
    clipped_ = true;
}

List::~List()
{
    if (clipped_) p_.draw->PopClipRect();
    state_.count = index_;
    if (index_ > 0) state_.lit = std::clamp(state_.lit, 0, index_ - 1);
    state_.height = y_;
    const float view = listBottom_ - rows_.top;
    // The lit item into view, once.
    if (state_.reveal && state_.lit_bottom > state_.lit_top)
    {
        if (state_.lit_top - 12.0f < state_.target) state_.target = std::max(0.0f, state_.lit_top - 12.0f);
        if (state_.lit_bottom + 12.0f > state_.target + view) state_.target = state_.lit_bottom + 12.0f - view;
    }
    state_.reveal = false;
    state_.target = std::clamp(state_.target, 0.0f, std::max(0.0f, y_ - view));
    if (rail_) Rail(view);
    // The lit item's name and help, in a box as the rows' bars: in the help
    // area, else at the bottom of the rows.
    state_.help = anyHelp_;
    std::optional<Area> box = help_area_;
    if (!box && anyHelp_) box = Area{rows_.x0, rail_ ? railX0_ + kRailWidth : rows_.x1, rows_.bottom - kHelpHeight + 10.0f,
                                     rows_.bottom - 4.0f};
    if (box)
    {
        const Area& a = *box;
        p_.Gradient(a.x0, a.top, a.x1, a.bottom, p_.Col(18, 24, 28, 200), p_.Col(6, 9, 11, 200));
        p_.Rect(a.x0, a.top, a.x1, a.top + 2, p_.Col(255, 255, 255, 18));
        p_.Rect(a.x0, a.top, a.x0 + 6, a.bottom, Orange(p_, 220), 2.0f);
        if (!help_.empty())
        {
            const bool titled = help_area_ && !helpTitle_.empty();
            float y = a.top + 14.0f;
            if (titled)
            {
                p_.Text(fonts_.small, kSmallSize, a.x0 + 22.0f, y + 12.0f, helpTitle_.c_str(), Orange(p_),
                        p_.Col(4, 12, 16, 230), 2.0f, 0.0f);
                y += 38.0f;
            }
            p_.Wrapped(fonts_.small, kSmallSize * 0.92f, a.x0 + 22.0f, y, a.x1 - a.x0 - 40.0f, help_.c_str(),
                       p_.Col(232, 244, 248, 240));
        }
    }
    state_.entered = true;
}

List::Slot List::Place(float height, bool litable)
{
    Slot slot{};
    slot.top = rows_.top + y_ - state_.scroll;
    slot.bottom = slot.top + height;
    slot.index = -1;
    if (litable)
    {
        slot.index = index_++;
        slot.lit = slot.index == state_.lit;
        const ImVec2 at = p_.ToDesign(in_.pointer);
        slot.hovered = in_.pointer_shown && at.x >= rows_.x0 - 8 && at.x <= rows_.x1 + 8 && at.y >= slot.top
            && at.y < slot.bottom + kGap && at.y >= rows_.top && at.y < listBottom_;
        if (slot.lit && state_.reveal)
        {
            // Its place, for scrolling to it.
            state_.lit_top = y_;
            state_.lit_bottom = y_ + height;
        }
    }
    y_ += height + kGap;
    return slot;
}

void List::LightUnderPointer(Slot& slot)
{
    if (!slot.hovered) return;
    pointerOver_ = true;
    if ((in_.pointer_moved || in_.click) && state_.lit != slot.index)
    {
        state_.lit = slot.index;
        if (state_.entered) Play(Sound::Hover);
    }
    slot.lit = state_.lit == slot.index;
}

List::Slot List::ValueRow(const char* label, const char* about)
{
    Slot slot = Place(kRowHeight, true);
    LightUnderPointer(slot);
    RowBar(slot, label);
    Help(label, about, slot.lit);
    return slot;
}

bool List::Pressed(const Slot& slot, bool right)
{
    const bool pressed = (slot.lit && (in_.accept || (right && in_.right))) || (slot.hovered && in_.click);
    if (pressed) Play(Sound::Select);
    return pressed;
}

void List::Chevron(float mid, bool lit)
{
    p_.Text(fonts_.row, kRowSize, rows_.x1 - 24.0f, mid - 2.0f, "\xE2\x80\xBA",
            lit ? p_.Col(255, 255, 255) : p_.Col(190, 200, 206), p_.Col(6, 6, 6), 2.0f, 1.0f);
}

void List::Help(const char* label, const char* about, bool lit)
{
    if (!about) return;
    anyHelp_ = true;
    if (!lit) return;
    help_ = about;
    helpTitle_ = Upper(label);
}

void List::Caption(const char* text)
{
    // The game's orange, in capitals.
    const std::string upper = Upper(text);
    const Slot slot = Place(kCaptionHeight, false);
    const float y = slot.bottom - 16.0f;
    p_.Text(fonts_.small, kSmallSize, rows_.x0 + 4.0f, y, upper.c_str(), Orange(p_), p_.Col(4, 12, 16, 230), 2.0f, 0.0f);
    const float after = rows_.x0 + 4.0f + p_.TextWidth(fonts_.small, kSmallSize, upper.c_str()) + 16.0f;
    if (after < rows_.x1) p_.Rect(after, y - 1, rows_.x1, y + 1, Orange(p_, 80));
}

void List::Note(const char* text)
{
    if (!text || !*text) return;
    const float size = kSmallSize * 0.92f, width = rows_.x1 - rows_.x0 - 8.0f;
    const Slot slot = Place(p_.WrappedHeight(fonts_.small, size, width, text) + 6.0f, false);
    p_.Wrapped(fonts_.small, size, rows_.x0 + 4.0f, slot.top + 3.0f, width, text, p_.Col(226, 240, 246, 225));
}

void List::RowBar(const Slot& slot, const char* label)
{
    // The game's pages' dark title bar, the label in white capitals.
    const bool lit = slot.lit;
    const float x0 = rows_.x0, top = slot.top, bottom = slot.bottom;
    Bar(p_, x0, top, rows_.x1, bottom, lit, lit);
    const std::string upper = Upper(label);
    p_.Text(fonts_.row, kRowSize, x0 + 24.0f, (top + bottom) * 0.5f, upper.c_str(),
            lit ? p_.Col(255, 255, 255) : p_.Col(236, 238, 240), p_.Col(6, 6, 6), 2.5f, 0.0f);
}

bool List::Pill(const Slot& slot, float x0, float x1, const char* text, bool chosen)
{
    const float mid = (slot.top + slot.bottom) * 0.5f, y0 = mid - kPillHeight * 0.5f, y1 = mid + kPillHeight * 0.5f;
    const ImVec2 at = p_.ToDesign(in_.pointer);
    const bool over = slot.hovered && Inside(at, x0, y0, x1, y1);
    if (chosen)
    {
        // Chosen: the game's gold, as its arrows.
        p_.RoundGradient(x0, y0, x1, y1, p_.Col(255, 216, 86), p_.Col(214, 150, 22), 8.0f);
        p_.Frame(x0, y0, x1, y1, p_.Col(90, 56, 0, 220), 8.0f, 2.0f);
        p_.Text(fonts_.small, kSmallSize, (x0 + x1) * 0.5f, mid, text, p_.Col(40, 24, 0), 0, 0.0f);
    }
    else
    {
        // As its outlined ACCEPT.
        p_.Rect(x0, y0, x1, y1, over ? p_.Col(255, 255, 255, 40) : p_.Col(0, 0, 0, 130), 8.0f);
        p_.Frame(x0, y0, x1, y1, over ? p_.Col(255, 214, 80) : p_.Col(240, 240, 240, 150), 8.0f, 2.0f);
        p_.Text(fonts_.small, kSmallSize, (x0 + x1) * 0.5f, mid, text, p_.Col(236, 238, 240), p_.Col(0, 0, 0, 160),
                1.5f);
    }
    return over && in_.click;
}

bool List::Arrow(const Slot& slot, float x, bool right, bool enabled)
{
    const float mid = (slot.top + slot.bottom) * 0.5f, y0 = mid - kArrowSize * 0.5f;
    const ImVec2 at = p_.ToDesign(in_.pointer);
    const bool over = enabled && slot.hovered && Inside(at, x, y0, x + kArrowSize, y0 + kArrowSize);
    ArrowSquare(p_, x, y0, kArrowSize, right ? 1 : 0, enabled, over);
    return over && in_.click;
}

void List::Rail(float view)
{
    const float x0 = railX0_, x1 = railX0_ + kRailWidth, top = rows_.top, bottom = listBottom_;
    const float most = std::max(1.0f, y_ - view);
    const ImVec2 at = p_.ToDesign(in_.pointer);
    const double now = ImGui::GetTime();
    const float dt = std::clamp(ImGui::GetIO().DeltaTime, 0.0f, 0.1f);
    // Its arrows at both ends, the track between them with the thumb (as long
    // as the screen shows of the items).
    const float trackTop = top + kRailWidth + 8.0f, trackBottom = bottom - kRailWidth - 8.0f;
    const float track = std::max(1.0f, trackBottom - trackTop);
    const float thumb = std::clamp(track * view / std::max(view, y_), 44.0f, track);
    const float thumbTop = trackTop + (track - thumb) * std::clamp(state_.scroll / most, 0.0f, 1.0f);
    const bool shown = in_.pointer_shown;
    const bool overUp = shown && Inside(at, x0, top, x1, top + kRailWidth);
    const bool overDown = shown && Inside(at, x0, bottom - kRailWidth, x1, bottom);
    const bool overThumb = shown && Inside(at, x0, thumbTop, x1, thumbTop + thumb);
    const bool overTrack = shown && Inside(at, x0, trackTop, x1, trackBottom);
    const float rowStep = kRowHeight + kGap;
    if (in_.click)
    {
        if (overUp || overDown)
        {
            // A row's step, then on while held.
            state_.hold = overUp ? -1 : 1;
            state_.since = now;
            state_.target += float(state_.hold) * rowStep;
            Play(Sound::Step);
        }
        else if (overThumb)
        {
            state_.dragging = true;
            state_.drag = at.y - thumbTop;
        }
        else if (overTrack)
        {
            state_.target += (at.y < thumbTop ? -1.0f : 1.0f) * view * 0.85f;
            Play(Sound::Step);
        }
    }
    if (!in_.held) state_.hold = 0, state_.dragging = false;
    if (state_.hold != 0 && now - state_.since > 0.35 && (state_.hold < 0 ? overUp : overDown))
        state_.target += float(state_.hold) * 900.0f * dt;
    if (state_.dragging)
    {
        const float f = std::clamp((at.y - state_.drag - trackTop) / std::max(1.0f, track - thumb), 0.0f, 1.0f);
        state_.target = state_.scroll = f * most;
    }
    state_.target = std::clamp(state_.target, 0.0f, most);
    // Drawn: the track a dark groove, the thumb as the silver arrow (gold
    // under the hand or dragged).
    p_.Rect(x0 + 12.0f, trackTop, x1 - 12.0f, trackBottom, p_.Col(0, 0, 0, 160), 8.0f);
    const bool hot = overThumb || state_.dragging;
    p_.Rect(x0 + 3.0f, thumbTop - 1.0f, x1 - 3.0f, thumbTop + thumb + 1.0f, p_.Col(16, 18, 20, 230), 9.0f);
    p_.RoundGradient(x0 + 5.0f, thumbTop + 1.0f, x1 - 5.0f, thumbTop + thumb - 1.0f,
                     hot ? p_.Col(255, 226, 120) : p_.Col(232, 236, 240),
                     hot ? p_.Col(220, 160, 30) : p_.Col(150, 156, 164), 7.0f);
    for (int n = -1; n <= 1; ++n)
    {
        const float y = thumbTop + thumb * 0.5f + float(n) * 7.0f;
        p_.Rect(x0 + 14.0f, y - 1.0f, x1 - 14.0f, y + 1.0f, p_.Col(40, 44, 48, 160));
    }
    ArrowSquare(p_, x0, top, kRailWidth, 2, state_.scroll > 0.5f, overUp);
    ArrowSquare(p_, x0, bottom - kRailWidth, kRailWidth, 3, state_.scroll < most - 0.5f, overDown);
}

bool List::Plate(const char* label, const char* about, bool enabled)
{
    Slot slot = Place(kPlateHeight, enabled);
    if (enabled) LightUnderPointer(slot);
    const float mid = (slot.top + slot.bottom) * 0.5f - 3.0f;
    const float cx = (rows_.x0 + rows_.x1) * 0.5f, half = (rows_.x1 - rows_.x0) * 0.5f - 30.0f;
    PlateAt(enabled ? p_ : p_.Faded(0.45f), fonts_.plate, cx, half, mid, label, enabled && slot.lit, false,
            kPlateSize * kListPlate);
    Help(label, about, enabled && slot.lit);
    return enabled && Pressed(slot, false);
}

bool List::Row(const char* label, const char* value, const char* about)
{
    const Slot slot = ValueRow(label, about);
    const float mid = (slot.top + slot.bottom) * 0.5f;
    p_.Text(fonts_.row, kRowSize * 0.9f, rows_.x1 - 56.0f, mid, value, Orange(p_), p_.Col(6, 6, 6), 2.0f, 1.0f);
    Chevron(mid, slot.lit);
    return Pressed(slot, true);
}

std::optional<int> List::Bars(const char* label, int value, int min, int max, int step, int bars, const char* text,
                              const char* about)
{
    const Slot drawn = ValueRow(label, about);
    const Slot& slot = drawn;
    const bool lit = slot.lit;
    const float mid = (slot.top + slot.bottom) * 0.5f;
    // From the right: the value in orange, the right arrow, the bars, the
    // left arrow (the game's Audio Options, in a row).
    const float textRight = rows_.x1 - 22.0f, textWidth = 112.0f;
    const float rightArrow = textRight - textWidth - 12.0f - kArrowSize;
    const float barsWidth = std::clamp(rows_.x1 - rows_.x0 - 470.0f - textWidth - 2.0f * kArrowSize, 180.0f, 420.0f);
    const float barsRight = rightArrow - 14.0f, barsLeft = barsRight - barsWidth;
    const float leftArrow = barsLeft - 14.0f - kArrowSize;
    int next = value;
    if (Arrow(drawn, leftArrow, false, value > min)) next = value - step;
    if (Arrow(drawn, rightArrow, true, value < max)) next = value + step;
    const float gap = bars > 12 ? 4.0f : 7.0f;
    const float barWidth = (barsWidth - gap * float(bars - 1)) / float(bars);
    const int range = std::max(1, max - min);
    const int filled = int(std::lround(float(value - min) * float(bars) / float(range)));
    for (int n = 0; n < bars; ++n)
    {
        const float x0 = barsLeft + float(n) * (barWidth + gap), x1 = x0 + barWidth;
        // Rising towards the right, as the game's.
        const float rise = 5.0f + 24.0f * float(n) / float(std::max(1, bars - 1));
        const float y0 = mid + 16.0f - rise - 8.0f, y1 = mid + 16.0f;
        if (n < filled)
        {
            p_.Rect(x0, y0, x1, y1, p_.Col(kBarR, kBarG, kBarB), 1.0f);
            p_.Rect(x0, y0, x1, y0 + 2, p_.Col(230, 255, 170, 140));
        }
        else p_.Rect(x0, y0, x1, y1, p_.Col(0, 0, 0, 200), 1.0f);
    }
    p_.Text(fonts_.row, kRowSize * 0.95f, textRight, mid, text, Orange(p_), p_.Col(6, 6, 6), 2.5f, 1.0f);
    if (lit && in_.left) next = value - step;
    if (lit && in_.right) next = value + step;
    // Held on the bars: the value under the hand.
    const ImVec2 at = p_.ToDesign(in_.pointer);
    if (slot.hovered && in_.held && at.x >= barsLeft - 6.0f && at.x <= barsRight + 6.0f)
    {
        const float f = std::clamp((at.x - barsLeft) / barsWidth, 0.0f, 1.0f);
        const int steps = (range + step - 1) / step;
        next = min + int(std::lround(f * float(steps))) * step;
    }
    next = std::clamp(next, min, max);
    if (next == value) return std::nullopt;
    Play(Sound::Step);
    return next;
}

std::optional<bool> List::Switch(const char* label, bool on, const char* about, const char* off_text,
                                 const char* on_text)
{
    const Slot drawn = ValueRow(label, about);
    const Slot& slot = drawn;
    const bool lit = slot.lit;
    const char* texts[2] = {off_text, on_text};
    const float w = std::max({130.0f, p_.TextWidth(fonts_.small, kSmallSize, off_text) + 36.0f,
                              p_.TextWidth(fonts_.small, kSmallSize, on_text) + 36.0f});
    const float right = rows_.x1 - 22.0f;
    int clicked = -1;
    for (int n = 0; n < 2; ++n)
    {
        const float x1 = right - float(1 - n) * (w + 12.0f), x0 = x1 - w;
        if (Pill(drawn, x0, x1, texts[n], (n == 1) == on)) clicked = n;
    }
    bool next = on;
    if (lit && (in_.left || in_.right || in_.accept)) next = !on;
    if (clicked >= 0) next = clicked == 1;
    else if (slot.hovered && in_.click && !lit) next = on;
    if (next == on) return std::nullopt;
    Play(Sound::Switch);
    return next;
}

std::optional<int> List::Choice(const char* label, const std::vector<std::string>& choices, int index,
                                const char* about)
{
    const Slot drawn = ValueRow(label, about);
    const bool lit = drawn.lit;
    if (choices.empty()) return std::nullopt;
    const int count = int(choices.size());
    index = std::clamp(index, 0, count - 1);
    // Each choice its own button, right-aligned.
    const auto width = [&](const std::string& choice) {
        return std::max(84.0f, p_.TextWidth(fonts_.small, kSmallSize, choice.c_str()) + 34.0f);
    };
    float total = 0.0f;
    for (const auto& choice : choices) total += width(choice) + 10.0f;
    int clicked = -1;
    float x = rows_.x1 - 22.0f - total + 10.0f;
    for (int n = 0; n < count; ++n)
    {
        const float x0 = x, x1 = x + width(choices[std::size_t(n)]);
        if (Pill(drawn, x0, x1, choices[std::size_t(n)].c_str(), n == index)) clicked = n;
        x = x1 + 10.0f;
    }
    int next = index;
    if (lit && in_.left) next = std::max(0, index - 1);
    if (lit && in_.right) next = std::min(count - 1, index + 1);
    if (lit && in_.accept) next = (index + 1) % count;
    if (clicked >= 0) next = clicked;
    if (next == index) return std::nullopt;
    Play(Sound::Switch);
    return next;
}

namespace
{
ImU32 ToneColour(const Painter& p, Tone tone)
{
    switch (tone)
    {
    case Tone::Good: return p.Col(140, 228, 110);
    case Tone::Busy: return Orange(p);
    case Tone::Away: return p.Col(150, 160, 166);
    case Tone::Bad: return p.Col(255, 110, 92);
    default: return p.Col(226, 232, 236);
    }
}
} // namespace

void DrawPerson(const Painter& p, ImFont* font, float cx, float cy, float r, const Person& person)
{
    const ImVec2 centre = p.At(cx, cy);
    const float radius = r * p.s;
    // A dark ring, then the picture or the initials on their colour.
    p.draw->AddCircleFilled(centre, radius + 3.0f * p.s, p.Col(10, 12, 14, 230), 40);
    if (person.image)
        p.draw->AddImageRounded(person.image, {centre.x - radius, centre.y - radius},
                                {centre.x + radius, centre.y + radius}, {0, 0}, {1, 1}, p.Col(255, 255, 255), radius);
    else
    {
        constexpr int kTints[8][3] = {{214, 84, 64},  {224, 150, 40}, {120, 170, 60}, {50, 150, 130},
                                      {60, 120, 200}, {120, 90, 200}, {190, 80, 150}, {110, 120, 130}};
        const int* tint = kTints[std::size_t(person.tint % 8 + 8) % 8];
        p.draw->AddCircleFilled(centre, radius, p.Col(tint[0], tint[1], tint[2]), 40);
        p.Text(font, r * 1.05f, cx, cy, person.initials.c_str(), p.Col(255, 255, 255), p.Col(0, 0, 0, 120), 1.5f);
    }
    p.draw->AddCircle(centre, radius, p.Col(255, 255, 255, 60), 40, std::max(1.0f, 1.5f * p.s));
    if (person.presence >= 0)
    {
        // Free, busy or away: a dot at the lower right.
        const ImVec2 dot{centre.x + radius * 0.72f, centre.y + radius * 0.72f};
        const float d = std::max(4.0f, r * 0.24f * p.s);
        const ImU32 colour = person.presence == 0 ? p.Col(110, 220, 90)
                             : person.presence == 1 ? Orange(p) : p.Col(140, 148, 152);
        p.draw->AddCircleFilled(dot, d + 2.0f * p.s, p.Col(10, 12, 14), 20);
        p.draw->AddCircleFilled(dot, d, colour, 20);
    }
}

float DrawFlag(const Painter& p, float x, float y, float height, const Person& person)
{
    if (!person.flags || person.country.size() != 2) return 0.0f;
    const int cell = overlay::FlagCell(person.country);
    if (cell < 0) return 0.0f;
    const auto uv = overlay::FlagCellUv(cell);
    const float width = height * float(overlay::kFlagWidth) / float(overlay::kFlagHeight);
    p.Rect(x - 1.0f, y - height * 0.5f - 1.0f, x + width + 1.0f, y + height * 0.5f + 1.0f, p.Col(0, 0, 0, 160), 2.0f);
    p.draw->AddImage(person.flags, p.At(x, y - height * 0.5f), p.At(x + width, y + height * 0.5f), {uv.u0, uv.v0},
                     {uv.u1, uv.v1}, p.Col(255, 255, 255));
    return width;
}

bool List::PlayerRow(const Person& person, const char* name, const char* status, Tone tone, const char* tag,
                     const char* about)
{
    Slot slot = Place(90.0f, true);
    LightUnderPointer(slot);
    const bool lit = slot.lit;
    Bar(p_, rows_.x0, slot.top, rows_.x1, slot.bottom, lit, lit);
    Help(name, about, lit);
    const float mid = (slot.top + slot.bottom) * 0.5f;
    DrawPerson(p_, fonts_.row, rows_.x0 + 52.0f, mid, 32.0f, person);
    const float x = rows_.x0 + 104.0f;
    p_.Text(fonts_.row, kRowSize, x, mid - 16.0f, name, lit ? p_.Col(255, 255, 255) : p_.Col(236, 238, 240),
            p_.Col(6, 6, 6), 2.5f, 0.0f);
    const float nameWidth = p_.TextWidth(fonts_.row, kRowSize, name);
    DrawFlag(p_, x + nameWidth + 14.0f, mid - 16.0f, 22.0f, person);
    if (status && *status)
        p_.Text(fonts_.small, kSmallSize * 0.92f, x, mid + 22.0f, status, ToneColour(p_, tone), p_.Col(6, 6, 6, 180),
                1.5f, 0.0f);
    Chevron(mid, lit);
    const float right = rows_.x1 - 58.0f;
    if (tag && *tag)
    {
        const float w = p_.TextWidth(fonts_.small, kSmallSize * 0.85f, tag) + 20.0f;
        p_.Frame(right - w, mid - 17.0f, right, mid + 17.0f, p_.Col(240, 240, 240, 150), 6.0f, 1.5f);
        p_.Text(fonts_.small, kSmallSize * 0.85f, right - w * 0.5f, mid, tag, p_.Col(236, 238, 240), 0, 0.0f);
    }
    return Pressed(slot, true);
}

void List::Card(const Person* person, const char* kicker, const char* title, const std::vector<std::string>& lines,
                Tone tone, int games, int won, int lost)
{
    // Its text beside the picture (and before the score), lines wrapped.
    const float textX = person ? rows_.x0 + 162.0f : rows_.x0 + 26.0f;
    const float textRight = rows_.x1 - (games > 0 ? 200.0f : 26.0f);
    const float wrap = std::max(120.0f, textRight - textX), lineSize = kSmallSize * 0.95f;
    std::vector<float> heights;
    float linesHeight = 0.0f;
    for (const auto& line : lines)
    {
        heights.push_back(p_.WrappedHeight(fonts_.small, lineSize, wrap, line.c_str(), 30.0f));
        linesHeight += heights.back() + 4.0f;
    }
    const float heading = (kicker && *kicker ? 40.0f : 0.0f) + (title && *title ? 44.0f : 0.0f);
    const float height = std::max(person ? 136.0f : 0.0f, 24.0f + heading + linesHeight + 12.0f);
    const Slot slot = Place(height, false);
    const float x0 = rows_.x0, x1 = rows_.x1, top = slot.top;
    // As the rows' bars, a little lighter, the game's orange along its top.
    p_.Gradient(x0, top, x1, slot.bottom, p_.Col(30, 40, 46, 225), p_.Col(10, 14, 16, 225));
    p_.Rect(x0, top, x1, top + 4, Orange(p_, 200));
    if (person) DrawPerson(p_, fonts_.plate, x0 + 82.0f, top + height * 0.5f, 52.0f, *person);
    float y = top + 30.0f;
    if (kicker && *kicker)
    {
        p_.Text(fonts_.small, kSmallSize, textX, y, kicker, Orange(p_), p_.Col(4, 12, 16, 220), 2.0f, 0.0f);
        y += 40.0f;
    }
    if (title && *title)
    {
        p_.Text(fonts_.plate, kPlateSize * 0.82f, textX, y, title, p_.Col(250, 250, 250), p_.Col(6, 6, 6), 3.0f, 0.0f);
        if (person) DrawFlag(p_, textX + p_.TextWidth(fonts_.plate, kPlateSize * 0.82f, title) + 16.0f, y, 26.0f, *person);
        y += 26.0f;
    }
    else y -= 14.0f;
    for (std::size_t n = 0; n < lines.size(); ++n)
    {
        p_.Wrapped(fonts_.small, lineSize, textX, y, wrap, lines[n].c_str(),
                   n == 0 ? ToneColour(p_, tone) : p_.Col(220, 228, 232));
        y += heights[n] + 4.0f;
    }
    if (games > 0)
    {
        // The series: the games won and lost, big, at the right; best of under it.
        const std::string score = std::to_string(won) + " \xE2\x80\x93 " + std::to_string(lost);
        p_.Text(fonts_.title, kTitleSize * 0.8f, x1 - 30.0f, top + height * 0.42f, score.c_str(), p_.Col(250, 250, 250),
                p_.Col(6, 6, 6), 3.0f, 1.0f);
        const std::string of = "BEST OF " + std::to_string(games);
        p_.Text(fonts_.small, kSmallSize * 0.85f, x1 - 30.0f, top + height * 0.42f + 46.0f, of.c_str(), Orange(p_),
                p_.Col(4, 12, 16, 220), 1.5f, 1.0f);
    }
}

void List::BoardRow(int rank, const char* name, const char* record, const char* points, bool you,
                    const Person* person)
{
    Slot slot = Place(64.0f, true);
    LightUnderPointer(slot);
    const bool lit = slot.lit;
    Bar(p_, rows_.x0, slot.top, rows_.x1, slot.bottom, you || lit, lit);
    if (you) p_.Rect(rows_.x0, slot.top, rows_.x0 + 7, slot.bottom, p_.Col(255, 200, 60), 2.0f);
    const float mid = (slot.top + slot.bottom) * 0.5f;
    const std::string place = rank > 0 ? std::to_string(rank) : std::string("-");
    p_.Text(fonts_.row, kRowSize * 1.05f, rows_.x0 + 92.0f, mid, place.c_str(),
            rank > 0 && rank <= 3 ? p_.Col(255, 214, 80) : p_.Col(236, 238, 240), p_.Col(6, 6, 6), 2.5f, 1.0f);
    float x = rows_.x0 + 118.0f;
    p_.Text(fonts_.row, kRowSize, x, mid, name, you ? Orange(p_) : p_.Col(240, 242, 244), p_.Col(6, 6, 6), 2.5f, 0.0f);
    if (person) DrawFlag(p_, x + p_.TextWidth(fonts_.row, kRowSize, name) + 14.0f, mid, 22.0f, *person);
    p_.Text(fonts_.row, kRowSize, rows_.x1 - 24.0f, mid, points, Orange(p_), p_.Col(6, 6, 6), 2.5f, 1.0f);
    if (record && *record)
        p_.Text(fonts_.small, kSmallSize * 0.92f, rows_.x1 - 190.0f, mid, record, p_.Col(200, 210, 214),
                p_.Col(6, 6, 6, 160), 1.5f, 1.0f);
}

std::optional<int> List::Cycle(const char* label, const char* value, const char* about)
{
    const Slot drawn = ValueRow(label, about);
    const Slot& slot = drawn;
    const bool lit = slot.lit;
    // The value between the game's arrow buttons, at the right.
    const float width = 380.0f, right = rows_.x1 - 22.0f;
    const float rightArrow = right - kArrowSize, leftArrow = right - width;
    int step = 0;
    if (Arrow(drawn, leftArrow, false, true)) step = -1;
    if (Arrow(drawn, rightArrow, true, true)) step = 1;
    p_.Text(fonts_.small, kSmallSize, (leftArrow + kArrowSize + rightArrow) * 0.5f, (slot.top + slot.bottom) * 0.5f,
            value, Orange(p_), p_.Col(6, 6, 6), 2.0f);
    if (lit && in_.left) step = -1;
    if (lit && (in_.right || in_.accept)) step = 1;
    if (step == 0) return std::nullopt;
    Play(Sound::Switch);
    return step;
}
} // namespace mscharged::ui
