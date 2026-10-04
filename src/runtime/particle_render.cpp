#include "runtime/particle_render.h"
#include "runtime/views.h"
#include "Game/Effects/ParticleBillboard.h"
#include "Game/GL/GLInventory.h"
#include "NL/gl/gl.h"
#include "NL/gl/glMemory.h"
#include "NL/gl/glTexture.h"
#include "NL/gl/glTextureManager.h"
#include "NL/glx/glxTexture.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <optional>
#include <thread>

namespace mscharged
{
namespace
{
bool Linked(const GLResourcePool* pool)
{
    auto* first = glGetResourcePools();
    if (!first) return false;
    auto* item = first;
    do { if (item == pool) return true; item = item->m_next; } while (item != first);
    return false;
}
}
struct ParticleRenderer::Implementation
{
    GLResourcePool& pool;
    ParticleSimulation& simulation;
    std::shared_ptr<const resources::Texture> texture;
    void (*drain)();
    std::thread::id thread = std::this_thread::get_id();
    GLResourceMark mark = 0;
    int level = 0;
    PlatTexture* registered = nullptr;
    std::uint16_t index = 0xffff;
    std::optional<std::uint64_t> pending;
    bool busy = false;
    Implementation(GLResourcePool& p, ParticleSimulation& s, void (*d)()) : pool(p), simulation(s), drain(d)
    {
        if (!drain || !Linked(&pool) || !glGetTextureManager() || glNativeViewDispatchActive() || glIsFrameActive())
            throw std::logic_error("Particle renderer requires an idle initialized graphics pool and real drain callback");
        texture = simulation.Texture(); // Qualified immutable decoder ownership.
        if (!texture || texture->id == 0xffffffffU || glGetTextureIndex(texture->id) != 0xffff)
            throw std::invalid_argument("Particle texture is missing, requires global-white fallback, or its hash is already registered");
        mark = pool.MarkResource(); level = pool.m_level;
        try
        {
            auto* memory = pool.Allocate(sizeof(PlatTexture), GLM_Header);
            registered = new (memory) PlatTexture;
            registered->m_Width = texture->width; registered->m_Height = texture->height;
            registered->m_Levels = registered->m_MaxLevel = texture->levels;
            registered->m_Format = static_cast<eGXTextureFormat>(texture->game_format);
            registered->m_nPaletteEntries = texture->palette_entries;
            std::copy(texture->bits.begin(), texture->bits.end(), registered->m_Bits);
            // GL texture binding only reads these retained original tiled bytes.
            // Release drains GPU references before either owner is discarded.
            registered->m_SwizzledData = const_cast<std::uint8_t*>(texture->pixels.data());
            registered->m_PaletteData = texture->palette.empty() ? nullptr
                : reinterpret_cast<u16*>(const_cast<std::uint8_t*>(texture->palette.data()));
            registered->m_NativeDataBytes = texture->pixels.size();
            registered->m_NativePaletteBytes = texture->palette.size();
            glRegisterTexture(texture->id, registered, &pool);
            index = registered->m_TextureIndex;
            if (index == 0xffff || index >= glGetTextureManager()->mCapacity
                || glGetTextureManager()->mTextures[index] != registered || glGetTextureIndex(texture->id) != index)
                throw std::logic_error("Particle texture registration did not establish a real binding");
        }
        catch (...) { pool.ReleaseResource(mark); mark = 0; throw; }
    }
    void CheckThread() const
    {
        if (thread != std::this_thread::get_id() || busy)
            throw std::logic_error("Particle renderer requires its nonrecursive owner thread");
    }
    void CheckStorage() const
    {
        if (!mark || !Linked(&pool) || !glGetTextureManager() || pool.m_level < level
            || pool.m_inventory->GetTexture(texture->id) != registered
            || index >= glGetTextureManager()->mCapacity || glGetTextureManager()->mTextures[index] != registered
            || glGetTextureIndex(texture->id) != index)
            throw std::logic_error("Particle texture ownership is inactive or was replaced");
        if (glNativeViewDispatchActive()) throw std::logic_error("Particle renderer cannot mutate a dispatching view");
    }
    void Check() const { CheckThread(); CheckStorage(); }
    void Release()
    {
        CheckThread();
        if (!mark) return;
        Check();
        if (pending || glIsFrameActive() || pool.m_level != level)
            throw std::logic_error("Finish particle frames and nested pool owners before release");
        busy = true;
        try { drain(); pool.ReleaseResource(mark); mark = 0; registered = nullptr; index = 0xffff; busy = false; }
        catch (...) { busy = false; throw; }
    }
    ~Implementation() { try { Release(); } catch (...) { std::terminate(); } }
};
ParticleRenderer::ParticleRenderer(GLResourcePool& pool, ParticleSimulation& simulation, void (*drain)())
    : impl_(std::make_unique<Implementation>(pool, simulation, drain)) {}
ParticleRenderer::~ParticleRenderer() = default;
bool ParticleRenderer::Active() const { impl_->CheckThread(); return impl_->mark != 0; }
std::uint16_t ParticleRenderer::TextureIndex() const { impl_->Check(); return impl_->index; }
void ParticleRenderer::Release() { impl_->Release(); }
void ParticleRenderer::FinishFrame()
{
    impl_->Check();
    if (!impl_->pending) return;
    if (glIsFrameActive() || *impl_->pending == glNativeFrameGeneration())
        throw std::logic_error("Particle frame must be sent or cancelled before finishing");
    impl_->busy = true;
    try { impl_->drain(); impl_->pending.reset(); impl_->busy = false; }
    catch (...) { impl_->busy = false; throw; }
}
unsigned ParticleRenderer::Submit(GLView& view, bool visible, float aspect, bool allow_in_front)
{
    impl_->Check();
    if (!glIsFrameActive() || impl_->pending || !view.m_Interface || view.m_NativeIterating)
        throw std::logic_error("Particle submission requires one collecting original frame");
    if (!std::isfinite(aspect) || aspect <= 0 || aspect > 16)
        throw std::invalid_argument("Invalid particle billboard aspect");
    if (!visible) return 0; // Original visibility early return precedes sampling.
    const auto profile = impl_->simulation.RenderProfile();
    if (profile.layer > 0x7fffffffU)
        throw std::invalid_argument("Particle layer exceeds the qualified signed sort range");
    impl_->busy = true;
    struct Leave { bool& busy; ~Leave() { busy = false; } } leave{impl_->busy};
    const auto generation = glNativeFrameGeneration();
    nlMatrix4 matrix; view.m_Interface->GetViewMatrix(matrix);
    impl_->CheckStorage();
    if (!glIsFrameActive() || generation != glNativeFrameGeneration())
        throw std::logic_error("Particle camera callback changed the original frame");
    for (float value : matrix.e)
        if (!std::isfinite(value)) throw std::invalid_argument("Nonfinite particle view matrix");
    nlVector3 right, up; matrix.GetColumn_(0, right); matrix.GetColumn_(1, up);
    // Original RenderAllParticles scales its horizontal camera basis by aspect
    // before running UpdateParticle. The qualified right-vector bound is 16.
    nlVec3Scale(right, aspect);
    auto quads = impl_->simulation.Sample({right.x,right.y,right.z}, {up.x,up.y,up.z});
    if (quads.empty()) return 0;
    if (quads.size() > 65535 / 4)
        throw std::length_error("Particle mesh exceeds the original GX vertex limit");
    // Original glHasQuads returns true on the Wii path. Both stream orders are
    // shared in the source helper; this qualified GX path uses its quad branch.
    impl_->pending = glNativeFrameGeneration();
    fxSetParticleRasterState(true, false, allow_in_front && profile.in_front, false, profile.blend);
    GLTexturedColourMeshWriter mesh;
    mesh.Begin(static_cast<int>(quads.size() * 4), GLP_QuadList, nullptr);
    for (const auto& quad : quads)
    {
        ParticleReturn value{};
        for (unsigned i=0;i<4;++i)
        {
            value.position[i]={quad.position[i][0],quad.position[i][1],quad.position[i][2]};
            value.texcoord[i]={quad.uv[i][0],quad.uv[i][1]};value.c.c[i]=quad.colour[i];
        }
        fxWriteParticleQuad(mesh,value,true);
    }
    auto* binding=static_cast<glTextureBinding*>(mesh.GetModel()->packets->materialParameters);
    // Original binding sets its resolved index. The native manager also resolves
    // the authored hash on use, so retain both genuine identities.
    binding->texture=impl_->texture->id;binding->textureIndex=impl_->index;
    binding->SetWrapS(false);binding->SetWrapT(false);binding->unknown07=0;
    mesh.End();view.AttachModel(mesh.GetModel(),profile.layer);
    return static_cast<unsigned>(quads.size());
}
}
