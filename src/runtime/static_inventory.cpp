#include "runtime/static_inventory.h"
#include "runtime/materials.h"
#include "Game/GL/GLInventory.h"
#include "Game/GL/GLTextureAnim.h"
#include <cmath>
#include "NL/gl/glModel.h"
#include "NL/gl/glTexture.h"
#include "NL/gl/glTextureManager.h"
#include "NL/glx/glxTexture.h"
#include <algorithm>
#include <climits>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace mscharged
{
namespace
{
template<class T> T* Array(GLResourcePool& pool, std::size_t count, eGLMemory type)
{
    if (!count || count > std::min<std::size_t>(ULONG_MAX, resources::MaximumAssetBytes) / sizeof(T))
        throw std::length_error("Native static resource array exceeds its budget");
    auto* output = static_cast<T*>(pool.Allocate(static_cast<unsigned long>(count * sizeof(T)), type));
    for (std::size_t i = 0; i < count; ++i) new (output + i) T{};
    return output;
}

void Texture(GLResourcePool& pool, const resources::Texture& input)
{
    if (!input.width || !input.height || !input.levels || input.game_format >= GXTex_Num
        || input.palette_entries > 256 || input.palette.size() != std::size_t(input.palette_entries) * 2)
        throw std::invalid_argument("Invalid checked static texture metadata");
    auto* texture = Array<PlatTexture>(pool, 1, GLM_Header);
    texture->m_Width = input.width; texture->m_Height = input.height;
    texture->m_Levels = input.levels; texture->m_MaxLevel = input.levels;
    texture->m_Format = static_cast<eGXTextureFormat>(input.game_format);
    texture->m_nPaletteEntries = input.palette_entries;
    std::copy(input.bits.begin(), input.bits.end(), texture->m_Bits);
    texture->m_SwizzledData = Array<unsigned char>(pool, input.pixels.size(), GLM_TextureData);
    texture->m_NativeDataBytes = input.pixels.size();
    texture->m_NativePaletteBytes = input.palette.size();
    std::memcpy(texture->m_SwizzledData, input.pixels.data(), input.pixels.size());
    if (input.palette_entries)
    {
        // GX consumes original tiled pixels and big-endian palette bytes.
        texture->m_PaletteData = Array<u16>(pool, input.palette_entries, GLM_TextureData);
        std::memcpy(texture->m_PaletteData, input.palette.data(), input.palette.size());
    }
    glRegisterTexture(input.id, texture, &pool);
}

void Animation(GLResourcePool& pool, const resources::TextureAnimation& input,
               const std::vector<resources::Texture>& textures)
{
    if (input.frames.empty() || input.frames.size() > 4096 || input.mode >= GLAnimMode_Num
        || input.direction < -1 || input.direction > 1 || !std::isfinite(input.elapsed) || input.elapsed < 0)
        throw std::invalid_argument("Invalid checked texture animation metadata");
    auto* manager = glGetTextureManager();
    if (!manager->mFreeIndices->mCount) throw std::length_error("GL texture manager is full");
    auto* anim = Array<GLTextureAnim>(pool, 1, GLM_Header);
    anim->m_nFrame = 0; anim->m_uHashID = input.id;
    anim->m_nNumTextures = anim->m_NativeFrameCount = input.frames.size();
    anim->m_ePlayMode = static_cast<eGLTexAnimMode>(input.mode);
    anim->m_nPlayDir = input.direction; anim->m_bPaused = input.paused;
    anim->m_fTime = input.elapsed; anim->m_textureIndex = 0xFFFF;
    anim->m_pAnimTex = Array<GLAnimTex>(pool, input.frames.size(), GLM_Header);
    for (unsigned i = 0; i < input.frames.size(); ++i)
    {
        const auto& frame = input.frames[i];
        // Keep dependencies in this marked batch: external/animated aliases
        // cannot outlive or be rewound underneath its frame storage.
        if (std::none_of(textures.begin(), textures.end(), [&](const auto& t) { return t.id == frame.texture; })
            || !std::isfinite(frame.duration) || frame.duration < 0)
            throw std::invalid_argument("Invalid or external texture animation frame");
        auto* texture = pool.m_inventory->GetTexture(frame.texture);
        if (!texture || texture->m_TextureIndex >= manager->mCapacity)
            throw std::invalid_argument("Texture animation frame is not registered");
        anim->SetTexture(i, {texture->m_TextureIndex, frame.duration});
    }
    // Validate all fallible inputs and reserve the tree node before consuming
    // a manager slot. Mark rollback releases aliases before their frame pixels.
    pool.m_inventory->AddTextureAnim(input.id, anim);
    manager->RegisterTextureAnim(anim);
}

void Model(GLResourcePool& pool, const resources::StaticModel& input)
{
    auto* model = Array<glModel>(pool, 1, GLM_Header);
    model->id = input.id; model->numPackets = static_cast<u32>(input.packets.size());
    model->packets = Array<glModelPacket>(pool, input.packets.size(), GLM_Header);
    for (std::size_t i = 0; i < input.packets.size(); ++i)
    {
        const auto& input_packet = input.packets[i]; auto& packet = model->packets[i];
        if (input_packet.vertices.size() > UINT16_MAX || input_packet.indices.size() > UINT16_MAX || input_packet.primitive > 5)
            throw std::length_error("Native static packet exceeds GX index/primitive limits");
        packet.numVertices = static_cast<u32>(input_packet.indices.size());
        packet.numUniqueVertices = static_cast<u16>(input_packet.vertices.size());
        packet.primType = input_packet.primitive;
        const auto id = input_packet.material.program;
        const std::vector<unsigned> layout = id == 0x386ecbdd ? std::vector<unsigned>{1,3,4}
            : id == 0x112ab470 ? std::vector<unsigned>{1,2,4,4,4,4,3}
            : (id == 0x32475c7d || id == 0x32bc21e8 || id == 0x09609a35 || id == 0xf2d57ac6 || id == 0x845cad59)
                ? std::vector<unsigned>{1,2,4,4,4,3}
            : id == 0x3eccd955 ? std::vector<unsigned>{1,2,4,4,3}
            : id == 0x2169db5c ? std::vector<unsigned>{1,2,4,3}
            : id == 0xd3e572da ? std::vector<unsigned>{1,4,3} : std::vector<unsigned>{1,4};
        packet.numStreams = layout.size();
        packet.rasterState = input_packet.raster;
        packet.indexBuffer = Array<u16>(pool, input_packet.indices.size(), GLM_IndexData);
        for (std::size_t index = 0; index < input_packet.indices.size(); ++index)
        {
            if (input_packet.indices[index] >= input_packet.vertices.size()) throw std::out_of_range("Native static index exceeds vertex storage");
            packet.indexBuffer[index] = input_packet.indices[index];
        }
        packet.streams = Array<glModelStream>(pool, layout.size(), GLM_Header);
        unsigned coordinate = 0;
        for (unsigned stream = 0; stream < layout.size(); ++stream)
        {
            auto& output = packet.streams[stream]; output.id = layout[stream];
            output.index = output.id == 4 ? coordinate : 0;
            output.stride = output.id == 1 || output.id == 2 ? 12 : output.id == 3 ? 4 : 8;
            auto* bytes = Array<unsigned char>(pool, input_packet.vertices.size() * output.stride, GLM_VertexData);
            output.address = bytes;
            for (std::size_t i = 0; i < input_packet.vertices.size(); ++i)
            {
                const auto& v = input_packet.vertices[i];
                const void* data = output.id == 1 ? static_cast<const void*>(v.position.data())
                    : output.id == 2 ? v.normal.data() : output.id == 3 ? static_cast<const void*>(v.colour.data())
                    : coordinate == 0 ? v.uv.data() : coordinate == 1 ? v.uv1.data()
                    : coordinate == 2 ? v.uv2.data() : v.uv3.data();
                std::memcpy(bytes + i * output.stride, data, output.stride);
            }
            if (output.id == 4) ++coordinate;
        }
        auto* storage = Array<unsigned char>(pool, MaterialParameterSize(id), GLM_Header);
        InstallMaterial(packet, input_packet.material, storage);
    }
    pool.m_inventory->AddModel(input.id, model);
}
}

StaticInventory::StaticInventory(GLResourcePool& pool, const std::vector<resources::StaticModel>& models,
    const std::vector<resources::Texture>& textures, void (*before_release)(),
    const std::vector<resources::TextureAnimation>& animations)
    : pool_(pool), before_release_(before_release)
{
    if (!glGetTextureManager()) throw std::logic_error("Initialize graphics memory before static assets");
    // Models may reference already registered render-target textures.
    if (models.empty()) throw std::invalid_argument("Static inventory batch is empty");
    mark_ = pool_.MarkResource();
    try
    {
        for (const auto& texture : textures) Texture(pool_, texture);
        for (const auto& animation : animations) Animation(pool_, animation, textures);
        for (const auto& model : models) mscharged::Model(pool_, model);
    }
    catch (...) { Release(); throw; }
}

StaticInventory::~StaticInventory() { Release(); }
glModel* StaticInventory::Model(std::uint32_t id) const
{ return mark_ ? pool_.m_inventory->GetModel(id) : nullptr; }
void StaticInventory::Release()
{
    if (!mark_) return;
    // A renderer must drain commands that reference CPU storage before rewind.
    if (before_release_) before_release_();
    pool_.ReleaseResource(mark_); mark_ = 0;
}
}
