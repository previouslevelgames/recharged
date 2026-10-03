#include "runtime/world_objects.h"
#include "runtime/materials.h"
#include "runtime/graphics_state.h"
#include "runtime/graphics_memory.h"
#include "runtime/startup.h"
#include "Game/GL/GLInventory.h"
#include "Game/GL/GLTextureAnim.h"
#include "NL/gl/glMemoryInit.h"
#include "NL/gl/glMemory.h"
#include "NL/gl/glMatrix.h"
#include "NL/gl/glTextureManager.h"
#include "NL/gl/glMaterialProgram.h"
#include "NL/MemAlloc.h"
#include <cstring>
#include <iostream>
#include <limits>

namespace
{
using namespace mscharged;
unsigned checks = 0, drains = 0, cache_invalidations = 0;
StaticWorldObjects* draining = nullptr;
bool fail_drain = false;
void Check(bool yes) { ++checks; if (!yes) throw std::runtime_error("World ownership assertion failed"); }
template<class E = std::exception, class F> void Reject(F f)
{
    ++checks;
    try { f(); } catch (const E&) { return; }
    throw std::runtime_error("Invalid world ownership operation accepted");
}
void Drain()
{
    ++drains;
    if (draining)
    {
        Check(draining->Objects().size() == 2); // Data remains alive during drain.
        Reject<std::logic_error>([] { draining->Release(); });
    }
    if (fail_drain) throw std::runtime_error("Injected drain failure");
}
void Invalidate() { ++cache_invalidations; }
resources::StaticWorldObject Object(unsigned id, float x)
{
    resources::StaticWorldObject o;
    o.id = id; o.type = 0x10002; o.model = 123; o.creation_flags = 3;
    o.transform = {2,0,0,0, 0,-3,0,0, 0,0,.5f,0, x,4,5,1};
    o.radius = 20; o.bounds_min = {-4,-5,-6}; o.bounds_max = {7,8,9}; return o;
}
void Session()
{
    const auto start1 = StandardAllocator.TotalFreeMemory(), start2 = VirtualAllocator.TotalFreeMemory();
    const GLMemoryRequirement req[] = {{GLM_Header, 4096}, {GLM_VertexData, 4096}};
    const GLMemoryConfig config{4096, 4096, req, 2, 8};
    glInitMemory(&config); InitializeOriginalGraphicsState(); SetGraphicsCacheInvalidator(Invalidate);
    {
        MaterialPrograms materials;
        auto* original = glGetCurrentResourcePool();
        const auto free1 = StandardAllocator.TotalFreeMemory(), free2 = VirtualAllocator.TotalFreeMemory();
        const auto queue = glGetTextureManager()->mFreeIndices->mCount;
        auto recovered = [&] {
            Check(StandardAllocator.TotalFreeMemory() == free1 && VirtualAllocator.TotalFreeMemory() == free2);
            Check(glGetCurrentResourcePool() == original && glGetResourcePools() == original
                && original->m_next == original && original->m_prev == original);
            Check(glGetTextureManager()->mFreeIndices->mCount == queue);
        };
        std::vector objects{Object(10, 12), Object(11, -8)};
        resources::StaticModel model{123, {}};
        resources::Packet packet;
        packet.primitive = 0; packet.material.program = 0x21db4385; packet.material.textures[0].texture = 70;
        packet.vertices = {{{1,2,3},{0,0}}, {{4,5,6},{1,0}}, {{7,8,9},{0,1}}};
        packet.indices = {0,1,2}; model.packets = {packet, packet};
        std::vector models{model};
        resources::TextureBundle bundle;
        resources::Texture tex;
        tex.id = 50; tex.width = tex.height = 4; tex.levels = 1; tex.game_format = 3; tex.gx_format = 6;
        tex.bits = {8,8,8,8}; tex.pixels.resize(64, 0x91); bundle.textures.push_back(tex);
        tex.id = 51; tex.pixels.assign(64, 0x37); bundle.textures.push_back(tex);
        resources::TextureAnimation anim{70, 0, 1, false, 0, {{50, .1f}, {51, .1f}}};
        bundle.animations.push_back(anim);
        constexpr WorldObjectMemory memory{16384, 8192};
        {
            auto own_objects = objects; auto own_models = models; auto own_bundle = bundle;
            StaticWorldObjects owner(own_objects, own_models, own_bundle, memory, Drain);
            Check(glGetCurrentResourcePool() == original && owner.Objects().size() == 2 && !owner.Find(99));
            auto* a = owner.Find(10)->model; auto* b = owner.Find(11)->model;
            auto* source = owner.Pool().m_inventory->GetModel(123);
            Check(a != b && a != source && a->packets != b->packets && a->packets != source->packets);
            Check(a->packets[0].materialParameters != b->packets[0].materialParameters
                && a->packets[0].materialParameters != source->packets[0].materialParameters);
            Check(a->packets[0].streams == b->packets[0].streams && a->packets[0].indexBuffer == b->packets[0].indexBuffer);
            Check(a->packets[0].streams == source->packets[0].streams && a->packets[0].matrix != b->packets[0].matrix);
            Check(a->packets[0].matrix == a->packets[1].matrix && a->packets[0].matrix != source->packets[0].matrix);
            Check(a->packets[0].matrix > UINT32_MAX && a->packets[0].matrix % 32 == 0);
            own_objects.clear(); own_models.clear(); own_bundle.textures.clear(); own_bundle.animations.clear();
            nlMatrix4 matrix;
            glModelGetMatrix(a, matrix); Check(std::memcmp(matrix.e, objects[0].transform.data(), 64) == 0);
            glModelGetMatrix(b, matrix); Check(std::memcmp(matrix.e, objects[1].transform.data(), 64) == 0);
            auto* parameters = static_cast<glTextureBinding*>(a->packets[0].materialParameters);
            parameters->flags = 3;
            Check(static_cast<glTextureBinding*>(b->packets[0].materialParameters)->flags == 0
                && static_cast<glTextureBinding*>(source->packets[0].materialParameters)->flags == 0);
            glSetCurrentResourcePool(&owner.Pool());
            auto* native_anim = owner.Pool().m_inventory->GetTextureAnim(70);
            Check(native_anim && native_anim->m_nFrame == 0);
            const auto alias = glGetTextureManager()->GetTextureIndex(70);
            auto* frame0 = glGetTextureManager()->GetTextureAtIndex(&alias);
            owner.Pool().m_inventory->UpdateTextureAnims(.1f);
            Check(native_anim->m_nFrame == 1 && glGetTextureManager()->GetTextureAtIndex(&alias) != frame0);
            glSetCurrentResourcePool(original);
            for (int frame = 0; frame < 3; ++frame) glplatFrameAllocNextFrame();
            glModelGetMatrix(a, matrix); Check(std::memcmp(matrix.e, objects[0].transform.data(), 64) == 0);
            const auto old_drains = drains;
            draining = &owner; fail_drain = true;
            Reject<std::runtime_error>([&] { owner.Release(); });
            Check(owner.Find(10) && owner.Find(10)->model == a && owner.Pool().m_inventory->GetModel(123) == source);
            fail_drain = false; owner.Release(); owner.Release(); draining = nullptr;
            Check(owner.Objects().empty() && !owner.Find(10) && drains == old_drains + 2);
            Reject<std::logic_error>([&] { owner.Pool(); });
        }
        recovered();
        const auto old_drains = drains;
        auto bad_objects = objects; bad_objects[1].id = 10;
        Reject([&] { StaticWorldObjects owner(bad_objects, models, bundle, memory, Drain); }); recovered();
        bad_objects = objects; bad_objects[1].model = 999;
        Reject([&] { StaticWorldObjects owner(bad_objects, models, bundle, memory, Drain); }); recovered();
        bad_objects = objects; bad_objects[0].transform[0] = std::numeric_limits<float>::infinity();
        Reject([&] { StaticWorldObjects owner(bad_objects, models, bundle, memory, Drain); }); recovered();
        Reject([&] { StaticWorldObjects owner({}, models, bundle, memory, Drain); }); recovered();
        Reject([&] { StaticWorldObjects owner(objects, {model, model}, bundle, memory, Drain); }); recovered();
        Reject([&] { StaticWorldObjects owner(objects, models, bundle, {SIZE_MAX,1}, Drain); }); recovered();
        auto bad_models = models; bad_models[0].packets[0].indices[0] = 999;
        Reject([&] { StaticWorldObjects owner(objects, bad_models, bundle, memory, Drain); }); recovered();
        auto bad_bundle = bundle; bad_bundle.animations[0].frames[1].texture = 999;
        Reject([&] { StaticWorldObjects owner(objects, models, bad_bundle, memory, Drain); }); recovered();
        Reject<std::bad_alloc>([&] { StaticWorldObjects owner(objects, models, bundle, {16384,32}, Drain); }); recovered();
        // Exercise exhaustion before and after source installation, cloning,
        // material copies and persistent matrix allocation.
        unsigned failures = 0, successes = 0;
        for (std::size_t bytes = 64; bytes <= 8192; bytes += 128)
        {
            try { StaticWorldObjects owner(objects, models, bundle, {bytes,8192}); ++successes; }
            catch (const std::bad_alloc&) { ++failures; }
            recovered();
        }
        Check(failures > 10 && successes > 10 && drains == old_drains);
        // Two independent batches may resolve the same IDs against their own
        // pools; tearing down either must not touch the other's geometry.
        {
            StaticWorldObjects first(objects, models, bundle, memory);
            StaticWorldObjects second(objects, models, bundle, memory);
            Check(glGetCurrentResourcePool() == original);
            const auto* second_model = second.Find(10)->model;
            first.Release();
            Check(second.Find(10)->model == second_model && second_model->packets[0].indexBuffer[2] == 2);
        }
        recovered();
    }
    glShutdownMemory(); SetGraphicsCacheInvalidator(nullptr);
    Check(StandardAllocator.TotalFreeMemory() == start1 && VirtualAllocator.TotalFreeMemory() == start2);
}
}
int main()
{
    try
    {
        std::vector<std::uint64_t> standard(2 * 1024 * 1024), virtual_arena(2 * 1024 * 1024);
        for (int session = 0; session < 3; ++session)
        {
            mscharged::ResetStartupMemory();
            StandardAllocator.Initialize(standard.data(), standard.size() * 8);
            VirtualAllocator.Initialize(virtual_arena.data(), virtual_arena.size() * 8);
            gMemoryInitialized = 1; Session(); mscharged::ResetStartupMemory();
        }
        Check(cache_invalidations == 9);
        std::cout << "World ownership checks: " << checks << ", three recovered arena sessions\n"; return 0;
    }
    catch (const std::exception& e) { std::cerr << e.what() << " (check " << checks << ")\n"; return 1; }
}
