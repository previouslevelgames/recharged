#include "runtime/particle_simulation_internal.h"
#include "runtime/graphics_memory.h"
#include "Game/Effects/ParticleSystem.h"
#include <algorithm>
#include <cmath>
#include <mutex>
#include <thread>

namespace mscharged
{
namespace
{
std::mutex ownership;
const void* owner = nullptr;
void Bound(float value, float limit, const char* field)
{
    if (!std::isfinite(value) || std::abs(value) > limit)
        throw std::invalid_argument(std::string("Unqualified particle ") + field);
}
void Range(const fxRange& range)
{ Bound(range.base, 10000, "range base"); Bound(range.range, 10000, "range width"); }
void Qualify(const EffectsSpec& spec, const EffectsTemplate& value)
{
    if (spec.m_eAttach != FXBind_Emitter || spec.m_eJointBinding != JB_Normal
        || spec.m_bGround || spec.m_bLight || spec.m_uTerrainID || spec.m_nForwardAxis != 0)
        throw std::invalid_argument("Particle spec requires an unavailable attachment, terrain or light service");
    if (value.m_uModelID != 0xffffffff || value.m_eBillboard != EfBill_Billboard
        || value.DisablesDepthWrite() || (value.m_uFlags & ~3) || value.m_eEmitter > Emitter_Disc
        || value.m_eBlend > EfBlend_Additive || value.m_uEmitterDeathCode || value.m_uParticleCreationCode || value.m_uParticleDeathCode)
        throw std::invalid_argument("Particle template requires an unqualified model, billboard, flag or event service");
    if (value.m_nFrames != 1 && value.m_nFrames != 4 && value.m_nFrames != 9
        && value.m_nFrames != 16 && value.m_nFrames != 25 && value.m_nFrames != 36)
        throw std::invalid_argument("Particle atlas must contain 1, 4, 9, 16, 25 or 36 frames");
    if (value.m_cColour.size() < 25)
        throw std::invalid_argument("Particle colour channel requires at least 25 authored samples");
    Bound(value.m_fFountainLife, 1e10f, "fountain lifetime");
    for (auto range : {value.m_rMass, value.m_rParticleLife, value.m_rInheritVelocity,
            value.m_rAcceleration, value.m_rRotation, value.m_rFPS}) Range(range);
    if (value.m_rParticleLife.base - .5f * std::abs(value.m_rParticleLife.range) < .000001f
        || value.m_rParticleLife.base + .5f * std::abs(value.m_rParticleLife.range) > 1000.f)
        throw std::invalid_argument("Particle lifetime range is outside the qualified positive interval");
    Bound(value.m_fTexcoordFlipPercentage, 10000, "UV flip threshold");
    for (const auto* property : value.mProperties)
    {
        if (!property) throw std::invalid_argument("Missing particle property");
        if (!property->mUseCurve) Range({property->base, property->range});
        else
        {
            if (!property->mNumKeys || !property->mKeys || property->mKeys[0].mTime != 0)
                throw std::invalid_argument("Particle curve does not cover time zero");
            for (unsigned long i = 0; i < property->mNumKeys; ++i)
            {
                const auto& key = property->mKeys[i];
                for (float coefficient : {key.mCubic, key.mQuadratic, key.mLinear, key.mConstant})
                    Bound(coefficient, 10000, "curve coefficient");
            }
        }
    }
    Bound(spec.m_fDelay, 1000, "delay"); Bound(spec.m_fOffset, 10000, "offset");
    for (float v : {spec.m_vLocalOffset.x, spec.m_vLocalOffset.y, spec.m_vLocalOffset.z}) Bound(v, 10000, "local offset");
    Bound(spec.m_fLingerStart, 1e10f, "linger start"); Bound(spec.m_fLingerEnd, 1e10f, "linger end");
    if (spec.m_fDelay < 0 || (spec.m_fLingerEnd >= 0
        && (spec.m_fLingerStart < 0 || spec.m_fLingerStart > spec.m_fLingerEnd)))
        throw std::invalid_argument("Particle delay or linger interval is outside the qualified profile");
}
std::array<float, 3> Vector(const nlVector3& v) { return {v.x, v.y, v.z}; }
}
ParticleSimulationContext::ParticleSimulationContext(ParticleSimulationOptions options) : capacity(options.capacity)
{
    if (!capacity || capacity > 16384) throw std::invalid_argument("Invalid shared particle capacity");
    {
        const std::lock_guard lock(ownership);
        if (owner || !gMemoryInitialized) throw std::logic_error("Particle simulation requires initialized, unowned game memory");
        owner = this; live = true; saved_seed = gEffectsRandomSeed;
    }
    try
    {
        ScopedGameAllocator arena(VirtualAllocator);
        fxParticleStartup(int(capacity)); atlas = true;
        free.m_Allocator.Initialize(capacity, 0);
        particles = static_cast<Particle*>(nlMalloc(capacity * sizeof(Particle), alignof(Particle), false));
        if (!particles) throw std::bad_alloc();
        std::uninitialized_value_construct_n(particles, capacity);
        for (unsigned i = 0; i < capacity; ++i) free.AddStart(particles + i);
        gEffectsRandomSeed = options.seed;
    }
    catch (...)
    {
        free.Clear(); free.m_Allocator.FreeBlocks();
        if (particles) { std::destroy_n(particles, capacity); nlFree(particles); particles = nullptr; }
        if (atlas) fxParticleShutdown();
        gEffectsRandomSeed = saved_seed;
        const std::lock_guard lock(ownership); owner = nullptr; live = false;
        throw;
    }
}
void ParticleSimulationContext::Check() const
{
    if (!live || !gMemoryInitialized || thread != std::this_thread::get_id())
        throw std::logic_error("Particle storage requires its live owner thread and arenas");
}
ParticleSimulationContext::~ParticleSimulationContext()
{
    if (!live) return;
    try
    {
        Check(); free.Clear(); free.m_Allocator.FreeBlocks();
        std::destroy_n(particles, capacity); nlFree(particles);
        if (atlas) fxParticleShutdown();
        gEffectsRandomSeed = saved_seed;
        const std::lock_guard lock(ownership); owner = nullptr; live = false;
    }
    catch (...) { std::terminate(); }
}
struct ParticleSimulation::Implementation
{
    EffectsRegistry::Handle registry;
    std::shared_ptr<const EffectsGroup> group;
    std::shared_ptr<const resources::Texture> texture;
    EffectsSpec spec{}; // Copy only this record; its immutable template stays retained.
    std::shared_ptr<ParticleSimulationContext> context;
    unsigned capacity;
    std::thread::id thread = std::this_thread::get_id();
    bool live = false, failed = false;
    std::uint32_t seed = 0;
    std::unique_ptr<ParticleSystem> system;
    Implementation(EffectsRegistry::Handle source, std::uint32_t hash, std::size_t index, std::shared_ptr<ParticleSimulationContext> storage)
        : registry(std::move(source)), context(std::move(storage)), capacity(context->capacity)
    {
        if (!registry || capacity == 0 || capacity > 16384) throw std::invalid_argument("Invalid particle registry or capacity");
        group = registry->FindGroup(hash);
        if (!group || index >= group->m_numSpecs || group->m_userSpecs)
            throw std::invalid_argument("Particle group/spec is unavailable");
        spec = group->m_specs[index];
        if (!spec.m_pTemplate) throw std::invalid_argument("Particle template is unresolved");
        Qualify(spec, *spec.m_pTemplate);
        texture = registry->FindTexture(spec.m_pTemplate->m_hTexture);
        if (!texture) throw std::invalid_argument("Particle template has no retained authored texture");
        context->Check(); live = true;
        try { ScopedGameAllocator arena(VirtualAllocator); Construct(gEffectsRandomSeed); }
        catch (...) { Cleanup(); throw; }
    }
    void Check(bool allow_failed = false) const
    {
        if (thread != std::this_thread::get_id()) throw std::logic_error("Particle simulation requires its owning thread");
        if (!live || !gMemoryInitialized) throw std::logic_error("Particle simulation is released or game memory is unavailable");
        if (failed && !allow_failed) throw std::logic_error("Particle simulation failed; reset or release it");
    }
    void Construct(std::uint32_t initial_seed)
    {
        // These are the selected emitter-only fields applied by the original
        // EmissionController default frame and fxUpdateParticleSystem. No actor,
        // pose, terrain or renderer service is substituted.
        system = std::make_unique<ParticleSystem>(ParticleSystem::NativeSimulation{}, spec.m_pTemplate, &context->free, &spec, group->m_hashID);
        system->m_Particles.m_Allocator.Initialize(capacity, 0);
        system->m_fDelay = spec.m_fDelay; system->m_uLayer = spec.m_uLayer;
        nlVec3Set(system->m_vForward, 0, 0, 1);
        nlVec3Set(system->m_vPosition, 0, 0, spec.m_fOffset);
        system->UpdateCoordSys();
        gEffectsRandomSeed = initial_seed; seed = initial_seed; failed = false;
    }
    void Cleanup()
    {
        if (!live) return;
        Check(true);
        system.reset(); // Original destructor returns live nodes to free first.
        live = false; context.reset();
    }
    ~Implementation() { try { Cleanup(); } catch (...) { std::terminate(); } }
};
ParticleSimulation::ParticleSimulation(EffectsRegistry::Handle registry, std::uint32_t group, std::size_t spec, ParticleSimulationOptions options)
    : impl_(std::make_unique<Implementation>(std::move(registry), group, spec, std::make_shared<ParticleSimulationContext>(options))) {}
ParticleSimulation::ParticleSimulation(EffectsRegistry::Handle registry, std::uint32_t group, std::size_t spec,
    std::shared_ptr<ParticleSimulationContext> context, int)
    : impl_(std::make_unique<Implementation>(std::move(registry), group, spec, std::move(context))) {}
void ParticleSimulation::ApplyFrame(const ParticleEmitterFrame& frame)
{
    impl_->Check();
    // Qualified emitter branch of original fxUpdateParticleSystem and
    // ComputePositionAndVelocity; pose, terrain and ground branches reject.
    auto& system = *impl_->system;
    system.m_aFacing = frame.facing; system.m_uLayer = impl_->spec.m_uLayer;
    system.m_vPosition = {frame.position[0], frame.position[1], frame.position[2] + impl_->spec.m_fOffset};
    system.m_vVelocity = {frame.velocity[0], frame.velocity[1], frame.velocity[2]};
    system.m_vForward = {frame.direction[0], frame.direction[1], frame.direction[2]};
    system.UpdateCoordSys(); system.m_bVisible = frame.visible;
}
void ParticleSimulation::ClearParticles() { impl_->Check(true); if (impl_->system) impl_->system->ClearParticles(); }
ParticleSimulation::~ParticleSimulation() = default;
bool ParticleSimulation::Active() const
{
    if (impl_->thread != std::this_thread::get_id()) throw std::logic_error("Particle simulation requires its owning thread");
    return impl_->live;
}
bool ParticleSimulation::Failed() const { impl_->Check(true); return impl_->failed; }
void ParticleSimulation::Release() { if (Active()) impl_->Cleanup(); }
float ParticleSimulation::Elapsed() const { impl_->Check(); return impl_->system->m_fElapsedTime; }
std::uint32_t ParticleSimulation::Seed() const { impl_->Check(true); return impl_->seed; }
std::shared_ptr<const resources::Texture> ParticleSimulation::Texture() const { impl_->Check(); return impl_->texture; }
ParticleRenderProfile ParticleSimulation::RenderProfile() const
{
    impl_->Check();
    return {static_cast<std::uint32_t>(impl_->spec.m_uLayer), impl_->spec.m_pTemplate->m_eBlend,
        impl_->spec.m_pTemplate->IsInFront() || impl_->spec.m_bInFront != 0};
}
bool ParticleSimulation::Advance(float dt)
{
    impl_->Check();
    if (!std::isfinite(dt) || dt < 0 || dt > 1) throw std::invalid_argument("Particle delta must be finite and within [0, 1]");
    try
    {
        const bool alive = impl_->system->Update(dt);
        impl_->seed = gEffectsRandomSeed;
        // The qualified coefficient/lifetime bounds prevent original overflow;
        // also check published state before clients can consume it.
        for (const auto& p : Snapshot())
            for (float v : {p.elapsed, p.fraction, p.lifespan, p.rotation, p.velocity, p.position[0], p.position[1], p.position[2]})
                if (!std::isfinite(v)) throw std::domain_error("Original particle simulation produced nonfinite state");
        return alive;
    }
    catch (...) { impl_->seed = gEffectsRandomSeed; impl_->failed = true; throw; }
}
void ParticleSimulation::Die() { impl_->Check(); impl_->system->Die(); }
void ParticleSimulation::Reset(std::uint32_t seed)
{
    impl_->Check(true);
    impl_->system.reset();
    try { ScopedGameAllocator arena(VirtualAllocator); impl_->Construct(seed); }
    catch (...) { impl_->failed = true; throw; }
}
std::vector<ParticleSnapshot> ParticleSimulation::Snapshot() const
{
    impl_->Check(); std::vector<ParticleSnapshot> result;
    result.reserve(impl_->system->m_NumParticles);
    auto it = impl_->system->m_Particles.Begin();
    while (it.hasNext())
    {
        const auto* p = *it; it.Step();
        result.push_back({p->timeElapsed, p->timeFraction, p->lifeSpan, p->rot, p->dRot, p->size, p->sizeScale,
            p->velocity, p->acceleration, p->mass, p->frame, p->FPS, Vector(p->initialPosition), Vector(p->position), Vector(p->velDir), p->flipTexcoords});
    }
    return result;
}
std::vector<ParticleQuad> ParticleSimulation::Sample(std::array<float, 3> right, std::array<float, 3> up)
{
    impl_->Check();
    for (float value : right) Bound(value, 16, "view right");
    for (float value : up) Bound(value, 1, "view up");
    const nlVector3 r{right[0], right[1], right[2]}, u{up[0], up[1], up[2]};
    std::vector<ParticleQuad> result; result.reserve(impl_->system->m_NumParticles);
    try
    {
        auto it = impl_->system->m_Particles.Begin();
        while (it.hasNext())
        {
            ParticleReturn value{}; auto* particle = *it; it.Step();
            impl_->system->UpdateParticle(&value, particle, impl_->spec.m_pTemplate, r, u,
                impl_->spec.m_pTemplate->IsLocalSpace() ? &impl_->system->m_mCoordSys : nullptr);
            ParticleQuad output{};
            for (unsigned i = 0; i < 4; ++i)
            {
                output.position[i] = Vector(value.position[i]);
                output.uv[i] = {value.texcoord[i].x, value.texcoord[i].y}; output.colour[i] = value.c.c[i];
                for (float v : output.position[i]) if (!std::isfinite(v)) throw std::domain_error("Original particle quad is nonfinite");
            }
            result.push_back(output);
        }
    }
    catch (...) { impl_->failed = true; throw; }
    return result;
}
}
