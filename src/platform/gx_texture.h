#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <new>
#include <span>
#include <vector>

#include "platform/host_metadata.h"

// GX textures as the game's texture bundles (.rlt) store them: a 32-byte
// big-endian header and the levels. Shared by the textures the tweaks make in
// the original game module (away kits, robot goalies).
namespace mscharged::platform
{
// The texture `hash` of a texture bundle (.rlt), or empty.
std::span<const std::uint8_t> FindBundleTexture(std::span<const std::uint8_t> bundle, std::uint32_t hash);

namespace gx
{
// These files are compiled into the original game module, whose ordinary
// operators allocate from the game heap: keep scratch images in the host.
template<class T> struct HostAllocator
{
    using value_type = T;
    HostAllocator() noexcept = default;
    template<class U> HostAllocator(const HostAllocator<U>&) noexcept {}
    T* allocate(std::size_t count)
    {
        if (count > std::numeric_limits<std::size_t>::max() / sizeof(T)) throw std::bad_alloc();
        void* storage = ChargedNativeMetadataAllocate(count * sizeof(T));
        if (!storage) throw std::bad_alloc();
        return static_cast<T*>(storage);
    }
    void deallocate(T* pointer, std::size_t) noexcept { ChargedNativeMetadataRelease(pointer); }
    template<class U> bool operator==(const HostAllocator<U>&) const noexcept { return true; }
};
template<class T> using HostVector = std::vector<T, HostAllocator<T>>;

std::uint32_t BE32(const std::uint8_t* p);
std::uint16_t BE16(const std::uint8_t* p);
void PutBE16(std::uint8_t* p, std::uint16_t v);
void PutBE32(std::uint8_t* p, std::uint32_t v);

// The formats the game's team-coloured textures use (the game's numbering).
enum : std::uint32_t
{
    kRGB5A3 = 1,
    kCMPR = 2,
    kRGBA8 = 3,
};

struct RGB
{
    float r, g, b; // 0..1, sRGB
};

struct Texel
{
    RGB colour;
    float alpha; // 0..1
};

struct Level
{
    int width, height;
    std::size_t offset; // of the level's tiles in the texture
};

// The format and levels of a texture (its 32-byte header included).
bool Levels(std::span<const std::uint8_t> texture, std::uint32_t& format, HostVector<Level>& levels);

// CMPR: tiles of 8x8 texels, four 4x4 blocks each. Decoding gives 0..1 colours;
// encoding takes 16 texels (0..255), transparent ones need the three-colour mode.
void DecodeBlock(const std::uint8_t* block, Texel out[16]);
void EncodeBlock(const float colours[16][3], const bool opaque[16], std::uint8_t* block);
// The block holding texel (x, y).
std::size_t BlockOffset(const Level& level, int x, int y);

// RGB5A3 and RGBA8: tiles of 4x4 texels. The offset of a texel's tile and its
// index in the tile.
std::size_t TexelTile(const Level& level, int x, int y, int tileBytes, int& index);
Texel DecodeRGB5A3(std::uint16_t v);
// Keeps the texel's mode and alpha bits: only its colour changes.
std::uint16_t EncodeRGB5A3(std::uint16_t original, RGB c);

void DecodeLevel(std::span<const std::uint8_t> texture, std::uint32_t format, const Level& level,
    HostVector<Texel>& out);
} // namespace gx
} // namespace mscharged::platform
