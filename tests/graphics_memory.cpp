#include "runtime/graphics_memory.h"
#include "runtime/startup.h"
#include "NL/gl/glMemory.h"
#include "NL/gl/glMemoryInit.h"
#include "NL/gl/glModel.h"
#include "NL/gl/glTexture.h"
#include "NL/gl/glTextureManager.h"
#include "NL/glx/glxTexture.h"
#include "Game/GL/GLInventory.h"
#include "Game/GraphicsMemoryStartup.h"
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <memory>
#include <new>
#include <random>
#include <stdexcept>
#include <vector>

namespace
{
void Require(bool condition, const char* message)
{ if (!condition) throw std::runtime_error(message); }
template<class Error, class Action> void Reject(Action action)
{
    try { action(); } catch (const Error&) { return; }
    throw std::runtime_error("Invalid graphics memory operation was accepted");
}
struct HostFree
{ void operator()(void* memory) const { ::operator delete(memory, std::align_val_t(64)); } };
struct Arenas
{
    std::unique_ptr<void, HostFree> mem1, mem2;
    Arenas(unsigned standard = 8 * 1024 * 1024, unsigned virtual_size = 16 * 1024 * 1024)
        : mem1(::operator new(standard, std::align_val_t(64))),
          mem2(::operator new(virtual_size, std::align_val_t(64)))
    {
        mscharged::ResetStartupMemory();
        StandardAllocator.Initialize(mem1.get(), standard);
        VirtualAllocator.Initialize(mem2.get(), virtual_size);
        gMemoryInitialized = 1;
        Require(reinterpret_cast<std::uintptr_t>(mem1.get()) > UINT32_MAX
            && reinterpret_cast<std::uintptr_t>(mem2.get()) > UINT32_MAX, "Test arenas must exercise pointers above 4 GiB");
    }
    ~Arenas() { glShutdownMemory(); mscharged::ResetStartupMemory(); }
};
unsigned invalidations = 0, model_releases = 0;
void Invalidate() { ++invalidations; }
void ReleaseModel(glModel*) { ++model_releases; }

const GLMemoryRequirement requirements[] = {{GLM_Header, 65536}, {GLM_VertexData, 65536}};
const GLMemoryConfig config{65, 97, requirements, 2, 4};
bool Owns(MemoryAllocator& arena, void* pointer)
{
    auto address = reinterpret_cast<std::uintptr_t>(pointer);
    auto begin = reinterpret_cast<std::uintptr_t>(arena.m_memory);
    return address >= begin && address < begin + arena.m_memory_size;
}
void Aligned(void* pointer)
{ Require(pointer && reinterpret_cast<std::uintptr_t>(pointer) % 32 == 0, "GL allocation lost 32-byte alignment/native address"); }

void CheckFrames()
{
    Reject<std::logic_error>([] { glFrameAlloc(1, GLM_Header); });
    Reject<std::invalid_argument>([] { glInitMemory(nullptr); });
    glInitResourcePools();
    glInitMemory(&config);
    Require(glGetCurrentResourcePool() && glGetTextureManager()->mCapacity == 4, "GL global initialization failed");
    Reject<std::logic_error>([] { glInitMemory(&config); });
    Reject<std::logic_error>([] { glInitResourcePools(); });
    Reject<std::invalid_argument>([] { glFrameAlloc(1, static_cast<eGLMemory>(99)); });
    auto* first = static_cast<unsigned char*>(glFrameAlloc(65, GLM_Header));
    auto* first_virtual = static_cast<unsigned char*>(glFrameAlloc(97, GLM_VertexData));
    Aligned(first); Aligned(first_virtual);
    Require(Owns(StandardAllocator, first) && Owns(VirtualAllocator, first_virtual), "GL frames use the wrong arena");
    std::memset(first, 0xa1, 65); std::memset(first_virtual, 0xb2, 97);
    Reject<std::bad_alloc>([] { glFrameAlloc(1, GLM_Header); });
    Reject<mscharged::StartupStopped>([] { glplatFrameAllocNextFrame(); });
    mscharged::SetGraphicsCacheInvalidator(Invalidate);
    glplatFrameAllocNextFrame();
    auto* second = static_cast<unsigned char*>(glFrameAlloc(65, GLM_Header));
    auto* second_virtual = static_cast<unsigned char*>(glFrameAlloc(97, GLM_TextureData));
    Aligned(second); Aligned(second_virtual);
    Require(second == first + 96 && second_virtual == first_virtual + 128, "GL double-buffer strides are wrong");
    std::memset(second, 0xc3, 65); std::memset(second_virtual, 0xd4, 97);
    for (unsigned i = 0; i < 65; ++i) Require(first[i] == 0xa1, "Second frame overwrote first-frame memory");
    for (unsigned i = 0; i < 97; ++i) Require(first_virtual[i] == 0xb2, "Second MEM2 frame overwrote first-frame memory");
    glplatFrameAllocNextFrame();
    Require(glFrameAlloc(65, GLM_Header) == first && glFrameAlloc(97, GLM_VertexData) == first_virtual && invalidations == 2,
        "Frame reuse did not reset offsets or invalidate caches");
    glShutdownMemory(); glShutdownMemory();
    Reject<std::logic_error>([] { glplatFrameAllocNextFrame(); });
    Require(!glGetResourcePools() && !glGetCurrentResourcePool() && !glGetTextureManager(), "Graphics shutdown left state behind");
}

void CheckInventory()
{
    glInitMemory(&config);
    auto* pool = glGetCurrentResourcePool();
    pool->m_inventory->SetModelReleaseCallback(ReleaseModel);
    {
        alignas(32) unsigned char small[64]{};
        MemoryAllocator empty{}; empty.Initialize(small, sizeof(small));
        void* held = empty.Allocate(sizeof(small) - alignof(FreeBlockList), 8, false);
        PlatTexture pending;
        const auto free_indices = glGetTextureManager()->mFreeIndices->mCount;
        {
            mscharged::ScopedGameAllocator allocator(empty);
            Reject<std::bad_alloc>([&] { glRegisterTexture(7, &pending, pool); });
        }
        Require(pending.m_TextureIndex == 0xFFFF && !glx_GetTex(7)
            && glGetTextureManager()->mFreeIndices->mCount == free_indices,
            "Inventory node allocation failure consumed a texture index");
        empty.Free(held);
    }
    const auto free = pool->GetFreeMemory();
    glModel* base = new (glResourceAlloc(sizeof(glModel), GLM_Header, pool)) glModel{};
    base->id = 42;
    pool->m_inventory->AddModel(42, base);
    Reject<std::invalid_argument>([&] { pool->m_inventory->AddModel(42, base); });
    auto* texture = new (glResourceAlloc(sizeof(PlatTexture), GLM_Header, pool)) PlatTexture;
    glRegisterTexture(42, texture, pool);
    glTextureBinding binding(42);
    Require(glGetTextureManager()->GetTexture(&binding) == texture && glx_GetTex(42) == texture, "Static texture resolution failed");
    const auto before = pool->GetFreeMemory();
    const auto mark = pool->MarkResource();
    Require(mark > UINT32_MAX, "GL resource marker was truncated");
    glModel* shadow = new (glResourceAlloc(sizeof(glModel), GLM_Header, pool)) glModel{};
    pool->m_inventory->AddModel(42, shadow);
    auto* shadow_texture = new (glResourceAlloc(sizeof(PlatTexture), GLM_Header, pool)) PlatTexture;
    glRegisterTexture(42, shadow_texture, pool);
    Require(pool->m_inventory->GetModel(42) == shadow && glx_GetTex(42) == shadow_texture
        && glGetTextureManager()->GetTexture(&binding) == shadow_texture, "Inventory shadowing failed");
    const auto inner = pool->MarkResource();
    auto* file = nlMalloc(1031, 32, false);
    pool->m_inventory->m_pFileData[2]->AddEnd(file);
    auto* nested = new (glResourceAlloc(sizeof(glModel), GLM_Header, pool)) glModel{};
    pool->m_inventory->AddModel(19, nested);
    pool->ReleaseResource(mark); // Releases both nested levels, including file storage.
    Require(pool->GetFreeMemory() == before && pool->m_inventory->GetModel(42) == base
        && !pool->m_inventory->GetModel(19) && glx_GetTex(42) == texture && model_releases == 2,
        "Nested inventory release did not restore resources");
    Require(glGetTextureManager()->GetTexture(&binding) == texture, "Cached texture binding did not restore the lower inventory level");
    Reject<std::invalid_argument>([&] { pool->ReleaseResource(inner); });
    Reject<std::invalid_argument>([&] { pool->ReleaseResource(mark); });
    Reject<std::invalid_argument>([&] { pool->ReleaseResource(1); });
    std::vector<GLResourceMark> marks;
    for (unsigned i = 0; i < 15; ++i) marks.push_back(pool->MarkResource());
    Reject<std::length_error>([&] { pool->MarkResource(); });
    pool->ReleaseResource(marks.front());
    Require(pool->GetFreeMemory() == before, "Marker depth failure consumed resource memory");
    auto* other = glCreateResourcePool(requirements, 2, "other");
    const auto other_mark = other->MarkResource();
    Reject<std::invalid_argument>([&] { pool->ReleaseResource(other_mark); });
    other->ReleaseResource(other_mark); glDestroyResourcePool(other);
    Reject<std::invalid_argument>([&] { glDestroyResourcePool(other); });
    Reject<std::invalid_argument>([&] { glSetCurrentResourcePool(other); });
    Require(pool->GetPeakMemoryUsage() > pool->GetTotalMemory() - before && before < free, "Resource peak accounting failed");
    glShutdownMemory();
    Require(model_releases == 3 && !glx_GetTex(42), "Final inventory shutdown failed");
}

void CheckTexturesAndAVL()
{
    glInitMemory(&config);
    auto* pool = glGetCurrentResourcePool();
    glTextureBinding binding(0x1001);
    std::vector<PlatTexture*> textures;
    const auto mark = pool->MarkResource();
    for (unsigned i = 0; i < 4; ++i)
    {
        auto* texture = new (glResourceAlloc(sizeof(PlatTexture), GLM_Header, pool)) PlatTexture;
        textures.push_back(texture);
        glRegisterTexture(0x1000 + i, texture, pool);
    }
    Require(glGetTextureManager()->GetTexture(&binding) == textures[1], "Texture binding failed");
    auto* extra = new (glResourceAlloc(sizeof(PlatTexture), GLM_Header, pool)) PlatTexture;
    Reject<std::length_error>([&] { glRegisterTexture(0x2000, extra, pool); });
    Require(extra->m_TextureIndex == 0xFFFF && !glx_GetTex(0x2000), "Texture capacity failure published a partial registration");
    Reject<std::invalid_argument>([&] { glRegisterTexture(0x2001, textures[0], pool); });
    unsigned long bad = 4;
    Reject<std::out_of_range>([&] { glGetTextureManager()->GetTextureAtIndex(&bad); });
    Reject<std::length_error>([&] { glGetTextureManager()->mFreeIndices->RemoveStart(); });
    Reject<std::logic_error>([] { glShutdownTextureManager(); });
    pool->ReleaseResource(mark);
    // A cached index must not resolve another texture after index recycling.
    Require(!glGetTextureManager()->GetTexture(&binding), "Released texture binding retained a stale index");
    const auto tree_mark = pool->MarkResource();
    std::vector<unsigned> order(1500);
    for (unsigned i = 0; i < order.size(); ++i) order[i] = i;
    std::shuffle(order.begin(), order.end(), std::mt19937(123));
    glModel model{};
    for (auto key : order) pool->m_inventory->AddModel(key, &model);
    for (auto key : order) Require(pool->m_inventory->GetModel(key) == &model, "Original AVL lookup lost a key");
    pool->ReleaseResource(tree_mark);
    Require(!pool->m_inventory->GetModel(500), "Original AVL release retained a key");
    glShutdownMemory();
    glTextureIndexQueue queue(static_cast<u16*>(nlMalloc(6, 8, false)), 3);
    auto* buffer = queue.mBuffer;
    for (unsigned pass = 0; pass < 20; ++pass)
    {
        queue.AddEnd(11); queue.AddEnd(22); queue.AddEnd(33);
        Reject<std::length_error>([&] { queue.AddEnd(44); });
        Require(queue.RemoveStart() == 11 && queue.RemoveStart() == 22 && queue.RemoveStart() == 33, "Texture index queue wrap is incorrect");
    }
    nlFree(buffer);
}

void CheckFailures()
{
    const auto standard = StandardAllocator.TotalFreeMemory(), virtual_free = VirtualAllocator.TotalFreeMemory();
    CurrentAllocator = &VirtualAllocator;
    GLMemoryConfig invalid = config;
    invalid.mFrameMemSize2 = ~0UL;
    Reject<std::length_error>([&] { glInitMemory(&invalid); });
    Require(CurrentAllocator == &VirtualAllocator, "Invalid budget changed current allocator");
    const GLMemoryRequirement invalid_requirements[] = {{GLM_Header, UINT32_MAX}, {GLM_TextureData, 1}};
    Reject<std::length_error>([&] { glCreateResourcePool(invalid_requirements, 2, "overflow"); });
    Reject<std::invalid_argument>([] { glCreateResourcePool(nullptr, -1, "bad"); });
    const GLMemoryRequirement big[] = {{GLM_Header, 100}, {GLM_VertexData, 32 * 1024 * 1024}};
    invalid = config; invalid.mResourceRequirements = big;
    Reject<std::bad_alloc>([&] { glInitMemory(&invalid); });
    Require(!glGetResourcePools() && !glGetTextureManager() && CurrentAllocator == &VirtualAllocator,
        "Partial resource-pool initialization did not restore global state/allocator");
    Require(StandardAllocator.TotalFreeMemory() == standard && VirtualAllocator.TotalFreeMemory() == virtual_free,
        "Failed resource-pool initialization leaked an arena allocation");
    invalid = config; invalid.mMaxTextures = 0;
    Reject<std::invalid_argument>([&] { glInitMemory(&invalid); });
    struct FailedObject { FailedObject() { throw std::runtime_error("constructor failure"); } };
    Reject<std::runtime_error>([] { new (8, false) FailedObject; });
    Require(VirtualAllocator.TotalFreeMemory() == virtual_free, "Matching placement delete did not reclaim failed construction");
    glInitMemory(&config);
    auto* pool = glGetCurrentResourcePool();
    const auto free = pool->GetFreeMemory(), peak = pool->GetPeakMemoryUsage();
    Reject<std::bad_alloc>([&] { pool->Allocate(~0UL, GLM_Header); });
    Require(pool->GetFreeMemory() == free && pool->GetPeakMemoryUsage() == peak, "Failed allocation changed resource usage");
    glShutdownMemory();
    Require(StandardAllocator.TotalFreeMemory() == standard && VirtualAllocator.TotalFreeMemory() == virtual_free,
        "Normal shutdown failed to recover arenas");
    CurrentAllocator = &StandardAllocator;
}
}

