// Builds the robot goalie map (assets/gameplay/robot_goalie.map): for each texel of a
// Charged goalie texture, where on the GameCube robot goalie texture its colour comes from.
// Run by make_map.py, which reads both games' files; see assets/gameplay/README.md.
//
//   make_map GAMECUBE.msh CHARGED.msh OUT.map --target HASH:WIDTH:HEIGHT...
//   make_map --apply MAP SOURCE.rgba PREFIX   (preview: the made textures, PREFIX<hash>.rgba)
//
// Both Kritters stand in the same T-pose with the same bone orientations, but Charged's has
// other proportions (arms 2.5-4x longer). A Charged surface point is moved into the GameCube
// body bone by bone: bones only one skeleton has join their nearest shared ancestor (a group);
// along a group's bone the joint-to-joint length ratio applies, across it one radial factor.
// Its texel then takes the texture coordinates of the nearest GameCube surface facing the same
// way. GameCube surfaces nobody sees (the body under the jersey) are left out first.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
struct V3
{
    float x = 0, y = 0, z = 0;
};
V3 operator+(V3 a, V3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
V3 operator-(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
V3 operator*(V3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
float Dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
V3 Cross(V3 a, V3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
float Length(V3 a) { return std::sqrt(Dot(a, a)); }

struct Corner
{
    V3 pos;
    float uv[2];
    std::uint32_t bone[4];
    float weight[4];
    std::uint32_t texture;
};
struct Matrix
{
    float m[16];  // NL row-major: rows 0-2 the axes, row 3 the translation (world = local * M)
};
struct Mesh
{
    std::vector<Corner> corners;  // three per triangle
    std::map<std::uint32_t, Matrix> binds;
    std::map<std::uint32_t, std::uint32_t> parents;
};

template<class T> void Read(FILE* f, T* out, std::size_t count)
{
    if (std::fread(out, sizeof(T), count, f) != count) throw std::runtime_error("truncated mesh");
}

Mesh LoadMesh(const char* path)
{
    FILE* f = std::fopen(path, "rb");
    if (!f) throw std::runtime_error(std::string("cannot open ") + path);
    char magic[4];
    std::uint32_t triangles = 0, count = 0;
    Read(f, magic, 4);
    if (std::memcmp(magic, "MSH2", 4) != 0) throw std::runtime_error("not a MSH2 mesh");
    Read(f, &triangles, 1);
    Mesh mesh;
    mesh.corners.resize(std::size_t(triangles) * 3);
    for (Corner& c : mesh.corners)
    {
        Read(f, &c.pos.x, 3);
        Read(f, c.uv, 2);
        Read(f, c.bone, 4);
        Read(f, c.weight, 4);
        Read(f, &c.texture, 1);
    }
    Read(f, &count, 1);
    for (std::uint32_t i = 0; i < count; ++i)
    {
        std::uint32_t bone;
        Matrix m;
        Read(f, &bone, 1);
        Read(f, m.m, 16);
        mesh.binds[bone] = m;
    }
    Read(f, &count, 1);
    for (std::uint32_t i = 0; i < count; ++i)
    {
        std::uint32_t pair[2];
        Read(f, pair, 2);
        mesh.parents[pair[0]] = pair[1];
    }
    std::fclose(f);
    return mesh;
}

V3 ToLocal(const Matrix& m, V3 p)
{
    V3 d = p - V3{m.m[12], m.m[13], m.m[14]};
    return {Dot(d, {m.m[0], m.m[1], m.m[2]}), Dot(d, {m.m[4], m.m[5], m.m[6]}), Dot(d, {m.m[8], m.m[9], m.m[10]})};
}
V3 ToWorld(const Matrix& m, V3 l)
{
    return V3{m.m[12], m.m[13], m.m[14]} + V3{m.m[0], m.m[1], m.m[2]} * l.x + V3{m.m[4], m.m[5], m.m[6]} * l.y
        + V3{m.m[8], m.m[9], m.m[10]} * l.z;
}
V3 Axis(const Matrix& m) { return {m.m[0], m.m[1], m.m[2]}; }
V3 Origin(const Matrix& m) { return {m.m[12], m.m[13], m.m[14]}; }

struct Triangle
{
    V3 p[3];
    float uv[3][2];
    V3 n;
};

// Closest point of a triangle (Ericson, Real-Time Collision Detection 5.1.5) as barycentrics.
void Closest(const Triangle& t, V3 p, float& u, float& v, float& w)
{
    V3 a = t.p[0], b = t.p[1], c = t.p[2], ab = b - a, ac = c - a, ap = p - a;
    float d1 = Dot(ab, ap), d2 = Dot(ac, ap);
    if (d1 <= 0 && d2 <= 0) { u = 1; v = w = 0; return; }
    V3 bp = p - b;
    float d3 = Dot(ab, bp), d4 = Dot(ac, bp);
    if (d3 >= 0 && d4 <= d3) { v = 1; u = w = 0; return; }
    float vc = d1 * d4 - d3 * d2;
    if (vc <= 0 && d1 >= 0 && d3 <= 0) { float s = d1 / (d1 - d3); u = 1 - s; v = s; w = 0; return; }
    V3 cp = p - c;
    float d5 = Dot(ab, cp), d6 = Dot(ac, cp);
    if (d6 >= 0 && d5 <= d6) { w = 1; u = v = 0; return; }
    float vb = d5 * d2 - d1 * d6;
    if (vb <= 0 && d2 >= 0 && d6 <= 0) { float s = d2 / (d2 - d6); u = 1 - s; w = s; v = 0; return; }
    float va = d3 * d6 - d5 * d4;
    if (va <= 0 && (d4 - d3) >= 0 && (d5 - d6) >= 0)
    {
        float s = (d4 - d3) / ((d4 - d3) + (d5 - d6));
        v = 1 - s; w = s; u = 0; return;
    }
    float denominator = 1 / (va + vb + vc);
    v = vb * denominator; w = vc * denominator; u = 1 - v - w;
}

bool RayHits(const std::vector<Triangle>& triangles, V3 o, V3 d, std::size_t skip, float maxT)
{
    for (std::size_t j = 0; j < triangles.size(); ++j)
    {
        if (j == skip) continue;
        const Triangle& t = triangles[j];
        V3 e1 = t.p[1] - t.p[0], e2 = t.p[2] - t.p[0], pv = Cross(d, e2);
        float det = Dot(e1, pv);
        if (std::fabs(det) < 1e-12f) continue;
        float inv = 1 / det;
        V3 tv = o - t.p[0];
        float u = Dot(tv, pv) * inv;
        if (u < 0 || u > 1) continue;
        V3 qv = Cross(tv, e1);
        float v = Dot(d, qv) * inv;
        if (v < 0 || u + v > 1) continue;
        float distance = Dot(e2, qv) * inv;
        if (distance > 1e-4f && distance < maxT) return true;
    }
    return false;
}

constexpr int kMaxSamples = 4;
struct Sample
{
    std::uint16_t u, v;
};
struct TexelSamples
{
    std::vector<std::pair<std::uint32_t, int>> hits;  // packed (u, v) and how often
    bool interior = false;
};

class Mapper
{
public:
    Mapper(Mesh gamecube, Mesh charged) : gc_(std::move(gamecube)), wii_(std::move(charged))
    {
        FitGroups();
        CollectVisibleTriangles();
    }

    // Texels of the texture the Charged packets bind as `texture`, `width` x `height`.
    std::vector<std::vector<Sample>> Bake(std::uint32_t texture, int width, int height) const
    {
        std::vector<TexelSamples> texels(std::size_t(width) * height);
        int triangles = 0;
        for (std::size_t t = 0; t + 2 < wii_.corners.size(); t += 3)
        {
            const Corner* c[3] = {&wii_.corners[t], &wii_.corners[t + 1], &wii_.corners[t + 2]};
            if (c[0]->texture != texture) continue;
            V3 normal = Cross(c[1]->pos - c[0]->pos, c[2]->pos - c[0]->pos);
            float length = Length(normal);
            if (length < 1e-12f) continue;
            normal = normal * (1 / length);
            ++triangles;
            V3 moved[3];
            float sx[3], sy[3];
            for (int i = 0; i < 3; ++i)
            {
                moved[i] = Move(*c[i]);
                sx[i] = c[i]->uv[0] * width;
                sy[i] = c[i]->uv[1] * height;
            }
            float area = (sx[1] - sx[0]) * (sy[2] - sy[0]) - (sx[2] - sx[0]) * (sy[1] - sy[0]);
            if (std::fabs(area) < 1e-6f) continue;
            // Texels within about one texel of the edges too, so filtering never reaches unpainted ones.
            const float margin = 1.0f / std::sqrt(std::fabs(area));
            int x0 = int(std::floor(std::min({sx[0], sx[1], sx[2]}))) - 1, x1 = int(std::ceil(std::max({sx[0], sx[1], sx[2]}))) + 1;
            int y0 = int(std::floor(std::min({sy[0], sy[1], sy[2]}))) - 1, y1 = int(std::ceil(std::max({sy[0], sy[1], sy[2]}))) + 1;
            for (int y = std::max(y0, 0); y <= std::min(y1, height - 1); ++y)
                for (int x = std::max(x0, 0); x <= std::min(x1, width - 1); ++x)
                {
                    float px = x + 0.5f, py = y + 0.5f;
                    float w0 = ((sx[1] - px) * (sy[2] - py) - (sx[2] - px) * (sy[1] - py)) / area;
                    float w1 = ((sx[2] - px) * (sy[0] - py) - (sx[0] - px) * (sy[2] - py)) / area;
                    float w2 = 1 - w0 - w1;
                    bool inside = w0 >= 0 && w1 >= 0 && w2 >= 0;
                    if (!inside && (w0 < -margin || w1 < -margin || w2 < -margin)) continue;
                    TexelSamples& texel = texels[std::size_t(y) * width + x];
                    if (!inside && texel.interior) continue;  // edge texels never override an interior hit
                    if (inside && !texel.interior) { texel.hits.clear(); texel.interior = true; }
                    w0 = std::max(w0, 0.f); w1 = std::max(w1, 0.f); w2 = std::max(w2, 0.f);
                    float sum = w0 + w1 + w2;
                    V3 p = (moved[0] * w0 + moved[1] * w1 + moved[2] * w2) * (1 / sum);
                    float uv[2];
                    Lookup(p, normal, uv);
                    // Charged reuses some kit areas for several places (plain strap colour on the
                    // retail kits): every contribution counts, they are averaged at run time.
                    std::uint32_t packed = (std::uint32_t(Quantize(uv[0])) << 16) | Quantize(uv[1]);
                    auto it = std::find_if(texel.hits.begin(), texel.hits.end(), [&](auto& h) { return Near(h.first, packed); });
                    if (it == texel.hits.end()) texel.hits.push_back({packed, 1});
                    else ++it->second;
                }
        }
        if (!triangles) throw std::runtime_error("no Charged packet binds the target texture");
        std::vector<std::vector<Sample>> out(texels.size());
        for (std::size_t k = 0; k < texels.size(); ++k)
        {
            auto hits = texels[k].hits;
            std::stable_sort(hits.begin(), hits.end(), [](auto& a, auto& b) { return a.second > b.second; });
            for (std::size_t i = 0; i < hits.size() && i < kMaxSamples; ++i)
                out[k].push_back({std::uint16_t(hits[i].first >> 16), std::uint16_t(hits[i].first)});
        }
        // Texels no triangle reaches take the samples of a painted neighbour, ring by ring.
        for (int pass = 0;; ++pass)
        {
            auto next = out;
            bool changed = false, empty = false;
            for (int y = 0; y < height; ++y)
                for (int x = 0; x < width; ++x)
                {
                    std::size_t k = std::size_t(y) * width + x;
                    if (!out[k].empty()) continue;
                    empty = true;
                    for (int dy = -1; dy <= 1 && next[k].empty(); ++dy)
                        for (int dx = -1; dx <= 1 && next[k].empty(); ++dx)
                        {
                            int xx = x + dx, yy = y + dy;
                            if (xx < 0 || yy < 0 || xx >= width || yy >= height) continue;
                            const auto& n = out[std::size_t(yy) * width + xx];
                            if (!n.empty()) { next[k] = n; changed = true; }
                        }
                }
            out.swap(next);
            if (!empty) break;
            if (!changed) throw std::runtime_error("target texture has no painted texel");
        }
        std::printf("target %08x %dx%d: %d triangles\n", texture, width, height, triangles);
        return out;
    }

private:
    static std::uint16_t Quantize(float t)
    {
        t -= std::floor(t);  // the robot texture repeats
        return std::uint16_t(std::lround(std::clamp(t, 0.0f, 1.0f) * 65535.0f));
    }
    static bool Near(std::uint32_t a, std::uint32_t b)  // within half a texel of a 512x256 texture
    {
        int du = int(a >> 16) - int(b >> 16), dv = int(a & 0xFFFF) - int(b & 0xFFFF);
        return std::abs(du) < 64 && std::abs(dv) < 128;
    }
    static std::uint32_t Dominant(const Corner& c)
    {
        int k = 0;
        for (int i = 1; i < 4; ++i) if (c.weight[i] > c.weight[k]) k = i;
        return c.bone[k];
    }

    // The nearest ancestor (or the bone) that both skeletons bind.
    std::uint32_t Shared(std::uint32_t bone, const std::map<std::uint32_t, std::uint32_t>& parents) const
    {
        for (int guard = 0; guard < 64 && bone; ++guard)
        {
            if (gc_.binds.count(bone) && wii_.binds.count(bone)) return bone;
            auto it = parents.find(bone);
            bone = it == parents.end() ? 0 : it->second;
        }
        return 0;
    }
    std::uint32_t Group(std::uint32_t bone, const std::map<std::uint32_t, std::uint32_t>& parents) const
    {
        std::uint32_t g = Shared(bone, parents);
        auto it = folded_.find(g);
        return it == folded_.end() ? g : it->second;
    }

    struct Stats
    {
        float lo = 1e9f, hi = -1e9f;
        double radial = 0;
        int n = 0;
    };
    struct Fit
    {
        float along = 1, across = 1;
    };

    void FitGroups()
    {
        std::map<std::uint32_t, int> gcCount, wiiCount;
        for (const Corner& c : gc_.corners) ++gcCount[Shared(Dominant(c), gc_.parents)];
        for (const Corner& c : wii_.corners) ++wiiCount[Shared(Dominant(c), wii_.parents)];
        // Groups with few vertices on either side (finger tips, toes) give unstable fits: they
        // join their shared parent until both meshes have enough vertices there.
        constexpr int kMinimumCorners = 40;
        for (auto& [g0, n] : wiiCount)
        {
            std::uint32_t g = g0;
            for (int guard = 0; guard < 64 && g && (gcCount[g] < kMinimumCorners || wiiCount[g] < kMinimumCorners); ++guard)
            {
                auto it = wii_.parents.find(g);
                g = it == wii_.parents.end() ? 0 : Shared(it->second, wii_.parents);
            }
            folded_[g0] = g ? g : g0;
        }
        std::map<std::uint32_t, Stats> gcStats, wiiStats;
        auto add = [](Stats& s, V3 l) {
            s.lo = std::min(s.lo, l.x); s.hi = std::max(s.hi, l.x);
            s.radial += double(l.y) * l.y + double(l.z) * l.z; ++s.n;
        };
        for (const Corner& c : gc_.corners)
            if (std::uint32_t g = Group(Dominant(c), gc_.parents)) add(gcStats[g], ToLocal(gc_.binds.at(g), c.pos));
        for (const Corner& c : wii_.corners)
            if (std::uint32_t g = Group(Dominant(c), wii_.parents)) add(wiiStats[g], ToLocal(wii_.binds.at(g), c.pos));
        std::map<std::uint32_t, std::vector<std::uint32_t>> children;
        for (auto& [bone, parent] : wii_.parents)
        {
            std::uint32_t g = Group(bone, wii_.parents), pg = parent ? Group(parent, wii_.parents) : 0;
            if (g && pg && g != pg) children[pg].push_back(g);
        }
        for (auto& [g, w] : wiiStats)
        {
            auto found = gcStats.find(g);
            if (found == gcStats.end()) continue;
            const Stats& s = found->second;
            float lw = 0, lg = 0;
            for (std::uint32_t c : children[g])
            {
                float dw = Dot(Origin(wii_.binds.at(c)) - Origin(wii_.binds.at(g)), Axis(wii_.binds.at(g)));
                float dg = Dot(Origin(gc_.binds.at(c)) - Origin(gc_.binds.at(g)), Axis(gc_.binds.at(g)));
                if (dw > lw) { lw = dw; lg = dg; }
            }
            if (lw < 1e-3f || lg < 1e-3f)  // an end bone: its vertices' reach
            {
                lw = std::max(std::fabs(w.hi), std::fabs(w.lo));
                lg = std::max(std::fabs(s.hi), std::fabs(s.lo));
            }
            Fit f;
            f.along = std::clamp(lw > 1e-4f ? lg / lw : 1.0f, 0.25f, 4.0f);
            f.across = std::clamp(float(std::sqrt(s.radial / s.n) / std::max(std::sqrt(w.radial / w.n), 1e-6)), 0.25f, 4.0f);
            fits_[g] = f;
        }
    }

    V3 Move(const Corner& c) const
    {
        V3 sum{};
        float weights = 0;
        for (int i = 0; i < 4; ++i)
        {
            if (c.weight[i] <= 0) continue;
            std::uint32_t g = Group(c.bone[i], wii_.parents);
            auto it = fits_.find(g);
            if (it == fits_.end()) continue;
            V3 l = ToLocal(wii_.binds.at(g), c.pos);
            l = {l.x * it->second.along, l.y * it->second.across, l.z * it->second.across};
            sum = sum + ToWorld(gc_.binds.at(g), l) * c.weight[i];
            weights += c.weight[i];
        }
        return weights > 0 ? sum * (1 / weights) : c.pos;
    }

    void CollectVisibleTriangles()
    {
        std::vector<Triangle> all;
        for (std::size_t t = 0; t + 2 < gc_.corners.size(); t += 3)
        {
            Triangle tri;
            for (int i = 0; i < 3; ++i)
            {
                tri.p[i] = gc_.corners[t + i].pos;
                tri.uv[i][0] = gc_.corners[t + i].uv[0];
                tri.uv[i][1] = gc_.corners[t + i].uv[1];
            }
            V3 n = Cross(tri.p[1] - tri.p[0], tri.p[2] - tri.p[0]);
            float l = Length(n);
            if (l < 1e-12f) continue;
            tri.n = n * (1 / l);
            all.push_back(tri);
        }
        // Hidden: almost every ray into the outer hemisphere hits something, or a close cover
        // lies right above it (a layer of clothing over the body).
        constexpr int kRays = 24;
        constexpr float kCoverDistance = 0.06f;
        for (std::size_t i = 0; i < all.size(); ++i)
        {
            const Triangle& t = all[i];
            V3 centre = (t.p[0] + t.p[1] + t.p[2]) * (1.f / 3);
            V3 a = std::fabs(t.n.x) < 0.9f ? V3{1, 0, 0} : V3{0, 1, 0};
            V3 tu = Cross(t.n, a);
            tu = tu * (1 / Length(tu));
            V3 tv = Cross(t.n, tu);
            int blocked = 0, covered = 0;
            for (int r = 0; r < kRays; ++r)
            {
                float phi = float(r) * 2.399963f;
                float z = 0.15f + 0.85f * (r + 0.5f) / kRays, rr = std::sqrt(1 - z * z);
                V3 d = t.n * z + tu * (rr * std::cos(phi)) + tv * (rr * std::sin(phi));
                blocked += RayHits(all, centre + t.n * 1e-3f, d, i, 1e30f);
                float zc = 0.71f + 0.29f * (r + 0.5f) / kRays, rc = std::sqrt(1 - zc * zc);
                V3 dc = t.n * zc + tu * (rc * std::cos(phi)) + tv * (rc * std::sin(phi));
                covered += RayHits(all, centre + t.n * 1e-3f, dc, i, kCoverDistance);
            }
            if (blocked < kRays - 1 && covered < kRays * 7 / 10) visible_.push_back(t);
        }
        std::printf("GameCube triangles: %zu visible of %zu\n", visible_.size(), all.size());
    }

    void Lookup(V3 p, V3 normal, float uv[2]) const
    {
        float best = 1e30f, bestAny = 1e30f;
        const Triangle* hit = nullptr;
        const Triangle* any = nullptr;
        float b[3]{}, ba[3]{};
        for (const Triangle& t : visible_)
        {
            float u, v, w;
            Closest(t, p, u, v, w);
            V3 d = t.p[0] * u + t.p[1] * v + t.p[2] * w - p;
            float distance = Dot(d, d);
            if (distance < bestAny) { bestAny = distance; any = &t; ba[0] = u; ba[1] = v; ba[2] = w; }
            if (Dot(t.n, normal) > 0.2f && distance < best) { best = distance; hit = &t; b[0] = u; b[1] = v; b[2] = w; }
        }
        if (!hit) { hit = any; std::copy(ba, ba + 3, b); }
        for (int k = 0; k < 2; ++k) uv[k] = hit->uv[0][k] * b[0] + hit->uv[1][k] * b[1] + hit->uv[2][k] * b[2];
    }

    Mesh gc_, wii_;
    std::map<std::uint32_t, std::uint32_t> folded_;
    std::map<std::uint32_t, Fit> fits_;
    std::vector<Triangle> visible_;
};

struct Target
{
    std::uint32_t texture;
    int width, height;
    std::vector<std::vector<Sample>> texels;
};

void Put32(std::vector<std::uint8_t>& out, std::uint32_t v) { for (int i = 0; i < 4; ++i) out.push_back(std::uint8_t(v >> (8 * i))); }
void Put16(std::vector<std::uint8_t>& out, std::uint16_t v) { out.push_back(std::uint8_t(v)); out.push_back(std::uint8_t(v >> 8)); }

// The format read by src/platform/robot_goalie.cpp (little endian):
//   "RGM1", u32 version 1, u32 source texture hash, u32 target count, then per target:
//   u32 texture hash, u16 width, u16 height, u32 sample count,
//   u8 samples per texel (1-4) for each texel in rows, then the samples (u16 s, u16 t; 65535 = 1).
std::vector<std::uint8_t> Serialize(std::uint32_t source, const std::vector<Target>& targets)
{
    std::vector<std::uint8_t> out = {'R', 'G', 'M', '1'};
    Put32(out, 1);
    Put32(out, source);
    Put32(out, std::uint32_t(targets.size()));
    for (const Target& t : targets)
    {
        std::uint32_t samples = 0;
        for (auto& texel : t.texels) samples += std::uint32_t(texel.size());
        Put32(out, t.texture);
        Put16(out, std::uint16_t(t.width));
        Put16(out, std::uint16_t(t.height));
        Put32(out, samples);
        for (auto& texel : t.texels) out.push_back(std::uint8_t(texel.size()));
        for (auto& texel : t.texels)
            for (const Sample& s : texel) { Put16(out, s.u); Put16(out, s.v); }
    }
    return out;
}

// Preview: the map applied to a decoded robot texture (u32 width, u32 height, RGBA bytes).
void Preview(const char* source, const char* prefix, const std::vector<Target>& targets)
{
    FILE* f = std::fopen(source, "rb");
    if (!f) throw std::runtime_error("cannot open preview source");
    std::uint32_t w = 0, h = 0;
    Read(f, &w, 1);
    Read(f, &h, 1);
    std::vector<std::uint8_t> rgba(std::size_t(w) * h * 4);
    Read(f, rgba.data(), rgba.size());
    std::fclose(f);
    auto texel = [&](int x, int y, int c) {
        x = ((x % int(w)) + int(w)) % int(w); y = ((y % int(h)) + int(h)) % int(h);
        return float(rgba[(std::size_t(y) * w + x) * 4 + c]);
    };
    for (const Target& t : targets)
    {
        std::vector<std::uint8_t> out(std::size_t(t.width) * t.height * 4);
        for (std::size_t k = 0; k < t.texels.size(); ++k)
        {
            float acc[4]{};
            for (const Sample& s : t.texels[k])
            {
                float x = s.u / 65535.0f * w - 0.5f, y = s.v / 65535.0f * h - 0.5f;
                int x0 = int(std::floor(x)), y0 = int(std::floor(y));
                float fx = x - x0, fy = y - y0;
                for (int c = 0; c < 4; ++c)
                    acc[c] += (texel(x0, y0, c) * (1 - fx) + texel(x0 + 1, y0, c) * fx) * (1 - fy)
                        + (texel(x0, y0 + 1, c) * (1 - fx) + texel(x0 + 1, y0 + 1, c) * fx) * fy;
            }
            for (int c = 0; c < 4; ++c) out[k * 4 + c] = std::uint8_t(std::lround(acc[c] / t.texels[k].size()));
        }
        char name[512];
        std::snprintf(name, sizeof name, "%s%08x.rgba", prefix, t.texture);
        FILE* o = std::fopen(name, "wb");
        std::uint32_t dims[2] = {std::uint32_t(t.width), std::uint32_t(t.height)};
        std::fwrite(dims, 4, 2, o);
        std::fwrite(out.data(), 1, out.size(), o);
        std::fclose(o);
    }
}
std::vector<Target> LoadMap(const char* path)
{
    FILE* f = std::fopen(path, "rb");
    if (!f) throw std::runtime_error("cannot open the map");
    std::vector<std::uint8_t> bytes;
    for (int c; (c = std::fgetc(f)) != EOF;) bytes.push_back(std::uint8_t(c));
    std::fclose(f);
    std::size_t at = 0;
    auto need = [&](std::size_t n) { if (bytes.size() - at < n) throw std::runtime_error("truncated map"); };
    auto u32 = [&] { need(4); std::uint32_t v = bytes[at] | bytes[at + 1] << 8 | bytes[at + 2] << 16 | std::uint32_t(bytes[at + 3]) << 24; at += 4; return v; };
    auto u16 = [&] { need(2); std::uint16_t v = std::uint16_t(bytes[at] | bytes[at + 1] << 8); at += 2; return v; };
    need(4);
    if (std::memcmp(bytes.data(), "RGM1", 4) != 0) throw std::runtime_error("not a robot goalie map");
    at = 4;
    if (u32() != 1) throw std::runtime_error("unknown map version");
    u32();
    std::vector<Target> targets(u32());
    for (Target& t : targets)
    {
        t.texture = u32();
        t.width = u16();
        t.height = u16();
        std::uint32_t samples = u32(), total = 0;
        need(std::size_t(t.width) * t.height);
        std::vector<int> counts(bytes.begin() + at, bytes.begin() + at + std::size_t(t.width) * t.height);
        at += counts.size();
        t.texels.resize(counts.size());
        for (std::size_t k = 0; k < counts.size(); ++k)
        {
            if (counts[k] < 1 || counts[k] > kMaxSamples) throw std::runtime_error("invalid sample count");
            for (int i = 0; i < counts[k]; ++i) { std::uint16_t u = u16(); t.texels[k].push_back({u, u16()}); }
            total += std::uint32_t(counts[k]);
        }
        if (total != samples) throw std::runtime_error("sample total disagrees");
    }
    if (at != bytes.size()) throw std::runtime_error("trailing map bytes");
    return targets;
}
} // namespace

int main(int argc, char** argv)
{
    try
    {
        if (argc == 5 && !std::strcmp(argv[1], "--apply"))  // make_map --apply MAP SOURCE.rgba PREFIX
        {
            Preview(argv[3], argv[4], LoadMap(argv[2]));
            return 0;
        }
        if (argc < 5) throw std::runtime_error("usage: make_map GAMECUBE.msh CHARGED.msh OUT.map --target HASH:W:H...");
        Mapper mapper(LoadMesh(argv[1]), LoadMesh(argv[2]));
        std::vector<Target> targets;
        for (int i = 4; i < argc; ++i)
        {
            if (!std::strcmp(argv[i], "--target") && i + 1 < argc)
            {
                Target t{};
                if (std::sscanf(argv[++i], "%x:%d:%d", &t.texture, &t.width, &t.height) != 3 || t.width < 1 || t.height < 1)
                    throw std::runtime_error("invalid --target");
                t.texels = mapper.Bake(t.texture, t.width, t.height);
                targets.push_back(std::move(t));
            }
            else throw std::runtime_error(std::string("unknown argument ") + argv[i]);
        }
        auto bytes = Serialize(0x8C2BD57Cu, targets);  // "superteamgoalie/superteamgoalie"
        FILE* f = std::fopen(argv[3], "wb");
        if (!f || std::fwrite(bytes.data(), 1, bytes.size(), f) != bytes.size()) throw std::runtime_error("cannot write the map");
        std::fclose(f);
        std::printf("wrote %s (%zu bytes)\n", argv[3], bytes.size());
    }
    catch (const std::exception& error)
    {
        std::fprintf(stderr, "make_map: %s\n", error.what());
        return 1;
    }
    return 0;
}
