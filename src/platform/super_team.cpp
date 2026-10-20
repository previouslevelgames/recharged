#include "platform/super_team.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <functional>
#include <unordered_map>

// Formats, from the two decompilations:
// - GameCube model (.glg): glxLoadModel.cpp and GLSkinMesh.h of Super Mario
//   Strikers. Models 16 bytes, packets 0x4A bytes (texture state, material
//   state, program, raster, matrix, six textures), streams 6 bytes, one u16
//   index per vertex for all streams; skin: bind matrices (0x1B00A), one bone
//   map per packet (0x1B00B, after the software one) and per vertex the
//   packet's matrix register (0x1B010).
// - Charged model (.rlg): glLoadModel.cpp, glxLoadModel.cpp, glModel.h,
//   glxDisplayList.cpp and GXCharacterDamageMaterialProgram of Mario Strikers
//   Charged. Models 12 bytes, packets 48 bytes, streams 8 bytes; the fielder
//   material reads position, normal and six texture coordinates in that order,
//   then the bone indices (7) and weights (5).
// - Skeletons (.shier): ids (0x18003), parents (0x18009) and each bone's
//   offset in its parent's bind frame (0x18010), the same on both consoles.
namespace mscharged::platform
{
namespace
{
using namespace gx;
using Bytes = std::span<const std::uint8_t>;

struct Invalid
{
};

void Need(bool ok)
{
    if (!ok) throw Invalid{};
}

struct Vec3
{
    float x, y, z;
};

Vec3 operator+(Vec3 a, Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
Vec3 operator-(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
Vec3 operator*(Vec3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
float Dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
float Length(Vec3 a) { return std::sqrt(Dot(a, a)); }
Vec3 Normalized(Vec3 a)
{
    float l = Length(a);
    return l > 1e-12f ? a * (1.0f / l) : Vec3{0, 0, 1};
}

float BEFloat(const std::uint8_t* p)
{
    std::uint32_t bits = BE32(p);
    float v;
    std::memcpy(&v, &bits, 4);
    return v;
}

void PutBEFloat(std::uint8_t* p, float v)
{
    std::uint32_t bits;
    std::memcpy(&bits, &v, 4);
    PutBE32(p, bits);
}

// A bind matrix as the models store it: rows x axis, y axis, z axis, origin.
struct Matrix
{
    std::array<float, 16> m;
    Vec3 Row(int r) const { return {m[4 * r], m[4 * r + 1], m[4 * r + 2]}; }
    Vec3 Origin() const { return Row(3); }
    Vec3 Local(Vec3 p) const
    {
        Vec3 d = p - Origin();
        return {Dot(d, Row(0)), Dot(d, Row(1)), Dot(d, Row(2))};
    }
    Vec3 World(Vec3 l) const { return Origin() + Row(0) * l.x + Row(1) * l.y + Row(2) * l.z; }
    Vec3 Direction(Vec3 l) const { return Row(0) * l.x + Row(1) * l.y + Row(2) * l.z; }
};

struct Bind
{
    std::uint32_t bone;
    Matrix matrix;
};

struct Binds
{
    HostVector<Bind> list;
    const Matrix* Find(std::uint32_t bone) const
    {
        for (const Bind& b : list)
            if (b.bone == bone) return &b.matrix;
        return nullptr;
    }
    bool Has(std::uint32_t bone) const { return Find(bone) != nullptr; }
};

Binds ReadBinds(Bytes d, std::size_t start, std::size_t size)
{
    Binds binds;
    for (std::size_t i = 0; i + 68 <= size; i += 68)
    {
        Bind b{BE32(&d[start + i]), {}};
        for (int k = 0; k < 16; ++k) b.matrix.m[k] = BEFloat(&d[start + i + 4 + 4 * std::size_t(k)]);
        for (float v : b.matrix.m) Need(std::isfinite(v));
        if (!binds.Has(b.bone)) binds.list.push_back(b);
    }
    return binds;
}

struct Chunk
{
    std::uint32_t id;
    std::size_t start, size;
};

// GameCube nlChunk (also the skeletons of both games): alignment in bits 24-30,
// the next chunk right after the size.
HostVector<Chunk> GameCubeChunks(Bytes d, std::size_t offset, std::size_t end)
{
    Need(end <= d.size());
    HostVector<Chunk> out;
    while (offset + 8 <= end)
    {
        std::uint32_t raw = BE32(&d[offset]), size = BE32(&d[offset + 4]);
        std::size_t header = offset + 8, start = header;
        if (std::uint32_t bits = (raw & 0x7F000000u) >> 24)
        {
            Need(bits < 16);
            std::size_t align = std::size_t(1) << bits;
            start = (start + align - 1) & ~(align - 1);
        }
        Need(size <= end - header && start - header <= size);
        out.push_back({raw & 0x80FFFFFFu, start, size - (start - header)});
        offset = header + size;
    }
    return out;
}

// Charged nlChunk: alignment in bits 24-27, the next chunk 4-aligned.
HostVector<Chunk> ChargedChunks(Bytes d, std::size_t offset, std::size_t end)
{
    Need(end <= d.size());
    HostVector<Chunk> out;
    while (offset + 8 <= end)
    {
        std::uint32_t raw = BE32(&d[offset]), size = BE32(&d[offset + 4]);
        std::size_t header = offset + 8, start = header;
        if (std::uint32_t bits = (raw & 0x0F000000u) >> 24)
        {
            std::size_t align = std::size_t(1) << bits;
            start = (start + align - 1) & ~(align - 1);
        }
        Need(size <= end - header && start - header <= size);
        out.push_back({raw & 0x80FFFFFFu, start, size - (start - header)});
        offset = header + size;
        offset += (4 - offset % 4) % 4;
    }
    return out;
}

const Chunk* FindChunk(const HostVector<Chunk>& chunks, std::uint32_t id)
{
    for (const Chunk& c : chunks)
        if (c.id == id) return &c;
    return nullptr;
}

const Chunk& NeedChunk(const HostVector<Chunk>& chunks, std::uint32_t id)
{
    const Chunk* c = FindChunk(chunks, id);
    Need(c != nullptr);
    return *c;
}

struct Skeleton
{
    HostVector<std::uint32_t> ids;
    HostVector<std::int32_t> parents;
    HostVector<Vec3> offsets;
    std::uint32_t Parent(std::uint32_t bone) const
    {
        for (std::size_t i = 0; i < ids.size(); ++i)
            if (ids[i] == bone) return parents[i] >= 0 ? ids[std::size_t(parents[i])] : 0;
        return 0;
    }
};

Skeleton ReadSkeleton(Bytes d)
{
    Need(d.size() >= 8);
    HostVector<Chunk> chunks = GameCubeChunks(d, 8, d.size());
    auto find = [&](std::uint32_t id) -> const Chunk& {
        for (const Chunk& c : chunks)
            if ((c.id & 0x00FFFFFFu) == id) return c;
        throw Invalid{};
    };
    const Chunk &ids = find(0x18003), &parents = find(0x18009), &offsets = find(0x18010);
    std::size_t n = ids.size / 4;
    Need(n > 0 && n <= 256 && parents.size / 4 == n && offsets.size >= n * 12);
    Skeleton s;
    for (std::size_t i = 0; i < n; ++i)
    {
        s.ids.push_back(BE32(&d[ids.start + 4 * i]));
        std::int32_t p = std::int32_t(BE32(&d[parents.start + 4 * i]));
        Need(p < std::int32_t(n));
        s.parents.push_back(p);
        const std::uint8_t* t = &d[offsets.start + 12 * i];
        s.offsets.push_back({BEFloat(t), BEFloat(t + 4), BEFloat(t + 8)});
    }
    return s;
}

// The GameCube triangles of a primitive (0 triangles, 1 strip, 2 fan, 3 quads), as
// positions in the packet's index list, front faces kept.
template<class F> void Triangles(int primitive, std::size_t count, F&& emit)
{
    switch (primitive)
    {
    case 0:
        for (std::size_t i = 0; i + 2 < count; i += 3) emit(i, i + 1, i + 2);
        break;
    case 1:
        for (std::size_t i = 0; i + 2 < count; ++i)
            if (i % 2 == 0) emit(i, i + 1, i + 2);
            else emit(i + 1, i, i + 2);
        break;
    case 2:
        for (std::size_t i = 1; i + 1 < count; ++i) emit(0, i, i + 1);
        break;
    case 3:
        for (std::size_t i = 0; i + 3 < count; i += 4)
        {
            emit(i, i + 1, i + 2);
            emit(i, i + 2, i + 3);
        }
        break;
    default:
        throw Invalid{};
    }
}

// One corner of a robot triangle in the robot's bind pose.
struct Corner
{
    Vec3 position, normal;
    float uv[2], glow[2];
    std::uint32_t bone;
    std::uint32_t vertex; // the GameCube vertex index
};

struct Robot
{
    HostVector<Corner> corners; // three per triangle
    Binds binds;
};

Robot ReadRobot(Bytes d)
{
    HostVector<Chunk> root = GameCubeChunks(d, 0, d.size());
    Need(!root.empty() && root[0].id == 0x8001B000u);
    HostVector<Chunk> chunks = GameCubeChunks(d, root[0].start, root[0].start + root[0].size);
    const Chunk &models = NeedChunk(chunks, 0x1B003), &packets = NeedChunk(chunks, 0x1B004),
                &streams = NeedChunk(chunks, 0x1B005), &vertices = NeedChunk(chunks, 0x1B006),
                &indices = NeedChunk(chunks, 0x1B007), &skinChunk = NeedChunk(chunks, 0x8001B008u);
    HostVector<Chunk> skin = GameCubeChunks(d, skinChunk.start, skinChunk.start + skinChunk.size);

    Robot robot;
    const Chunk& binds = NeedChunk(skin, 0x1B00A);
    robot.binds = ReadBinds(d, binds.start, binds.size);
    HostVector<const Chunk*> maps, stitch;
    for (const Chunk& c : skin)
        if (c.id == 0x1B00B) maps.push_back(&c);
    Need(!maps.empty());
    maps.erase(maps.begin()); // the first map is the software one

    std::size_t total = 0;
    Need(models.size >= 16 && models.size % 16 == 0);
    for (std::size_t m = 0; m < models.size; m += 16) total += BE32(&d[models.start + m]);
    Need(total > 0 && packets.size % total == 0);
    std::size_t packetSize = packets.size / total;
    Need(packetSize == 0x4A || packetSize == 0x42);
    Need(maps.size() >= total);
    stitch.assign(total, nullptr);
    for (const Chunk& c : skin)
        if (c.id == 0x1B010)
        {
            Need(c.size >= 8);
            std::uint32_t index = BE32(&d[c.start + 4]);
            Need(index < total);
            stitch[index] = &c;
        }

    auto vertexBytes = [&](std::uint32_t address, std::size_t stride, std::size_t index, std::size_t bytes) {
        std::size_t offset = std::size_t(address) + index * stride;
        Need(offset <= vertices.size && bytes <= vertices.size - offset);
        return &d[vertices.start + offset];
    };

    std::size_t packetIndex = 0;
    for (std::size_t m = 0; m < models.size; m += 16)
    {
        std::uint32_t count = BE32(&d[models.start + m]), first = BE32(&d[models.start + m + 12]);
        for (std::uint32_t p = 0; p < count; ++p, ++packetIndex)
        {
            std::size_t o = std::size_t(first) + p * packetSize;
            Need(o <= packets.size && packetSize <= packets.size - o);
            const std::uint8_t* pk = &d[packets.start + o];
            std::uint32_t indexOffset = BE32(pk + 4);
            std::size_t nverts = BE16(pk + 8);
            int primitive = pk[10];
            std::size_t nstreams = pk[11];
            std::uint32_t streamOffset = BE32(pk + 12);
            struct Stream
            {
                std::uint32_t address = 0;
                std::size_t stride = 0;
                bool present = false;
            } layout[16];
            for (std::size_t s = 0; s < nstreams; ++s)
            {
                std::size_t so = std::size_t(streamOffset) + s * 6;
                Need(so + 6 <= streams.size);
                const std::uint8_t* st = &d[streams.start + so];
                std::uint8_t id = st[4];
                if (id < 16) layout[id] = {BE32(st), st[5], true};
            }
            Need(layout[0].present && layout[1].present && layout[3].present);
            Need(layout[0].stride == 6 || layout[0].stride == 12);
            Need(layout[1].stride == 3 || layout[1].stride == 12);
            Need(std::size_t(indexOffset) + nverts * 2 <= indices.size);
            Need(stitch[packetIndex] && stitch[packetIndex]->size >= 8 + nverts);
            const Chunk& map = *maps[packetIndex];
            auto boneOf = [&](std::size_t e) {
                std::uint8_t reg = d[stitch[packetIndex]->start + 8 + e];
                for (std::size_t j = 0; j + 8 <= map.size; j += 8)
                    if (BE32(&d[map.start + j + 4]) == reg) return BE32(&d[map.start + j]);
                throw Invalid{};
            };
            auto corner = [&](std::size_t e) {
                std::size_t i = BE16(&d[indices.start + indexOffset + 2 * e]);
                Corner c{};
                if (layout[0].stride == 12)
                {
                    const std::uint8_t* q = vertexBytes(layout[0].address, 12, i, 12);
                    c.position = {BEFloat(q), BEFloat(q + 4), BEFloat(q + 8)};
                }
                else
                {
                    const std::uint8_t* q = vertexBytes(layout[0].address, 6, i, 6);
                    c.position = {std::int16_t(BE16(q)) / 256.0f, std::int16_t(BE16(q + 2)) / 256.0f,
                        std::int16_t(BE16(q + 4)) / 256.0f};
                }
                if (layout[1].stride == 12)
                {
                    const std::uint8_t* q = vertexBytes(layout[1].address, 12, i, 12);
                    c.normal = {BEFloat(q), BEFloat(q + 4), BEFloat(q + 8)};
                }
                else
                {
                    const std::uint8_t* q = vertexBytes(layout[1].address, 3, i, 3);
                    c.normal = {std::int8_t(q[0]) / 64.0f, std::int8_t(q[1]) / 64.0f, std::int8_t(q[2]) / 64.0f};
                }
                const std::uint8_t* t = vertexBytes(layout[3].address, layout[3].stride, i, 4);
                c.uv[0] = std::int16_t(BE16(t)) / 1024.0f;
                c.uv[1] = std::int16_t(BE16(t + 2)) / 1024.0f;
                if (layout[6].present)
                {
                    const std::uint8_t* g = vertexBytes(layout[6].address, layout[6].stride, i, 4);
                    c.glow[0] = std::int16_t(BE16(g)) / 1024.0f;
                    c.glow[1] = std::int16_t(BE16(g + 2)) / 1024.0f;
                }
                c.bone = boneOf(e);
                c.vertex = std::uint32_t(i);
                Need(std::isfinite(c.position.x) && std::isfinite(c.position.y) && std::isfinite(c.position.z));
                return c;
            };
            Triangles(primitive, nverts, [&](std::size_t a, std::size_t b, std::size_t c) {
                Corner ca = corner(a), cb = corner(b), cc = corner(c);
                if (ca.vertex == cb.vertex || cb.vertex == cc.vertex || ca.vertex == cc.vertex) return;
                robot.corners.push_back(ca);
                robot.corners.push_back(cb);
                robot.corners.push_back(cc);
            });
        }
    }
    Need(!robot.corners.empty());
    return robot;
}

// What the robot model takes from the Charged character's model.
struct Character
{
    std::uint32_t id = 0;
    Binds binds;
    float height = 0;
    HostVector<std::uint32_t> boundBones; // the strongest bone of each vertex, once per bone
    std::array<std::uint8_t, 104> parameters{}; // the first packet's material parameters
    std::uint32_t raster = 0;
    std::array<std::uint8_t, 8> specular{}; // the shiniest packet's reflection texture binding
    std::uint32_t fresnelRamp = 0;
    float specularAmount = 1, specularScale[2] = {1, 1};
    HostVector<std::uint32_t> morphs;
    std::uint32_t morphElementSize = 16;
};

constexpr std::uint32_t kCharacterDamage = 0x1ACE1D01u;

Character ReadCharacter(Bytes d)
{
    HostVector<Chunk> root = ChargedChunks(d, 0, d.size());
    Need(!root.empty() && root[0].id == 0x8001B000u);
    HostVector<Chunk> chunks = ChargedChunks(d, root[0].start, root[0].start + root[0].size);
    const Chunk &models = NeedChunk(chunks, 0x1B003), &packets = NeedChunk(chunks, 0x1B004),
                &streams = NeedChunk(chunks, 0x1B005), &vertices = NeedChunk(chunks, 0x1B006),
                &parameters = NeedChunk(chunks, 0x1B016), &skinChunk = NeedChunk(chunks, 0x8001B008u);
    HostVector<Chunk> skin = ChargedChunks(d, skinChunk.start, skinChunk.start + skinChunk.size);

    Character c;
    Need(models.size >= 12);
    c.id = BE32(&d[models.start]);
    std::size_t total = 0;
    for (std::size_t m = 0; m + 12 <= models.size; m += 12) total += BE32(&d[models.start + m + 4]);
    Need(total > 0 && total * 48 <= packets.size);

    HostVector<const Chunk*> maps;
    for (const Chunk& k : skin)
    {
        if (k.id == 0x1B00B) maps.push_back(&k);
        else if (k.id == 0x1B00A) c.binds = ReadBinds(d, k.start, k.size);
        else if (k.id == 0x1B00C && k.size >= 4)
        {
            std::uint32_t n = BE32(&d[k.start]);
            Need(n <= 256 && 4 + 4 * std::size_t(n) + 4 <= k.size);
            for (std::uint32_t i = 0; i < n; ++i) c.morphs.push_back(BE32(&d[k.start + 4 + 4 * std::size_t(i)]));
            c.morphElementSize = BE32(&d[k.start + 4 + 4 * std::size_t(n)]);
            Need(c.morphElementSize > 0 && c.morphElementSize <= 64);
        }
    }
    Need(!c.binds.list.empty() && maps.size() >= total);

    float low = 1e30f, high = -1e30f;
    bool material = false, shiny = false;
    for (std::size_t p = 0; p < total; ++p)
    {
        const std::uint8_t* pk = &d[packets.start + p * 48];
        std::size_t unique = BE16(pk + 8), nstreams = pk[11];
        std::uint32_t streamOffset = BE32(pk + 12), program = BE32(pk + 16), params = BE32(pk + 32);
        // Parts in other materials (eyes, shells) still give bones and size.
        const std::uint8_t* par = nullptr;
        if (program == kCharacterDamage)
        {
            Need(std::size_t(params) + 104 <= parameters.size);
            par = &d[parameters.start + params];
        }
        if (par && !material)
        {
            std::memcpy(c.parameters.data(), par, 104);
            c.raster = BE32(pk + 28);
        }
        if (par && (!material || (!shiny && BE32(par + 0x48) == 1)))
        {
            material = true;
            shiny = BE32(par + 0x48) == 1;
            std::memcpy(c.specular.data(), par + 8, 8);
            c.fresnelRamp = BE32(par + 0x48);
            c.specularAmount = BEFloat(par + 0x3C);
            c.specularScale[0] = BEFloat(par + 0x40);
            c.specularScale[1] = BEFloat(par + 0x44);
        }
        std::uint32_t pos = 0, idx = 0, wgt = 0;
        bool hasPos = false, hasIdx = false, hasWgt = false;
        for (std::size_t s = 0; s < nstreams; ++s)
        {
            std::size_t so = std::size_t(streamOffset) + s * 8;
            Need(so + 8 <= streams.size);
            const std::uint8_t* st = &d[streams.start + so];
            std::uint32_t address = BE32(st);
            std::uint8_t stride = st[5], id = st[6];
            if (id == 1 && !hasPos) { Need(stride == 12); pos = address; hasPos = true; }
            if (id == 7 && !hasIdx) { Need(stride == 4); idx = address; hasIdx = true; }
            if (id == 5 && !hasWgt) { Need(stride == 16); wgt = address; hasWgt = true; }
        }
        Need(hasPos && hasIdx && hasWgt);
        Need(std::size_t(pos) + unique * 12 <= vertices.size && std::size_t(idx) + unique * 4 <= vertices.size
            && std::size_t(wgt) + unique * 16 <= vertices.size);
        const Chunk& map = *maps[p];
        for (std::size_t i = 0; i < unique; ++i)
        {
            float z = BEFloat(&d[vertices.start + pos + i * 12 + 8]);
            if (std::isfinite(z))
            {
                low = std::min(low, z);
                high = std::max(high, z);
            }
            int best = 0;
            float bestWeight = -1;
            for (int k = 0; k < 4; ++k)
            {
                float w = BEFloat(&d[vertices.start + wgt + i * 16 + 4 * std::size_t(k)]);
                if (w > bestWeight)
                {
                    bestWeight = w;
                    best = k;
                }
            }
            std::size_t slot = d[vertices.start + idx + i * 4 + std::size_t(best)];
            if (slot * 4 + 4 > map.size) continue;
            std::uint32_t bone = BE32(&d[map.start + slot * 4]);
            if (std::find(c.boundBones.begin(), c.boundBones.end(), bone) == c.boundBones.end())
                c.boundBones.push_back(bone);
        }
    }
    Need(material && high > low);
    c.height = high - low;
    return c;
}

// Bones the character's model does not bind get a bind pose: the skeleton's
// offset in the parent's bind frame, and the robot's rotation relative to the
// parent (or the parent's own, where the robot lacks the bone).
void AddMissingBinds(Binds& binds, const Skeleton& skeleton, const Binds& robot)
{
    for (std::size_t i = 0; i < skeleton.ids.size(); ++i)
    {
        std::uint32_t bone = skeleton.ids[i];
        std::int32_t p = skeleton.parents[i];
        if (binds.Has(bone) || p < 0) continue;
        std::uint32_t parentBone = skeleton.ids[std::size_t(p)];
        const Matrix* parent = binds.Find(parentBone);
        if (!parent) continue;
        Matrix out = *parent;
        Vec3 position = parent->World(skeleton.offsets[i]);
        const Matrix *rb = robot.Find(bone), *rp = robot.Find(parentBone);
        if (rb && rp)
        {
            // rows(out) = rows(rb) * transpose(rows(rp)) * rows(parent)
            for (int r = 0; r < 3; ++r)
            {
                Vec3 relative{Dot(rb->Row(r), rp->Row(0)), Dot(rb->Row(r), rp->Row(1)), Dot(rb->Row(r), rp->Row(2))};
                Vec3 row = parent->Direction(relative);
                out.m[4 * r] = row.x;
                out.m[4 * r + 1] = row.y;
                out.m[4 * r + 2] = row.z;
            }
        }
        out.m[12] = position.x;
        out.m[13] = position.y;
        out.m[14] = position.z;
        binds.list.push_back({bone, out});
    }
}

// The robot fitted onto the character: per bone both skeletons have, the part of
// the robot between the joint and its child joint stretches to the character's
// joint length; thickness and the parts beyond the joints follow the character's
// height; the torso's width follows the character's shoulder and hip spacing.
struct Fit
{
    float along, across;
    float robotLength = -1, characterLength = -1; // the stretched span, when any
    int lateralAxis = 0;                            // 1 or 2: the local axis that points sideways
    float lateral = 1;
};

struct Fitter
{
    const Robot& robot;
    const Skeleton &robotSkeleton, &characterSkeleton;
    const Binds& characterBinds;
    HostVector<std::pair<std::uint32_t, Fit>> fits;

    std::uint32_t Shared(std::uint32_t bone, const Skeleton& skeleton) const
    {
        for (int step = 0; step < 64 && bone; ++step)
        {
            if (robot.binds.Has(bone) && characterBinds.Has(bone)) return bone;
            bone = skeleton.Parent(bone);
        }
        return 0;
    }

    const Fit* Find(std::uint32_t bone) const
    {
        for (const auto& f : fits)
            if (f.first == bone) return &f.second;
        return nullptr;
    }

    Fitter(const Robot& r, const Skeleton& rs, const Skeleton& cs, const Binds& cb, const Character& character)
        : robot(r), robotSkeleton(rs), characterSkeleton(cs), characterBinds(cb)
    {
        float low = 1e30f, high = -1e30f;
        for (const Corner& c : robot.corners)
        {
            low = std::min(low, c.position.z);
            high = std::max(high, c.position.z);
        }
        Need(high > low);
        float uniform = character.height / (high - low);

        // Child joints of each shared bone, in the character's skeleton.
        HostVector<std::pair<std::uint32_t, std::uint32_t>> children;
        for (std::size_t i = 0; i < cs.ids.size(); ++i)
        {
            if (cs.parents[i] < 0) continue;
            std::uint32_t g = Shared(cs.ids[i], cs), pg = Shared(cs.ids[std::size_t(cs.parents[i])], cs);
            if (g && pg && g != pg) children.push_back({pg, g});
        }
        HostVector<std::uint32_t> robotGroups, characterGroups;
        for (const Corner& c : robot.corners)
            if (std::uint32_t g = Shared(c.bone, rs);
                g && std::find(robotGroups.begin(), robotGroups.end(), g) == robotGroups.end())
                robotGroups.push_back(g);
        for (std::uint32_t bone : character.boundBones)
            if (std::uint32_t g = Shared(bone, cs);
                g && std::find(characterGroups.begin(), characterGroups.end(), g) == characterGroups.end())
                characterGroups.push_back(g);

        for (std::uint32_t g : robotGroups)
        {
            if (std::find(characterGroups.begin(), characterGroups.end(), g) == characterGroups.end()) continue;
            const Matrix &cg = *characterBinds.Find(g), &rg = *robot.binds.Find(g);
            float lr = 0, lc = 0;
            for (const auto& [parent, child] : children)
            {
                if (parent != g) continue;
                float dc = Dot(characterBinds.Find(child)->Origin() - cg.Origin(), cg.Row(0));
                float dr = Dot(robot.binds.Find(child)->Origin() - rg.Origin(), rg.Row(0));
                if (dc > lc)
                {
                    lc = dc;
                    lr = dr;
                }
            }
            Fit f{uniform, uniform};
            if (lc > 1e-3f && lr > 1e-3f)
            {
                f.along = lc / lr;
                f.robotLength = lr;
                f.characterLength = lc;
            }
            fits.push_back({g, f});
        }

        struct Lateral
        {
            std::uint32_t bone, left, right;
        };
        static constexpr Lateral kLateral[] = {
            {0xA90E80CBu, 0xE21B0233u, 0xFA5A68F9u}, // chest: shoulders
            {0x0E3F9CDCu, 0xE21B0233u, 0xFA5A68F9u}, // neck: shoulders
            {0xD693D55Au, 0xF62E1A3Bu, 0xC40B86C1u}, // pelvis: hips
            {0xA149C14Eu, 0xF62E1A3Bu, 0xC40B86C1u}, // spine: hips
        };
        for (const Lateral& l : kLateral)
        {
            Fit* f = nullptr;
            for (auto& entry : fits)
                if (entry.first == l.bone) f = &entry.second;
            const Matrix *cl = characterBinds.Find(l.left), *cr = characterBinds.Find(l.right),
                         *rl = robot.binds.Find(l.left), *rr = robot.binds.Find(l.right);
            if (!f || !cl || !cr || !rl || !rr) continue;
            float ratio = Length(cl->Origin() - cr->Origin()) / std::max(Length(rl->Origin() - rr->Origin()), 1e-6f);
            const Matrix& rg = *robot.binds.Find(l.bone);
            f->lateralAxis = std::fabs(rg.m[4]) >= std::fabs(rg.m[8]) ? 1 : 2;
            f->lateral = std::max(ratio, f->across);
        }

        for (const Bind& b : robot.binds.list)
            if (characterBinds.Has(b.bone) && !Find(b.bone)) fits.push_back({b.bone, Fit{uniform, uniform}});
    }

    // A robot corner on the character: its bone there, position and normal.
    bool Place(const Corner& c, std::uint32_t& bone, Vec3& position, Vec3& normal) const
    {
        bone = Shared(c.bone, robotSkeleton);
        const Fit* f = Find(bone);
        if (!f) return false;
        const Matrix &rg = *robot.binds.Find(bone), &cg = *characterBinds.Find(bone);
        Vec3 l = rg.Local(c.position);
        float along, alongScale;
        if (f->robotLength < 0)
        {
            along = l.x * f->along;
            alongScale = f->along;
        }
        else if (l.x < 0)
        {
            along = l.x * f->across;
            alongScale = f->across;
        }
        else if (l.x > f->robotLength)
        {
            along = f->characterLength + (l.x - f->robotLength) * f->across;
            alongScale = f->across;
        }
        else
        {
            along = l.x * f->along;
            alongScale = f->along;
        }
        float scale[2] = {f->across, f->across};
        if (f->lateralAxis) scale[f->lateralAxis - 1] = f->lateral;
        position = cg.World({along, l.y * scale[0], l.z * scale[1]});
        Vec3 n{Dot(c.normal, rg.Row(0)), Dot(c.normal, rg.Row(1)), Dot(c.normal, rg.Row(2))};
        n = {n.x / alongScale, n.y / scale[0], n.z / scale[1]};
        normal = Normalized(cg.Direction(Normalized(n)));
        return std::isfinite(position.x) && std::isfinite(position.y) && std::isfinite(position.z);
    }
};

// Big-endian byte buffers for the model's chunks.
struct Buffer
{
    HostVector<std::uint8_t> bytes;
    std::size_t Size() const { return bytes.size(); }
    void U8(std::uint8_t v) { bytes.push_back(v); }
    void U16(std::uint16_t v)
    {
        bytes.push_back(std::uint8_t(v >> 8));
        bytes.push_back(std::uint8_t(v));
    }
    void U32(std::uint32_t v)
    {
        U16(std::uint16_t(v >> 16));
        U16(std::uint16_t(v));
    }
    void F32(float v)
    {
        std::uint32_t bits;
        std::memcpy(&bits, &v, 4);
        U32(bits);
    }
    void Raw(const std::uint8_t* p, std::size_t n) { bytes.insert(bytes.end(), p, p + n); }
    void Pad4()
    {
        while (bytes.size() % 4) bytes.push_back(0);
    }
    void Chunk(std::uint32_t id, const Buffer& data)
    {
        U32(id);
        U32(std::uint32_t((data.Size() + 3) & ~std::size_t(3)));
        Raw(data.bytes.data(), data.Size());
        Pad4();
    }
};

std::int16_t TexCoord(float v)
{
    return std::int16_t(std::clamp(std::lround(v * 1024.0f), -32768l, 32767l));
}

// The robot's textures: its own body texture (with the glow layer added) and the
// shine mask and Mega Strike texture made from it.
constexpr std::uint32_t kSuperTeamGlow = 0xA15D579Du;
constexpr std::uint32_t kSuperTeamShine = 0x5354534Du;  // made, "STSM"
constexpr std::uint32_t kSuperTeamMega = 0x53544D47u;   // made, "STMG"
constexpr std::uint32_t kRobotModelBits = 0x53544D31u;  // "STM1"
constexpr std::size_t kMaxPacketBones = 9;              // as Charged's own models (glx_SkinMatrixSlots)
} // namespace

std::uint32_t SuperTeamModelId(std::uint32_t characterModelId) { return characterModelId ^ kRobotModelBits; }

bool SuperTeamKeepsLook(int characterClass)
{
    // eCharacterClass: BOWSER 1, PETEY 11, BOO 16.
    return characterClass == 1 || characterClass == 11 || characterClass == 16;
}

HostVector<std::uint8_t> MakeSuperTeamModel(const SuperTeamSource& source, std::span<const std::uint8_t> characterModel,
    std::span<const std::uint8_t> characterSkeleton)
{
    try
    {
        Robot robot = ReadRobot(source.model);
        Skeleton robotSkeleton = ReadSkeleton(source.skeleton);
        Skeleton skeleton = ReadSkeleton(characterSkeleton);
        Character character = ReadCharacter(characterModel);
        Binds binds = character.binds;
        AddMissingBinds(binds, skeleton, robot.binds);
        Fitter fitter(robot, robotSkeleton, skeleton, binds, character);

        struct OutVertex
        {
            Vec3 position, normal;
            float uv[2], damage[2];
            std::uint8_t slot;
        };
        struct OutPacket
        {
            HostVector<std::uint32_t> bones;
            HostVector<OutVertex> vertices;
            HostVector<std::uint16_t> indices;
            std::unordered_map<std::uint64_t, std::uint16_t, std::hash<std::uint64_t>, std::equal_to<std::uint64_t>,
                HostAllocator<std::pair<const std::uint64_t, std::uint16_t>>>
                lookup;
        };
        HostVector<OutPacket> packets(1);

        auto slotOf = [](const OutPacket& p, std::uint32_t bone) -> int {
            for (std::size_t i = 0; i < p.bones.size(); ++i)
                if (p.bones[i] == bone) return int(i);
            return -1;
        };
        for (std::size_t t = 0; t + 2 < robot.corners.size(); t += 3)
        {
            std::uint32_t bones[3];
            Vec3 positions[3], normals[3];
            bool ok = true;
            for (int k = 0; k < 3; ++k) ok = ok && fitter.Place(robot.corners[t + k], bones[k], positions[k], normals[k]);
            if (!ok) continue;
            OutPacket* p = &packets.back();
            std::size_t added = 0;
            for (int k = 0; k < 3; ++k)
                if (slotOf(*p, bones[k]) < 0 && std::find(bones, bones + k, bones[k]) == bones + k) ++added;
            if (p->bones.size() + added > kMaxPacketBones || p->vertices.size() + 3 > 0xFFF0)
            {
                packets.emplace_back();
                p = &packets.back();
            }
            for (int k = 0; k < 3; ++k)
            {
                const Corner& c = robot.corners[t + k];
                int slot = slotOf(*p, bones[k]);
                if (slot < 0)
                {
                    slot = int(p->bones.size());
                    p->bones.push_back(bones[k]);
                }
                std::uint64_t key = (std::uint64_t(c.vertex) << 8) | std::uint64_t(slot);
                auto found = p->lookup.find(key);
                if (found == p->lookup.end())
                {
                    OutVertex v{positions[k], normals[k], {c.uv[0], c.uv[1]}, {}, std::uint8_t(slot)};
                    // Damage layers (mud, ice) by a cylinder around the body: continuous splats.
                    float angle = std::atan2(positions[k].y, positions[k].x);
                    v.damage[0] = (angle / 6.2831853f + 0.5f) * 2.0f;
                    v.damage[1] = positions[k].z / character.height * 2.0f;
                    found = p->lookup.emplace(key, std::uint16_t(p->vertices.size())).first;
                    p->vertices.push_back(v);
                }
                p->indices.push_back(found->second);
            }
        }
        while (!packets.empty() && packets.back().indices.empty()) packets.pop_back();
        Need(!packets.empty());

        // Material parameters: the character's own, with the robot's textures and
        // the character's shiniest reflection.
        auto parametersOf = [&]() {
            std::array<std::uint8_t, 104> p = character.parameters;
            PutBE32(&p[0x00], kSuperTeamBody);
            std::memcpy(&p[0x08], character.specular.data(), 8);
            PutBE32(&p[0x10], kSuperTeamShine);
            PutBE32(&p[0x18], kSuperTeamMega);
            PutBEFloat(&p[0x3C], character.specularAmount);
            PutBEFloat(&p[0x40], character.specularScale[0]);
            PutBEFloat(&p[0x44], character.specularScale[1]);
            PutBE32(&p[0x48], character.fresnelRamp);
            return p;
        };

        Buffer params, indices, vertices, streams, packetRecords, matrices, models, skin;
        for (std::size_t p = 0; p < packets.size(); ++p)
        {
            const OutPacket& op = packets[p];
            std::uint32_t paramOffset = std::uint32_t(params.Size());
            auto par = parametersOf();
            params.Raw(par.data(), par.size());

            std::uint32_t indexOffset = std::uint32_t(indices.Size());
            for (std::uint16_t i : op.indices) indices.U16(i);
            indices.Pad4();

            std::uint32_t streamOffset = std::uint32_t(streams.Size());
            auto stream = [&](std::uint8_t id, std::uint8_t stride, auto&& write) {
                vertices.Pad4();
                streams.U32(std::uint32_t(vertices.Size()));
                streams.U8(0);
                streams.U8(stride);
                streams.U8(id);
                streams.U8(0);
                for (const OutVertex& v : op.vertices) write(v);
            };
            stream(1, 12, [&](const OutVertex& v) {
                vertices.F32(v.position.x);
                vertices.F32(v.position.y);
                vertices.F32(v.position.z);
            });
            stream(2, 12, [&](const OutVertex& v) {
                vertices.F32(v.normal.x);
                vertices.F32(v.normal.y);
                vertices.F32(v.normal.z);
            });
            for (int set = 0; set < 6; ++set)
                stream(4, 4, [&](const OutVertex& v) {
                    const float* uv = set < 4 ? v.uv : v.damage;
                    vertices.U16(std::uint16_t(TexCoord(uv[0])));
                    vertices.U16(std::uint16_t(TexCoord(uv[1])));
                });
            stream(7, 4, [&](const OutVertex& v) { vertices.U32(std::uint32_t(v.slot) << 24); });
            stream(5, 16, [&](const OutVertex&) {
                vertices.F32(1.0f);
                vertices.F32(0.0f);
                vertices.F32(0.0f);
                vertices.F32(0.0f);
            });

            packetRecords.U32(indexOffset);
            packetRecords.U32(std::uint32_t(op.indices.size()));
            packetRecords.U16(std::uint16_t(op.vertices.size()));
            packetRecords.U8(0);  // triangles
            packetRecords.U8(10); // streams
            packetRecords.U32(streamOffset);
            packetRecords.U32(kCharacterDamage);
            packetRecords.U32(0);
            packetRecords.U32(0); // matrix 0
            packetRecords.U32(character.raster);
            packetRecords.U32(paramOffset);
            packetRecords.U32(0);
            packetRecords.U32(0);
            packetRecords.U32(0);

            Buffer map;
            for (std::uint32_t bone : op.bones) map.U32(bone);
            skin.Chunk(0x1B00B, map);
        }
        Buffer bindChunk;
        for (const Bind& b : binds.list)
        {
            bindChunk.U32(b.bone);
            for (float v : b.matrix.m) bindChunk.F32(v);
        }
        skin.Chunk(0x1B00A, bindChunk);
        // The character's morph slots (facial expressions) without deltas: the
        // robot has no face to move.
        Buffer morph;
        morph.U32(std::uint32_t(character.morphs.size()));
        for (std::uint32_t id : character.morphs) morph.U32(id);
        morph.U32(character.morphElementSize);
        morph.U32(std::uint32_t(packets.size()));
        for (std::size_t i = 0; i < packets.size() * character.morphs.size(); ++i) morph.U32(0);
        skin.Chunk(0x1B00C, morph);

        for (int r = 0; r < 4; ++r)
            for (int c = 0; c < 4; ++c) matrices.F32(r == c ? 1.0f : 0.0f);
        models.U32(SuperTeamModelId(character.id));
        models.U32(std::uint32_t(packets.size()));
        models.U32(0);

        Buffer group;
        group.Chunk(0x1B016, params);
        group.Chunk(0x1B007, indices);
        group.Chunk(0x1B006, vertices);
        group.Chunk(0x1B005, streams);
        group.Chunk(0x1B004, packetRecords);
        group.Chunk(0x1B002, matrices);
        group.Chunk(0x1B003, models);
        group.Chunk(0x8001B008u, skin);
        Buffer file;
        file.Chunk(0x8001B000u, group);
        return std::move(file.bytes);
    }
    catch (const Invalid&)
    {
        return {};
    }
}

namespace
{
// A GameCube texture bundle (.glt): the directory after a 32-byte header.
std::span<const std::uint8_t> FindGameCubeTexture(Bytes bundle, std::uint32_t hash)
{
    if (bundle.size() < 32) return {};
    std::uint32_t count = BE32(&bundle[4]);
    if (count > (bundle.size() - 32) / 16) return {};
    std::size_t base = 32 + std::size_t(count) * 16;
    for (std::uint32_t i = 0; i < count; ++i)
    {
        const std::uint8_t* entry = &bundle[32 + std::size_t(i) * 16];
        if (BE32(entry) != hash) continue;
        std::size_t offset = BE32(entry + 4), size = BE32(entry + 8);
        if (base + offset > bundle.size() || size > bundle.size() - base - offset) return {};
        return bundle.subspan(base + offset, size);
    }
    return {};
}

struct Image
{
    int width = 0, height = 0;
    HostVector<float> rgb; // 0..255
    float* At(int x, int y) { return &rgb[(std::size_t(y) * width + x) * 3]; }
};

Image Decode(Bytes texture)
{
    std::uint32_t format;
    HostVector<Level> levels;
    Need(Levels(texture, format, levels));
    HostVector<Texel> texels;
    DecodeLevel(texture, format, levels[0], texels);
    Image out;
    out.width = levels[0].width;
    out.height = levels[0].height;
    out.rgb.resize(texels.size() * 3);
    for (std::size_t i = 0; i < texels.size(); ++i)
    {
        out.rgb[i * 3] = texels[i].colour.r * 255.0f;
        out.rgb[i * 3 + 1] = texels[i].colour.g * 255.0f;
        out.rgb[i * 3 + 2] = texels[i].colour.b * 255.0f;
    }
    return out;
}

Image Half(const Image& in)
{
    Image out;
    out.width = std::max(in.width / 2, 1);
    out.height = std::max(in.height / 2, 1);
    out.rgb.assign(std::size_t(out.width) * out.height * 3, 0.0f);
    for (int y = 0; y < out.height; ++y)
        for (int x = 0; x < out.width; ++x)
            for (int c = 0; c < 3; ++c)
            {
                float sum = 0;
                for (int dy = 0; dy < 2; ++dy)
                    for (int dx = 0; dx < 2; ++dx)
                    {
                        int sx = std::min(x * 2 + dx, in.width - 1), sy = std::min(y * 2 + dy, in.height - 1);
                        sum += in.rgb[(std::size_t(sy) * in.width + sx) * 3 + c];
                    }
                out.rgb[(std::size_t(y) * out.width + x) * 3 + c] = sum / 4;
            }
    return out;
}

// A CMPR texture of `image` with all its levels down to 8x8 (32-byte header included).
HostVector<std::uint8_t> EncodeCMPR(Image image)
{
    int levels = 1;
    for (int w = image.width, h = image.height; levels < 16 && w > 8 && h > 8; w /= 2, h /= 2) ++levels;
    HostVector<std::uint8_t> out(32, 0);
    PutBE32(&out[0], std::uint32_t(levels));
    PutBE32(&out[4], kCMPR);
    // Wrap modes and filters as the game's own character textures have them.
    out[8] = 0x05;
    out[9] = 0x06;
    out[10] = 0x05;
    PutBE16(&out[14], std::uint16_t(image.width));
    PutBE16(&out[16], std::uint16_t(image.height));
    for (int l = 0; l < levels; ++l)
    {
        if (l) image = Half(image);
        Level level{image.width, image.height, out.size()};
        out.resize(out.size() + std::size_t((image.width + 7) / 8) * ((image.height + 7) / 8) * 32);
        for (int by = 0; by < image.height; by += 4)
            for (int bx = 0; bx < image.width; bx += 4)
            {
                float colours[16][3];
                bool opaque[16];
                for (int y = 0; y < 4; ++y)
                    for (int x = 0; x < 4; ++x)
                    {
                        int sx = std::min(bx + x, image.width - 1), sy = std::min(by + y, image.height - 1);
                        const float* c = image.At(sx, sy);
                        for (int k = 0; k < 3; ++k) colours[y * 4 + x][k] = c[k];
                        opaque[y * 4 + x] = true;
                    }
                EncodeBlock(colours, opaque, &out[BlockOffset(level, bx, by)]);
            }
    }
    return out;
}

// Adds the glow layer (its own texture coordinates) onto the body texture.
void AddGlow(Image& body, const Image& glow, const Robot& robot)
{
    Image layer;
    layer.width = body.width;
    layer.height = body.height;
    layer.rgb.assign(body.rgb.size(), 0.0f);
    auto sample = [&](float u, float v, float out[3]) {
        float x = (u - std::floor(u)) * glow.width - 0.5f, y = (v - std::floor(v)) * glow.height - 0.5f;
        int x0 = int(std::floor(x)), y0 = int(std::floor(y));
        float fx = x - x0, fy = y - y0;
        for (int k = 0; k < 3; ++k) out[k] = 0;
        for (int dy = 0; dy < 2; ++dy)
            for (int dx = 0; dx < 2; ++dx)
            {
                int sx = ((x0 + dx) % glow.width + glow.width) % glow.width;
                int sy = ((y0 + dy) % glow.height + glow.height) % glow.height;
                float w = (dx ? fx : 1 - fx) * (dy ? fy : 1 - fy);
                const float* c = &glow.rgb[(std::size_t(sy) * glow.width + sx) * 3];
                for (int k = 0; k < 3; ++k) out[k] += w * c[k];
            }
    };
    for (std::size_t t = 0; t + 2 < robot.corners.size(); t += 3)
    {
        const Corner* c[3] = {&robot.corners[t], &robot.corners[t + 1], &robot.corners[t + 2]};
        float px[3], py[3];
        for (int k = 0; k < 3; ++k)
        {
            px[k] = c[k]->uv[0] * body.width;
            py[k] = c[k]->uv[1] * body.height;
        }
        float area = (px[1] - px[0]) * (py[2] - py[0]) - (px[2] - px[0]) * (py[1] - py[0]);
        if (std::fabs(area) < 1e-6f) continue;
        int x0 = int(std::floor(std::min({px[0], px[1], px[2]}))), x1 = int(std::ceil(std::max({px[0], px[1], px[2]})));
        int y0 = int(std::floor(std::min({py[0], py[1], py[2]}))), y1 = int(std::ceil(std::max({py[0], py[1], py[2]})));
        if (x1 - x0 > 4 * body.width || y1 - y0 > 4 * body.height) continue;
        for (int y = y0; y <= y1; ++y)
            for (int x = x0; x <= x1; ++x)
            {
                float qx = x + 0.5f, qy = y + 0.5f;
                float w0 = ((px[1] - qx) * (py[2] - qy) - (px[2] - qx) * (py[1] - qy)) / area;
                float w1 = ((px[2] - qx) * (py[0] - qy) - (px[0] - qx) * (py[2] - qy)) / area;
                float w2 = 1 - w0 - w1;
                const float edge = -0.02f;
                if (w0 < edge || w1 < edge || w2 < edge) continue;
                float u = w0 * c[0]->glow[0] + w1 * c[1]->glow[0] + w2 * c[2]->glow[0];
                float v = w0 * c[0]->glow[1] + w1 * c[1]->glow[1] + w2 * c[2]->glow[1];
                float g[3];
                sample(u, v, g);
                int tx = ((x % body.width) + body.width) % body.width, ty = ((y % body.height) + body.height) % body.height;
                float* dst = layer.At(tx, ty);
                for (int k = 0; k < 3; ++k) dst[k] = std::max(dst[k], g[k]);
            }
    }
    for (std::size_t i = 0; i < body.rgb.size(); ++i) body.rgb[i] = std::min(255.0f, body.rgb[i] + layer.rgb[i]);
}

void PutTexture(HostVector<std::uint8_t>& directory, HostVector<std::uint8_t>& data, std::uint32_t hash,
    const HostVector<std::uint8_t>& texture)
{
    while (data.size() % 32) data.push_back(0);
    std::size_t offset = data.size();
    data.insert(data.end(), texture.begin(), texture.end());
    std::uint8_t entry[16] = {};
    PutBE32(entry, hash);
    PutBE32(entry + 4, std::uint32_t(offset));
    PutBE32(entry + 8, std::uint32_t(texture.size()));
    directory.insert(directory.end(), entry, entry + 16);
}
} // namespace

HostVector<std::uint8_t> MakeSuperTeamTextures(const SuperTeamSource& source)
{
    try
    {
        Robot robot = ReadRobot(source.model);
        Bytes bodyTexture = FindGameCubeTexture(source.textures, kSuperTeamBody);
        Bytes glowTexture = FindGameCubeTexture(source.textures, kSuperTeamGlow);
        Need(!bodyTexture.empty());
        Image body = Decode(bodyTexture);
        if (!glowTexture.empty()) AddGlow(body, Decode(glowTexture), robot);

        Image shine = Half(body), mega = Half(body);
        for (std::size_t k = 0; k < shine.rgb.size(); k += 3)
        {
            float luma = (0.2126f * shine.rgb[k] + 0.7152f * shine.rgb[k + 1] + 0.0722f * shine.rgb[k + 2]) / 255.0f;
            float value = std::clamp(0.3f + 1.2f * luma, 0.0f, 1.0f) * 255.0f;
            shine.rgb[k] = shine.rgb[k + 1] = shine.rgb[k + 2] = value;
            for (int c = 0; c < 3; ++c) mega.rgb[k + c] = std::min(255.0f, mega.rgb[k + c] * 1.7f + 20.0f);
        }

        HostVector<std::uint8_t> directory, data;
        PutTexture(directory, data, kSuperTeamBody, EncodeCMPR(std::move(body)));
        PutTexture(directory, data, kSuperTeamShine, EncodeCMPR(std::move(shine)));
        PutTexture(directory, data, kSuperTeamMega, EncodeCMPR(std::move(mega)));
        HostVector<std::uint8_t> out(16, 0);
        out[0] = 'P';
        out[1] = 'T';
        out[2] = 'L';
        out[3] = 'G';
        PutBE32(&out[4], 3);
        out.insert(out.end(), directory.begin(), directory.end());
        out.insert(out.end(), data.begin(), data.end());
        return out;
    }
    catch (const Invalid&)
    {
        return {};
    }
}

bool ValidSuperTeamSource(const SuperTeamSource& source)
{
    try
    {
        Robot robot = ReadRobot(source.model);
        ReadSkeleton(source.skeleton);
        return !FindGameCubeTexture(source.textures, kSuperTeamBody).empty();
    }
    catch (const Invalid&)
    {
        return false;
    }
}
} // namespace mscharged::platform