int main()
{
    try
    {
        { Arenas arenas; CheckFrames(); CheckInventory(); CheckTexturesAndAVL(); CheckFailures();
          // Exact original PreInitFS budgets include an unaligned MEM2 half.
          auto mem1 = StandardAllocator.TotalFreeMemory(), mem2 = VirtualAllocator.TotalFreeMemory();
          for (unsigned i = 0; i < 5; ++i) { InitializeOriginalGraphicsMemory(); glShutdownMemory(); }
          Require(StandardAllocator.TotalFreeMemory() == mem1 && VirtualAllocator.TotalFreeMemory() == mem2, "Original budget restart leaked memory"); }
        { Arenas arenas(65536, 64);
          auto free = StandardAllocator.TotalFreeMemory(); CurrentAllocator = &VirtualAllocator;
          Reject<std::bad_alloc>([] { glInitMemory(&config); });
          Require(StandardAllocator.TotalFreeMemory() == free && VirtualAllocator.TotalFreeMemory() == 64
              && CurrentAllocator == &VirtualAllocator, "Second frame arena allocation failure was not transactional"); }
        { Arenas arenas(1024, 65536);
          auto free = StandardAllocator.TotalFreeMemory();
          Reject<std::bad_alloc>([] { glCreateResourcePool(requirements, 2, "partial inventory"); });
          Require(StandardAllocator.TotalFreeMemory() == free, "Partial inventory construction leaked memory"); }
        std::cout << "Graphics memory: native addresses, aligned double buffers, arena ownership, transactional failures, nested static inventories, original AVL, texture indices and repeated shutdown passed\n";
        return 0;
    }
    catch (const std::exception& error) { std::cerr << "FAILED: " << error.what() << '\n'; return 1; }
}
