#include "runtime/static_inventory.h"
#include "runtime/startup.h"
#include "NL/MemAlloc.h"
#include "NL/nlMemory.h"
#include "NL/gl/glMemoryInit.h"
#include "NL/gl/glTextureManager.h"
#include "NL/glx/glxTexture.h"
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace
{
void Require(bool condition, const char* message)
{ if (!condition) throw std::runtime_error(message); }
template<class Error, class Action> void Reject(Action action)
{
    try { action(); } catch (const Error&) { return; }
    throw std::runtime_error("Invalid static resource installation was accepted");
}
unsigned drained = 0;
void Drain() { ++drained; }
}
int main()
{
    try
    {
        std::vector<std::uint64_t> standard(1024 * 1024), virtual_arena(1024 * 1024);
        mscharged::ResetStartupMemory();
        StandardAllocator.Initialize(standard.data(), standard.size() * 8);
        VirtualAllocator.Initialize(virtual_arena.data(), virtual_arena.size() * 8);
        gMemoryInitialized = 1;
        const GLMemoryRequirement requirements[] = {{GLM_Header, 4096}, {GLM_VertexData, 4096}, {GLM_TextureData, 4096}};
        const GLMemoryConfig config{32, 32, requirements, 3, 4};
        glInitMemory(&config);
        auto& pool = *glGetCurrentResourcePool();
        const auto free = pool.GetFreeMemory();
        const auto standard_free = StandardAllocator.TotalFreeMemory(), virtual_free = VirtualAllocator.TotalFreeMemory();
        mscharged::resources::StaticModel model{1, {}};
        mscharged::resources::Packet packet;
        packet.primitive = 0; packet.texture = 20;
        packet.vertices = {{{1,2,3}, {0,0}}, {{4,5,6}, {1,0}}, {{7,8,9}, {0,1}}};
        packet.indices = {2,1,0}; model.packets.push_back(packet);
        mscharged::resources::Texture texture;
        texture.id = 20; texture.width = texture.height = 4; texture.levels = 1;
        texture.game_format = 3; texture.gx_format = 6; texture.bits = {8,8,8,8};
        texture.pixels.resize(64, 0xa5);
        {
            mscharged::StaticInventory inventory(pool, {model}, {texture}, Drain);
            auto* native = inventory.Model(1);
            Require(native && native->numPackets == 1 && native->packets[0].indexBuffer[0] == 2, "Native model/index installation failed");
            auto* stream = glFindModelStream(&native->packets[0], 1);
            Require(stream && stream->stride == 12 && static_cast<float*>(stream->address)[4] == 5, "Native position stream failed");
            auto* binding = static_cast<glTextureBinding*>(native->packets[0].materialParameters);
            auto* native_texture = glGetTextureManager()->GetTexture(binding);
            Require(native_texture && native_texture->m_Format == GXTex_RGBA8 && native_texture->m_Bits[0] == 8
                && native_texture->m_SwizzledData != texture.pixels.data()
                && std::memcmp(native_texture->m_SwizzledData, texture.pixels.data(), 64) == 0, "Native tiled texture ownership failed");
            Require(reinterpret_cast<std::uintptr_t>(stream->address) > UINT32_MAX
                && reinterpret_cast<std::uintptr_t>(native_texture->m_SwizzledData) % 32 == 0,
                "Native static resource pointers were truncated or misaligned");
            inventory.Release(); inventory.Release();
            Require(!inventory.Model(1) && drained == 1, "Static release was not repeatable or drained twice");
        }
        auto recovered = [&] {
            Require(pool.GetFreeMemory() == free && StandardAllocator.TotalFreeMemory() == standard_free
                && VirtualAllocator.TotalFreeMemory() == virtual_free
                && glGetTextureManager()->mFreeIndices->mCount == 4, "Static resource rollback leaked pool/nodes/indices");
        };
        recovered();
        Reject<std::invalid_argument>([&] { mscharged::StaticInventory inventory(pool, {model}, {texture, texture}); });
        recovered();
        auto bad = model; bad.packets[0].texture = 99;
        Reject<std::runtime_error>([&] { mscharged::StaticInventory inventory(pool, {bad}, {texture}); });
        recovered();
        bad = model; bad.packets[0].indices[0] = 3;
        Reject<std::out_of_range>([&] { mscharged::StaticInventory inventory(pool, {bad}, {texture}); });
        recovered();
        bad = model; bad.packets[0].vertices.resize(2000);
        Reject<std::bad_alloc>([&] { mscharged::StaticInventory inventory(pool, {bad}, {texture}); });
        recovered();
        texture.id = 21; texture.game_format = 8; texture.gx_format = 9;
        texture.palette_entries = 4; texture.palette = {0x80,0x12,0x90,0x34,0xa0,0x56,0xb0,0x78};
        model.packets[0].texture = 21;
        {
            mscharged::StaticInventory inventory(pool, {model}, {texture});
            auto* native_texture = glx_GetTex(21);
            Require(native_texture->m_nPaletteEntries == 4
                && std::memcmp(native_texture->m_PaletteData, texture.palette.data(), 8) == 0, "Big-endian palette bytes changed");
        }
        recovered(); glShutdownMemory(); mscharged::ResetStartupMemory();
        std::cout << "Static inventory: pool-owned native records, original lookup, GPU drain, tiled/palette byte retention and failure rollback passed\n";
        return 0;
    }
    catch (const std::exception& error) { std::cerr << "FAILED: " << error.what() << '\n'; return 1; }
}
