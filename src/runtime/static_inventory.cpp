#include "runtime/static_inventory.h"
#include "Game/GL/GLInventory.h"
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
    texture->m_Levels = input.levels; texture->m_MaxLevel = input.levels - 1;
    texture->m_Format = static_cast<eGXTextureFormat>(input.game_format);
    texture->m_nPaletteEntries = input.palette_entries;
    std::copy(input.bits.begin(), input.bits.end(), texture->m_Bits);
    texture->m_SwizzledData = Array<unsigned char>(pool, input.pixels.size(), GLM_TextureData);
    std::memcpy(texture->m_SwizzledData, input.pixels.data(), input.pixels.size());
    if (input.palette_entries)
    {
        // GX consumes original tiled pixels and big-endian palette bytes.
        texture->m_PaletteData = Array<u16>(pool, input.palette_entries, GLM_TextureData);
        std::memcpy(texture->m_PaletteData, input.palette.data(), input.palette.size());
    }
    glRegisterTexture(input.id, texture, &pool);
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
        glTextureBinding binding(input_packet.texture, input_packet.texture_flags & 1, (input_packet.texture_flags >> 1) & 1);
        if (!glGetTextureManager()->GetTexture(&binding))
            throw std::runtime_error("RLG diffuse texture is missing from the selected RLT bundle");
        packet.numVertices = static_cast<u32>(input_packet.indices.size());
        packet.numUniqueVertices = static_cast<u16>(input_packet.vertices.size());
        packet.primType = input_packet.primitive; packet.numStreams = 2;
        packet.indexBuffer = Array<u16>(pool, input_packet.indices.size(), GLM_IndexData);
        for (std::size_t index = 0; index < input_packet.indices.size(); ++index)
        {
            if (input_packet.indices[index] >= input_packet.vertices.size()) throw std::out_of_range("Native static index exceeds vertex storage");
            packet.indexBuffer[index] = input_packet.indices[index];
        }
        packet.streams = Array<glModelStream>(pool, 2, GLM_Header);
        auto* positions = Array<float>(pool, input_packet.vertices.size() * 3, GLM_VertexData);
        auto* coordinates = Array<float>(pool, input_packet.vertices.size() * 2, GLM_VertexData);
        for (std::size_t vertex = 0; vertex < input_packet.vertices.size(); ++vertex)
        {
            std::copy(input_packet.vertices[vertex].position.begin(), input_packet.vertices[vertex].position.end(), positions + vertex * 3);
            std::copy(input_packet.vertices[vertex].uv.begin(), input_packet.vertices[vertex].uv.end(), coordinates + vertex * 2);
        }
        packet.streams[0].address = positions; packet.streams[0].id = 1; packet.streams[0].stride = 12;
        packet.streams[1].address = coordinates; packet.streams[1].id = 4; packet.streams[1].stride = 8;
        auto* parameter = Array<glTextureBinding>(pool, 1, GLM_Header);
        *parameter = binding; packet.materialParameters = parameter;
        // Original material programs, render matrices and display lists are not
        // reconstructed by this diffuse-only static profile. They remain null.
    }
    pool.m_inventory->AddModel(input.id, model);
}
}

StaticInventory::StaticInventory(GLResourcePool& pool, const std::vector<resources::StaticModel>& models,
    const std::vector<resources::Texture>& textures, void (*before_release)())
    : pool_(pool), before_release_(before_release)
{
    if (!glGetTextureManager()) throw std::logic_error("Initialize graphics memory before static assets");
    if (models.empty() || textures.empty()) throw std::invalid_argument("Static inventory batch is empty");
    mark_ = pool_.MarkResource();
    try
    {
        for (const auto& texture : textures) Texture(pool_, texture);
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
