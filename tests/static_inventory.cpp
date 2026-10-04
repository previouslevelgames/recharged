#include "runtime/static_inventory.h"
#include "runtime/materials.h"
#include "runtime/graphics_state.h"
#include "runtime/material_environment.h"
#include "Game/GameObjectLighting.h"
#include "Game/Render/LightingLookup.h"
#include "NL/gl/glMaterialProgram.h"
#include "NL/gl/glState.h"
#include "NL/glx/GXScrollingDiffuseMaterialProgram.h"
#include "NL/glx/GXShadowVolumeMaterialProgram.h"
#include "NL/glx/GXMaskedSpecularFresnelMaterialProgram.h"
#include "NL/glx/GXSpecularDetailBlendMaterialProgram.h"
#include "NL/glx/GXScrollingSpecularMaterialProgram.h"
#include "NL/glx/GXCameraScrolledOverlayMaterialProgram.h"
#include "NL/glx/GXScrollingCameraOverlayMaterialProgram.h"
#include "NL/glx/GXMaskedDetailBlendMaterialProgram.h"
#include "NL/glx/GXScrollingMaskedDetailBlendMaterialProgram.h"
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
        Require(!lighting.lights[0].useWorldPosition && !lighting.lights[0].useColour
            && !lighting.lights[0].isPointLight, "Original default light coordinate/colour/type flags changed");
        Require(lighting.light_count == 2 && lighting.enabled && lighting.lights[0].intensity == .9f
            && lighting.lights[1].intensity == .25f && lighting.lights[0].rotYDeg == 55
            && lighting.lights[1].rotZDeg == -120, "Original stadium lighting defaults changed");
        mscharged::ValidateGameLighting(lighting);
        auto invalid_lighting = lighting;
        invalid_lighting.light_count = 7;
        Reject<std::invalid_argument>([&] { mscharged::MaterialPreviewScope context(identity, 0, invalid_lighting); });
        invalid_lighting = lighting; invalid_lighting.lights[0].intensity = -1;
        Reject<std::invalid_argument>([&] { mscharged::ValidateGameLighting(invalid_lighting); });
        invalid_lighting = lighting; invalid_lighting.lights[0].worldPosition.x = std::numeric_limits<float>::infinity();
        Reject<std::invalid_argument>([&] { mscharged::ValidateGameLighting(invalid_lighting); });
        invalid_lighting = lighting; invalid_lighting.lights[0].isPointLight = 1;
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
        Reject<std::logic_error>([] { mscharged::MaterialPreviewCameraPosition(); });
        {
            nlVector3 camera{12,-34,56};
            mscharged::MaterialPreviewScope context(identity, 0, {}, &camera);
            camera.x = 99;
            const auto& stored = mscharged::MaterialPreviewCameraPosition();
            Require(stored.x == 12 && stored.y == -34 && stored.z == 56, "Camera context must own its explicit input");
        }
        {
            mscharged::MaterialPreviewScope context(identity, 0);
            Reject<std::logic_error>([] { mscharged::MaterialPreviewCameraPosition(); });
        }
        for (unsigned axis = 0; axis < 3; ++axis)
            for (float value : {std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity()})
            {
                nlVector3 camera{0,0,0}; (axis == 0 ? camera.x : axis == 1 ? camera.y : camera.z) = value;
                Reject<std::invalid_argument>([&] { mscharged::MaterialPreviewScope context(identity, 0, {}, &camera); });
            }
        {
            nlVector3 camera{-1,2,-3};
            mscharged::MaterialPreviewScope context(identity, 0, {}, &camera);
            Require(mscharged::MaterialPreviewCameraPosition().x == -1, "Camera context did not recover after rejection");
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
        {
            auto volume=model;volume.packets[0].material.program=0x386ecbdd;
            volume.packets[0].material.switches[0]=1;
            mscharged::StaticInventory inventory(pool,{volume},{texture});
            const auto* packet=inventory.Model(1)->packets;
            const auto* parameters=static_cast<const GXShadowVolumeParameters*>(packet->materialParameters);
            Require(parameters->useFixedColour==1 && parameters->diffuseTexture.texture==20,
                    "Native volume material parameter layout");
            Require(packet->numStreams==3 && packet->streams[0].id==1 && packet->streams[1].id==3
                    && packet->streams[2].id==4 && packet->streams[2].stride==8,
                    "Native volume position/colour/floating UV stream order");
        }
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
        {
            auto detail = model;
            auto& p = detail.packets[0];
            p.material = {};
            p.material.program = 0x112ab470;
            p.material.scalars = {.25f, .75f, 64, 0};
            p.material.specular_colour = {.2f, .4f, .6f, .8f};
            p.material.switches = {1,1,0,0,0};
            std::vector<mscharged::resources::Texture> textures;
            for (unsigned i = 0; i < 4; ++i)
            {
                auto t = texture; t.id = 20 + i; textures.push_back(t);
                p.material.textures[i] = {20 + i, static_cast<std::uint8_t>(i)};
            }
            for (auto& v : p.vertices)
            {
                v.normal = {0,0,1}; v.colour = {10,20,30,40};
                v.uv = {1,-1}; v.uv1 = {2,-2}; v.uv2 = {3,-3}; v.uv3 = {4,-4};
            }
            {
                mscharged::StaticInventory inventory(pool, {detail}, textures);
                const auto& native = inventory.Model(1)->packets[0];
                const auto& params = *static_cast<const GXSpecularDetailBlendParameters*>(native.materialParameters);
                Require(native.numStreams == 7 && native.streams[5].index == 3 && native.streams[5].stride == 8
                    && static_cast<float*>(native.streams[5].address)[0] == 4
                    && static_cast<float*>(native.streams[5].address)[1] == -4
                    && native.streams[6].id == 3 && static_cast<u8*>(native.streams[6].address)[2] == 30,
                    "Detail fourth UV and colour stream installation");
                Require(params.diffuseTexture.texture == 20 && params.detailTexture.texture == 21
                    && params.blendMaskTexture.texture == 22 && params.glossTexture.texture == 23
                    && params.glossTexture.flags == 3 && params.glossTexture.textureIndex == 0xffff
                    && params.blendAmount == .25f && params.specularLevel == .75f && params.specularExponent == 64
                    && params.specularColour.c[2] == .6f && params.lightingEnabled == 1 && params.shadowEnabled == 1,
                    "Detail typed parameters and all four bindings");
            }
            recovered();
            auto invalid = detail; invalid.packets[0].material.textures[3].texture = 99;
            Reject<std::runtime_error>([&] { mscharged::StaticInventory inventory(pool, {invalid}, textures); });
            recovered();
            for (float value : {-1.f, 2.f, std::numeric_limits<float>::quiet_NaN()})
            {
                invalid = detail; invalid.packets[0].material.specular_colour[0] = value;
                Reject<std::invalid_argument>([&] { mscharged::StaticInventory inventory(pool, {invalid}, textures); });
                recovered();
                invalid = detail; invalid.packets[0].material.scalars[0] = value;
                Reject<std::invalid_argument>([&] { mscharged::StaticInventory inventory(pool, {invalid}, textures); });
                recovered();
            }
            invalid = detail; invalid.packets[0].material.scalars[2] = -1;
            Reject<std::invalid_argument>([&] { mscharged::StaticInventory inventory(pool, {invalid}, textures); });
            recovered();
            GameObjectLight light;
            light.useWorldPosition = true; light.intensity = 1;
            Reject<std::invalid_argument>([&] { LoadGameObjectSpecularLight(0, &light, 64, identity); });
            Reject<std::invalid_argument>([&] { LoadGameObjectSpecularLight(0, nullptr, 64, identity); });
            Reject<std::invalid_argument>([&] { LoadGameObjectSpecularLight(0, &light, -1, identity); });
            Reject<std::out_of_range>([&] { SetGameObjectSpecularLightingEnabled(1, 7); });
        }
        {
            auto scrolling = model;
            auto& p = scrolling.packets[0];
            p.material = {};
            p.material.program = 0x3eccd955;
            p.material.textures[0] = {20,1}; p.material.textures[1] = {21,2};
            p.material.scalars = {.75f,64,-.25f,.5f};
            p.material.specular_colour = {.2f,.4f,.6f,.8f};
            p.material.switches = {1,0,1,0,0};
            for (auto& v : p.vertices)
            {v.normal={0,0,1};v.uv={-1,.5f};v.uv1={2,-32};v.colour={10,20,30,255};}
            auto diffuse=texture, specular=texture; diffuse.id=20; specular.id=21;
            {
                mscharged::StaticInventory inventory(pool,{scrolling},{diffuse,specular});
                const auto& native=inventory.Model(1)->packets[0];
                const auto& params=*static_cast<const GXScrollingSpecularParameters*>(native.materialParameters);
                Require(native.numStreams==5 && native.streams[1].stride==12 && native.streams[3].stride==8
                    && native.streams[3].index==1 && static_cast<float*>(native.streams[3].address)[1]==-32
                    && native.streams[4].id==3 && static_cast<u8*>(native.streams[4].address)[2]==30,
                    "Scrolling normal, second UV and colour installation");
                Require(params.diffuseTexture.texture==20 && params.diffuseTexture.flags==1
                    && params.specularTexture.texture==21 && params.specularTexture.flags==2
                    && params.specularTexture.textureIndex==0xffff && params.specularLevel==.75f
                    && params.specularExponent==64 && params.specularColour.c[2]==.6f
                    && params.scrollSpeedX==-.25f && params.scrollSpeedY==.5f
                    && params.scrollSpecularTexture==1 && params.lightingEnabled==0 && params.shadowEnabled==1,
                    "Scrolling typed parameters and two bindings");
            }
            recovered();
            Reject<std::runtime_error>([&]{mscharged::StaticInventory inventory(pool,{scrolling},{diffuse});});
            recovered();
            for(float value:{-1.f,2.f,std::numeric_limits<float>::quiet_NaN()})
            {
                auto invalid=scrolling;invalid.packets[0].material.specular_colour[2]=value;
                Reject<std::invalid_argument>([&]{mscharged::StaticInventory inventory(pool,{invalid},{diffuse,specular});});
                recovered();
                invalid=scrolling;invalid.packets[0].material.scalars[0]=value;
                Reject<std::invalid_argument>([&]{mscharged::StaticInventory inventory(pool,{invalid},{diffuse,specular});});
                recovered();
            }
            for(unsigned field=0;field<3;++field)
            {
                auto invalid=scrolling;invalid.packets[0].material.switches[field]=2;
                Reject<std::invalid_argument>([&]{mscharged::StaticInventory inventory(pool,{invalid},{diffuse,specular});});
                recovered();
            }
        }
        for (bool scrolling : {false,true})
        {
            auto overlay = model;
            auto& p = overlay.packets[0];
            p.material = {}; p.material.program = scrolling ? 0x845cad59 : 0x32bc21e8;
            p.material.scalars = {-2,-.5f,.25f,0}; p.material.switches = scrolling ? std::array<std::uint32_t,5>{1,0,0,1,0} : std::array<std::uint32_t,5>{1,0,1,0,0};
            if(scrolling) p.material.scroll_speeds[0]={-.5f,.25f};
            std::vector<mscharged::resources::Texture> textures;
            for (unsigned i = 0; i < 3; ++i)
            {
                auto t = texture; t.id = 30 + i; textures.push_back(t);
                p.material.textures[i] = {30 + i, static_cast<u8>(i)};
            }
            for (auto& v : p.vertices) { v.normal = {0,0,1}; v.uv1 = {2,-2}; v.uv2 = {3,-3}; }
            {
                mscharged::StaticInventory inventory(pool, {overlay}, textures);
                const auto& native = inventory.Model(1)->packets[0];
                Require(native.numStreams == 6 && native.streams[4].stride == 8 && native.streams[4].index == 2
                    && static_cast<float*>(native.streams[4].address)[1] == -3 && native.streams[5].id == 3,
                    "Overlay native stream ordering");
                if(scrolling)
                {
                    const auto& params=*static_cast<const GXScrollingCameraOverlayParameters*>(native.materialParameters);
                Require(params.diffuseTexture.texture == 30 && params.overlayTexture.texture == 31
                    && params.overlayMaskTexture.texture == 32 && params.overlayMaskTexture.flags == 2
                    && params.overlayMaskTexture.textureIndex == 0xffff && params.overlayScale == -2
                    && params.cameraScroll == -.5f && params.overlayAmount == .25f && params.diffuseWrapEnabled == 1
                    && params.lightingEnabled == 0 && params.shadowEnabled == 1, "Overlay typed parameters");
                    Require(params.diffuseScrollSpeedX==-.5f && params.diffuseScrollSpeedY==.25f
                        && params.maskScrollEnabled==0,"Scrolling camera native parameters");
                }
                else
                {
                    const auto& params=*static_cast<const GXCameraScrolledOverlayParameters*>(native.materialParameters);
                Require(params.diffuseTexture.texture == 30 && params.overlayTexture.texture == 31
                    && params.overlayMaskTexture.texture == 32 && params.overlayMaskTexture.flags == 2
                    && params.overlayMaskTexture.textureIndex == 0xffff && params.overlayScale == -2
                    && params.cameraScroll == -.5f && params.overlayAmount == .25f && params.diffuseWrapEnabled == 1
                    && params.lightingEnabled == 0 && params.shadowEnabled == 1, "Overlay typed parameters");
                }
            }
            recovered();
            auto missing = textures; missing.pop_back();
            Reject<std::runtime_error>([&] { mscharged::StaticInventory inventory(pool, {overlay}, missing); });
            recovered();
            for (float value : {-1.f,2.f,std::numeric_limits<float>::quiet_NaN()})
            {
                auto invalid = overlay; invalid.packets[0].material.scalars[2] = value;
                Reject<std::invalid_argument>([&] { mscharged::StaticInventory inventory(pool, {invalid}, textures); });
                recovered();
            }
            auto invalid = overlay; invalid.packets[0].material.scalars[0] = std::numeric_limits<float>::denorm_min();
            Reject<std::invalid_argument>([&] { mscharged::StaticInventory inventory(pool, {invalid}, textures); });
            recovered();
            if(scrolling)
            {
                invalid=overlay;invalid.packets[0].material.scalars[0]=0;
                Reject<std::invalid_argument>([&]{mscharged::StaticInventory inventory(pool,{invalid},textures);});recovered();
                for(unsigned i=0;i<2;++i) for(float value:{10001.f,-10001.f,std::numeric_limits<float>::quiet_NaN()})
                {
                    invalid=overlay;invalid.packets[0].material.scroll_speeds[0][i]=value;
                    Reject<std::invalid_argument>([&]{mscharged::StaticInventory inventory(pool,{invalid},textures);});recovered();
                }
            }
            for (unsigned field = 0; field < (scrolling ? 4u : 3u); ++field)
            {
                invalid = overlay; invalid.packets[0].material.switches[field] = 2;
                Reject<std::invalid_argument>([&] { mscharged::StaticInventory inventory(pool, {invalid}, textures); });
                recovered();
            }
        }
        for (bool scrolling : {false,true})
        {
            auto detail = model; auto& p = detail.packets[0];
            p.material = {}; p.material.program = scrolling ? 0xf2d57ac6 : 0x09609a35;
            if(scrolling) p.material.scroll_speeds={{{-.5f,.25f},{.75f,-1},{1.25f,-1.5f}}};
            p.material.scalars[0] = .25f; p.material.switches = {1,0,0,0,0};
            std::vector<mscharged::resources::Texture> textures;
            for (unsigned i=0;i<3;++i)
            { auto t=texture; t.id=40+i; textures.push_back(t); p.material.textures[i]={40+i,static_cast<u8>(i)}; }
            for(auto& v:p.vertices){v.normal={0,0,1};v.uv1={2,-2};v.uv2={3,-3};}
            {
                mscharged::StaticInventory inventory(pool,{detail},textures);
                const auto& packet=inventory.Model(1)->packets[0];
                Require(packet.numStreams==6 && packet.streams[1].stride==12 && packet.streams[4].stride==8
                    && packet.streams[4].index==2 && static_cast<float*>(packet.streams[4].address)[1]==-3
                    && packet.streams[5].id==3,"Masked detail native streams");
                if(scrolling)
                {
                    const auto& params=*static_cast<const GXScrollingMaskedDetailBlendParameters*>(packet.materialParameters);
                Require(params.diffuseTexture.texture==40 && params.detailTexture.texture==41
                    && params.blendMaskTexture.texture==42 && params.blendMaskTexture.flags==2
                    && params.blendMaskTexture.textureIndex==0xffff && params.blendAmount==.25f
                    && params.lightingEnabled==1 && params.shadowEnabled==0,"Masked detail native parameters");
                    Require(params.diffuseScrollSpeedX==-.5f && params.diffuseScrollSpeedY==.25f
                        && params.detailScrollSpeedX==.75f && params.detailScrollSpeedY==-1
                        && params.blendMaskScrollSpeedX==1.25f && params.blendMaskScrollSpeedY==-1.5f,"Masked detail scroll pairs");
                }
                else
                {
                    const auto& params=*static_cast<const GXMaskedDetailBlendParameters*>(packet.materialParameters);
                Require(params.diffuseTexture.texture==40 && params.detailTexture.texture==41
                    && params.blendMaskTexture.texture==42 && params.blendMaskTexture.flags==2
                    && params.blendMaskTexture.textureIndex==0xffff && params.blendAmount==.25f
                    && params.lightingEnabled==1 && params.receiveShadows==0,"Masked detail native parameters");
                }
            }
            recovered();
            for(unsigned i=0;i<3;++i)
            {
                auto missing=textures;missing.erase(missing.begin()+i);
                Reject<std::runtime_error>([&]{mscharged::StaticInventory inventory(pool,{detail},missing);});recovered();
                auto invalid=detail;invalid.packets[0].material.textures[i].flags=4;
                Reject<std::invalid_argument>([&]{mscharged::StaticInventory inventory(pool,{invalid},textures);});recovered();
            }
            for(float value:{-1.f,2.f,std::numeric_limits<float>::quiet_NaN()})
            {
                auto invalid=detail;invalid.packets[0].material.scalars[0]=value;
                Reject<std::invalid_argument>([&]{mscharged::StaticInventory inventory(pool,{invalid},textures);});recovered();
            }
            if(scrolling) for(unsigned i=0;i<6;++i)
                for(float value:{10001.f,-10001.f,std::numeric_limits<float>::infinity(),std::numeric_limits<float>::quiet_NaN()})
                {
                    auto invalid=detail;invalid.packets[0].material.scroll_speeds[i/2][i%2]=value;
                    Reject<std::invalid_argument>([&]{mscharged::StaticInventory inventory(pool,{invalid},textures);});recovered();
                }
            for(unsigned i=0;i<2;++i)
            {
                auto invalid=detail;invalid.packets[0].material.switches[i]=2;
                Reject<std::invalid_argument>([&]{mscharged::StaticInventory inventory(pool,{invalid},textures);});recovered();
            }
        }
        materials.Release();
        Require(!glGetMaterialProgram(0x2169db5c) && !GXScrollingDiffuseMaterialProgram::Instance
            && !GXScrollingDiffuseMaterialProgram::Initialized,"Material shutdown left stale registry/instance state");
        Require(!glGetMaterialProgram(0x845cad59) && !GXScrollingCameraOverlayMaterialProgram::Instance
            && !GXScrollingCameraOverlayMaterialProgram::Initialized,"Scrolling camera registry teardown");
        Require(!glGetMaterialProgram(0x32bc21e8) && !GXCameraScrolledOverlayMaterialProgram::Instance
            && !GXCameraScrolledOverlayMaterialProgram::Initialized, "Overlay registry teardown");
        Require(!glGetMaterialProgram(0x3eccd955) && !GXScrollingSpecularMaterialProgram::Instance
            && !GXScrollingSpecularMaterialProgram::Initialized, "Scrolling material shutdown left stale state");
        Require(!glGetMaterialProgram(0x112ab470) && !GXSpecularDetailBlendMaterialProgram::Instance
            && !GXSpecularDetailBlendMaterialProgram::Initialized, "Detail material shutdown left stale state");
        Require(!glGetMaterialProgram(0xf2d57ac6) && !GXScrollingMaskedDetailBlendMaterialProgram::Instance
            && !GXScrollingMaskedDetailBlendMaterialProgram::Initialized,"Scrolling masked detail registry teardown");
        Require(!glGetMaterialProgram(0x09609a35) && !GXMaskedDetailBlendMaterialProgram::Instance
            && !GXMaskedDetailBlendMaterialProgram::Initialized,"Masked detail registry teardown");
        for (unsigned i = 0; i < 3; ++i)
        {
            mscharged::MaterialPrograms restart;
            Require(glGetMaterialProgram(0x845cad59) && GXScrollingCameraOverlayMaterialProgram::Initialized,"Scrolling camera restart");
            Require(glGetMaterialProgram(0xf2d57ac6) && GXScrollingMaskedDetailBlendMaterialProgram::Initialized,"Scrolling masked detail restart");
            Require(glGetMaterialProgram(0x09609a35) && GXMaskedDetailBlendMaterialProgram::Initialized,"Masked detail restart");
            Require(glGetMaterialProgram(0x32475c7d) && glGetMaterialProgram(0x112ab470)
                && GXSpecularDetailBlendMaterialProgram::Initialized
                && glGetMaterialProgram(0x3eccd955) && GXScrollingSpecularMaterialProgram::Initialized
                && glGetMaterialProgram(0x32bc21e8) && GXCameraScrolledOverlayMaterialProgram::Initialized, "Material registry restart failed");
        }
        glShutdownMemory(); mscharged::ResetStartupMemory();
        std::cout << "Static inventory: pool-owned native records, original lookup, GPU drain, tiled/palette byte retention and failure rollback passed\n";
        return 0;
    }
    catch (const std::exception& error) { std::cerr << "FAILED: " << error.what() << '\n'; return 1; }
}
