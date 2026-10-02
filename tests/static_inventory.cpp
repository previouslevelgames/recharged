#include "runtime/static_inventory.h"
#include "runtime/materials.h"
#include "runtime/graphics_state.h"
#include "runtime/material_environment.h"
#include "Game/GameObjectLighting.h"
#include "Game/Render/LightingLookup.h"
#include "NL/gl/glMaterialProgram.h"
#include "NL/gl/glState.h"
#include "NL/glx/GXScrollingDiffuseMaterialProgram.h"
#include "NL/glx/GXMaskedSpecularFresnelMaterialProgram.h"
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
#include <limits>

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
        mscharged::InitializeOriginalGraphicsState();
        auto& pool = *glGetCurrentResourcePool();
        const auto free = pool.GetFreeMemory();
        mscharged::MaterialPrograms materials;
        const auto standard_free = StandardAllocator.TotalFreeMemory(), virtual_free = VirtualAllocator.TotalFreeMemory();
        mscharged::resources::StaticModel model{1, {}};
        mscharged::resources::Packet packet;
        packet.primitive = 0; packet.material.program = 0x21db4385; packet.material.textures[0].texture = 20;
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
        Reject<std::logic_error>([&] { mscharged::MaterialPrograms duplicate; });
        Require(glGetMaterialProgram(0x21db4385)!=nullptr && glGetMaterialProgram(0x12345678)==nullptr,"Original material registry lookup failed");
        Reject<std::runtime_error>([&] { mscharged::MaterialParameterSize(0x12345678); });
        Reject<std::logic_error>([&] { mscharged::RequireMaterialPreview(); });
        nlMatrix4 identity; identity.SetIdentity();
        auto lighting = mscharged::DefaultGameLighting();
        static_assert(sizeof(GameObjectLight) == 36 && sizeof(StadiumLightingParams) == 64);
        Require(lighting.light_count == 2 && lighting.enabled && lighting.lights[0].intensity == .9f
            && lighting.lights[1].intensity == .25f && lighting.lights[0].unknown08 == 55
            && lighting.lights[1].unknown0C == -120, "Original stadium lighting defaults changed");
        mscharged::ValidateGameLighting(lighting);
        auto invalid_lighting = lighting;
        invalid_lighting.light_count = 7;
        Reject<std::invalid_argument>([&] { mscharged::MaterialPreviewScope context(identity, 0, invalid_lighting); });
        invalid_lighting = lighting; invalid_lighting.lights[0].intensity = -1;
        Reject<std::invalid_argument>([&] { mscharged::ValidateGameLighting(invalid_lighting); });
        invalid_lighting = lighting; invalid_lighting.lights[0].worldPosition.x = std::numeric_limits<float>::infinity();
        Reject<std::invalid_argument>([&] { mscharged::ValidateGameLighting(invalid_lighting); });
        invalid_lighting = lighting; invalid_lighting.lights[0].unknown02 = 1;
        Reject<std::invalid_argument>([&] { mscharged::ValidateGameLighting(invalid_lighting); });
        invalid_lighting = lighting; invalid_lighting.shadow.texture = 21;
        Reject<std::invalid_argument>([&] { mscharged::ValidateGameLighting(invalid_lighting); });
        Reject<std::logic_error>([&] { IsGameObjectLightingEnabled(); });
        {
            mscharged::MaterialPreviewScope context(identity, 0, lighting);
            Require(IsGameObjectLightingEnabled() && GetGameObjectLightCount(false, true) == 2
                && GetGameObjectLight(1, false)->intensity == .25f, "Active original light inputs");
            Reject<std::out_of_range>([&] { GetGameObjectLight(2, false); });
            Reject<std::logic_error>([&] { GetGameObjectLightCount(true, true); });
        }
        {
            mscharged::MaterialPreviewScope context(identity,3);
            Require(mscharged::MaterialPreviewTime()==3,"Material diagnostic clock");
            mscharged::RequireMaterialPreview();
            Reject<std::invalid_argument>([&] { mscharged::MaterialPreviewScope nested(identity,0); });
        }
        float normal[3][4]{};
        auto scaled=identity; scaled.e2[0][0]=2; scaled.e2[3][0]=3;
        mscharged::MaterialNormalMatrix(scaled,normal);
        Require(normal[0][0]==.5f && normal[1][1]==1 && normal[2][2]==1 && normal[0][3]==0,"Material inverse-transpose matrix includes no translation");
        auto recovered = [&] {
            Require(pool.GetFreeMemory() == free && StandardAllocator.TotalFreeMemory() == standard_free
                && VirtualAllocator.TotalFreeMemory() == virtual_free
                && glGetTextureManager()->mFreeIndices->mCount == 4, "Static resource rollback leaked pool/nodes/indices");
        };
        recovered();
        Reject<std::invalid_argument>([&] { mscharged::StaticInventory inventory(pool, {model}, {texture, texture}); });
        recovered();
        auto bad = model; bad.packets[0].material.textures[0].texture = 99;
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
        model.packets[0].material.textures[0].texture = 21;
        {
            mscharged::StaticInventory inventory(pool, {model}, {texture});
            auto* native_texture = glx_GetTex(21);
            Require(native_texture->m_nPaletteEntries == 4
                && std::memcmp(native_texture->m_PaletteData, texture.palette.data(), 8) == 0, "Big-endian palette bytes changed");
        }
        recovered();
        // Original shadow lookup across partial CI8 tiles; the retained palette
        // stays big endian for GX and is decoded explicitly for CPU sampling.
        {
            auto shadow_texture = texture;
            shadow_texture.width = 10; shadow_texture.height = 5;
            shadow_texture.palette_entries = 4;
            shadow_texture.palette = {0x80, 0, 0xc2, 0x10, 0x78, 0x88, 0xff, 0xff};
            shadow_texture.pixels.assign(128, 0);
            for (unsigned y = 0; y < 5; ++y) for (unsigned x = 0; x < 10; ++x)
                shadow_texture.pixels[((y / 4) * 2 + x / 8) * 32 + (y % 4) * 8 + x % 8] = (x+y) % 4;
            mscharged::StaticInventory inventory(pool, {model}, {shadow_texture});
            LightingLookup lookup;
            Reject<std::logic_error>([&] { lookup.SampleColour(0, 0, false); });
            lookup.LoadTexture(21);
            const unsigned intensities[] = {0, 131, 136, 255};
            for (int y = 0; y < 5; ++y) for (int x = 0; x < 10; ++x)
                Require(lookup.SampleColour(x, y, false).c[0] == intensities[(x+y)%4], "Shadow tile/palette decoding");
            Require(lookup.SampleColour(-99,-99,false).c[0] == 0
                && lookup.SampleColour(99,99,false).c[0] == 131, "Shadow sample edge clamp");
            Require(lookup.SampleFilteredColour(1,1,false).c[0] == 168, "Original seven-weight shadow filtering");
            Reject<std::invalid_argument>([&] { lookup.SampleFilteredColour(std::numeric_limits<float>::quiet_NaN(),0,false); });
            lookup.m_NativeShadowTint = {{255,0,0,255}};
            lookup.m_NativeHighlightTint = {{0,0,255,255}};
            Require(lookup.SampleColour(0,0,true).c[0] == 255 && lookup.SampleColour(3,0,true).c[2] == 255,
                "Original shadow tint endpoints");
            lighting.shadow.lookup = &lookup; lighting.shadow.texture = 21;
            mscharged::ValidateGameLighting(lighting);
            {
                mscharged::MaterialPreviewScope context(identity, 0, lighting);
                Reject<std::logic_error>([&] { ApplyGameObjectShadowLighting(1, 0); });
            }
            auto* native_texture = glx_GetTex(21);
            native_texture->m_Width = native_texture->m_Height = 65535;
            Reject<std::out_of_range>([&] { lookup.ReadTextureIntensity(native_texture, 65534, 65534); });
            native_texture->m_Width = 10; native_texture->m_Height = 5;
            native_texture->m_NativeDataBytes = 127;
            Reject<std::invalid_argument>([&] { lookup.LoadTexture(21); });
            native_texture->m_NativeDataBytes = 128;
            static_cast<u8*>(native_texture->m_SwizzledData)[0] = 4;
            Reject<std::out_of_range>([&] { lookup.LoadTexture(21); });
            Require(lookup.SampleColour(0,0,false).c[0] == 0 && lookup.m_NativeTexture == 21,
                "Failed shadow reload must preserve the previous lookup");
            lighting.shadow = {};
        }
        recovered();
        // Real Prepare methods choose alpha/depth/culling using texture metadata.
        texture.id=20; texture.game_format=3; texture.gx_format=6; texture.palette_entries=0; texture.palette.clear();
        texture.pixels.resize(64); model.packets[0].material.textures[0].texture=20;
        model.packets[0].material.program=0x2169db5c;
        model.packets[0].material.scalars={-.25f,.5f,0,0};
        model.packets[0].material.switches={1,1,1,1,0};
        model.packets[0].raster=0xC0007;
        for(unsigned alpha : {0u,1u,8u})
        {
            texture.bits[3]=alpha;
            {
                mscharged::StaticInventory inventory(pool,{model},{texture});
                auto& p=inventory.Model(1)->packets[0];
                auto* params=static_cast<GXScrollingDiffuseParameters*>(p.materialParameters);
                Require(params->scrollSpeedX==-.25f && params->scrollSpeedY==.5f && params->lightingEnabled==1
                    && params->disableCulling==1 && params->diffuseTexture.textureIndex==0xffff,"Native typed material parameters");
                Require(p.numStreams==4 && p.streams[1].stride==12 && p.streams[3].stride==4,"Native normal/colour streams");
                Require(glGetRasterState(p.rasterState,GLS_AlphaTest)==(alpha!=0)
                    && glGetRasterState(p.rasterState,GLS_AlphaBlend)==(alpha>1)
                    && glGetRasterState(p.rasterState,GLS_DepthWrite)==(alpha<2)
                    && glGetRasterState(p.rasterState,GLS_Culling)==0,"Original material alpha preparation");
            }
            recovered();
        }
        materials.Release();
        Require(!glGetMaterialProgram(0x2169db5c) && !GXScrollingDiffuseMaterialProgram::Instance
            && !GXScrollingDiffuseMaterialProgram::Initialized,"Material shutdown left stale registry/instance state");
        { mscharged::MaterialPrograms restart; Require(glGetMaterialProgram(0x32475c7d)!=nullptr,"Material registry restart failed"); }
        glShutdownMemory(); mscharged::ResetStartupMemory();
        std::cout << "Static inventory: pool-owned native records, original lookup, GPU drain, tiled/palette byte retention and failure rollback passed\n";
        return 0;
    }
    catch (const std::exception& error) { std::cerr << "FAILED: " << error.what() << '\n'; return 1; }
}
