#include "runtime/particle_controller.h"
#include "runtime/particle_simulation_internal.h"
#include "Game/Effects/EmissionControllerSteps.h"
#include <algorithm>
#include <cmath>
#include <exception>
#include <limits>

namespace mscharged
{
namespace
{
void Validate(const ParticleEmitterFrame& f)
{
    for (const auto& vector : {f.position, f.direction, f.velocity})
        for (float value : vector)
            if (!std::isfinite(value) || std::abs(value) > 10000)
                throw std::invalid_argument("Invalid controller position, direction or velocity");
    const double length = double(f.direction[0])*f.direction[0] + double(f.direction[1])*f.direction[1] + double(f.direction[2])*f.direction[2];
    if (length < 1e-12 || !std::isfinite(f.time_scale) || f.time_scale < 0 || f.time_scale > 16)
        throw std::invalid_argument("Invalid controller direction or time scale");
}
}
struct ParticleControllers::Implementation
{
    struct System { std::shared_ptr<ParticleSimulation> simulation; bool alive = true, visible = false; };
    struct Controller
    {
        Token token; std::uint32_t group; std::uint16_t id;
        ParticleEmitterFrame frame;
        std::vector<System> systems;
        bool live = true, lingering = false, destroyed_callback = false;
        bool m_bDisabled = false, m_Replaying = false, m_bDying = false;
        float m_TimeScale = 1, m_ReplayDeltaTime = 0, m_Age = 0;
        nlVector3 m_vPosition{0,0,0};
        UpdateCallback update;
        FinishedCallback finished;
        std::function<void(Controller&)> mUpdateCallback;
        std::function<void(Controller&, int)> mFinishedCallback;
        void Sync()
        { m_bDisabled = frame.disabled; m_TimeScale = frame.time_scale; m_vPosition = {frame.position[0],frame.position[1],frame.position[2]}; }
        void DestroyCallback()
        {
            if (destroyed_callback) return;
            destroyed_callback = true;
            if (mFinishedCallback) mFinishedCallback(*this, 2);
        }
    };
    std::shared_ptr<ParticleSimulationContext> context;
    std::vector<std::unique_ptr<Controller>> controllers;
    std::thread::id thread = std::this_thread::get_id();
    unsigned max_controllers;
    int next_id = 1;
    Token next_token = 1;
    bool live = true, failed = false, busy = false, renderer = false, frame = false;
    explicit Implementation(ParticleControllersOptions o) : max_controllers(o.max_controllers)
    {
        if (!max_controllers || max_controllers > 64) throw std::invalid_argument("Invalid controller limit");
        context = std::make_shared<ParticleSimulationContext>(ParticleSimulationOptions{o.capacity,o.seed});
        controllers.reserve(max_controllers);
    }
    void Check(bool allow_failed = false) const
    {
        if (thread != std::this_thread::get_id() || !live || !gMemoryInitialized)
            throw std::logic_error("Particle controllers require their live owning thread");
        if (busy || frame) throw std::logic_error("Particle controller mutation is reentrant or a rendered frame is pending");
        if (failed && !allow_failed) throw std::logic_error("Particle controllers failed; reset or release them");
    }
    void Unbound() const
    { Check(true); if (renderer) throw std::logic_error("Release particle renderer before changing controller ownership"); }
    Controller& Find(Token token) const
    {
        for (const auto& c : controllers) if (c->live && c->token == token) return *c;
        throw std::invalid_argument("Unknown or completed particle controller token");
    }
    struct Busy
    {
        bool& value;
        explicit Busy(bool& b) : value(b) { value = true; }
        ~Busy() { value = false; }
    };
    void Clean()
    {
        std::exception_ptr error;
        {
            Busy guard(busy);
            for (auto& c : controllers)
                try { c->DestroyCallback(); } catch (...) { if (!error) error = std::current_exception(); }
            controllers.clear();
        }
        if (error) std::rethrow_exception(error);
    }
};
ParticleControllers::ParticleControllers(ParticleControllersOptions options) : impl_(std::make_unique<Implementation>(options)) {}
ParticleControllers::~ParticleControllers()
{
    if (!impl_->live) return;
    // User callbacks cannot prevent native resource cleanup during destruction.
    try { impl_->Unbound(); } catch (...) { std::terminate(); }
    try { impl_->Clean(); } catch (...) {}
    impl_->context.reset(); impl_->live = false;
}
bool ParticleControllers::Active() const
{
    if (impl_->thread != std::this_thread::get_id()) throw std::logic_error("Particle controller owner thread required");
    return impl_->live;
}
bool ParticleControllers::Failed() const { impl_->Check(true); return impl_->failed; }
std::uint32_t ParticleControllers::Seed() const { impl_->Check(true); return uSeed; }
ParticleControllers::Token ParticleControllers::Start(EffectsRegistry::Handle registry, std::uint32_t hash, ParticleControllerOptions options)
{
    impl_->Unbound(); impl_->Check();
    auto group = registry ? registry->FindGroup(hash) : nullptr;
    if (!group || !group->m_numSpecs || group->m_numSpecs > 64 || group->m_userSpecs)
        throw std::invalid_argument("Controller group is absent, empty, oversized or requires user effects");
    if (impl_->controllers.size() >= impl_->max_controllers || impl_->next_token == std::numeric_limits<Token>::max())
        throw std::length_error("Particle controller capacity exhausted");
    // The full manager's global lingering eviction is not selected. Up to twelve
    // qualify without invoking that branch; a thirteenth is explicitly rejected.
    if (group->m_bIsLingering && std::count_if(impl_->controllers.begin(),impl_->controllers.end(),[](const auto& c){return c->live&&c->lingering;}) >= 12)
        throw std::invalid_argument("Global lingering eviction is outside this controller profile");
    auto c = std::make_unique<Implementation::Controller>();
    c->token = impl_->next_token; c->group = hash; c->lingering = group->m_bIsLingering != 0;
    c->systems.reserve(group->m_numSpecs);
    Implementation::Busy guard(impl_->busy);
    for (std::size_t i=0;i<group->m_numSpecs;++i)
        c->systems.push_back({std::shared_ptr<ParticleSimulation>(new ParticleSimulation(registry,hash,i,impl_->context,0)),true});
    c->id = fxNextControllerId(impl_->next_id, options.id);
    const auto token = impl_->next_token++;
    if (options.add_to_end) impl_->controllers.push_back(std::move(c));
    else impl_->controllers.insert(impl_->controllers.begin(),std::move(c));
    return token;
}
void ParticleControllers::SetFrame(Token token, const ParticleEmitterFrame& frame)
{ impl_->Check(); Validate(frame); auto& c=impl_->Find(token); c.frame=frame; c.Sync(); }
void ParticleControllers::SetCallbacks(Token token, UpdateCallback update, FinishedCallback finished)
{
    impl_->Check(); auto& c=impl_->Find(token);
    // Prepare potentially allocating wrappers before changing the callback pair.
    std::function<void(Implementation::Controller&)> u;
    std::function<void(Implementation::Controller&,int)> f;
    if (update) u=[](auto& item){ auto frame=item.frame; item.update(frame); Validate(frame); item.frame=frame; item.Sync(); };
    if (finished) f=[](auto& item,int reason){item.finished(item.token,reason);};
    c.update=std::move(update);c.finished=std::move(finished);c.mUpdateCallback=std::move(u);c.mFinishedCallback=std::move(f);
}
bool ParticleControllers::Advance(float dt)
{
    impl_->Check();
    if (!std::isfinite(dt)||dt<0||dt>1) throw std::invalid_argument("Controller delta must be within [0,1]");
    // Validate every scaled step before any original controller is advanced.
    for (const auto& c:impl_->controllers) if(c->live && !c->frame.disabled && dt*c->frame.time_scale>1)
        throw std::invalid_argument("Scaled controller delta exceeds the qualified original simulation range");
    Implementation::Busy guard(impl_->busy);
    try
    {
        for(auto& c:impl_->controllers)
        {
            if(!c->live)continue;
            float step=dt;
            if(!fxBeginControllerUpdate(*c,step))continue;
            if(!std::isfinite(c->m_Age))throw std::overflow_error("Controller age overflow");
            int systems=0,removed=0;
            for(auto& s:c->systems) if(s.alive)
            {
                ++systems; s.simulation->ApplyFrame(c->frame); s.visible=c->frame.visible;
                if(!s.simulation->Advance(step)){s.alive=false;s.simulation->Release();++removed;}
            }
            if(fxControllerSystemsFinished(*c,systems,removed))
            { c->live=false;c->DestroyCallback(); }
        }
        // Retain inactive system objects while graphics owns their bindings.
        if(!impl_->renderer)std::erase_if(impl_->controllers,[](const auto& c){return !c->live;});
    }
    catch(...){impl_->failed=true;throw;}
    return std::any_of(impl_->controllers.begin(),impl_->controllers.end(),[](const auto& c){return c->live;});
}
void ParticleControllers::Stop(Token token)
{
    impl_->Check(); auto& c=impl_->Find(token);Implementation::Busy guard(impl_->busy);
    try
    {
        if(!fxBeginControllerDie(c.m_bDying))return;
        for(auto& s:c.systems)if(s.alive)s.simulation->Die();
        if(c.mFinishedCallback)c.mFinishedCallback(c,0);
    }
    catch(...){impl_->failed=true;throw;}
}
void ParticleControllers::Clear(Token token)
{
    impl_->Check();auto& c=impl_->Find(token);Implementation::Busy guard(impl_->busy);
    try
    {for(auto& s:c.systems)if(s.alive)s.simulation->ClearParticles();if(c.mFinishedCallback)c.mFinishedCallback(c,0);}
    catch(...){impl_->failed=true;throw;}
}
void ParticleControllers::Destroy(Token token)
{
    impl_->Unbound();auto& c=impl_->Find(token);std::exception_ptr error;
    {Implementation::Busy guard(impl_->busy);c.live=false;try{c.DestroyCallback();}catch(...){error=std::current_exception();}
     std::erase_if(impl_->controllers,[](const auto& item){return !item->live;});}
    if(error){impl_->failed=true;std::rethrow_exception(error);}
}
void ParticleControllers::Reset(std::uint32_t seed)
{
    impl_->Unbound();std::exception_ptr error;try{impl_->Clean();}catch(...){error=std::current_exception();}
    uSeed=seed;impl_->next_id=1;impl_->failed=false;
    if(error)std::rethrow_exception(error);
}
void ParticleControllers::Release()
{
    if(!Active())return;impl_->Unbound();std::exception_ptr error;
    try{impl_->Clean();}catch(...){error=std::current_exception();}
    impl_->context.reset();impl_->live=false;
    if(error)std::rethrow_exception(error);
}
std::vector<ParticleControllerSnapshot> ParticleControllers::Snapshot() const
{
    impl_->Check();std::vector<ParticleControllerSnapshot> result;
    for(const auto& c:impl_->controllers)if(c->live)
    {
        unsigned systems=0,particles=0;for(const auto& s:c->systems)if(s.alive){++systems;particles+=s.simulation->Snapshot().size();}
        result.push_back({c->token,c->group,c->id,c->m_Age,c->m_bDying,c->lingering,systems,particles,c->frame});
    }
    return result;
}
std::vector<ParticleSnapshot> ParticleControllers::Particles(Token token,std::size_t spec) const
{
    impl_->Check();const auto& c=impl_->Find(token);
    if(spec>=c.systems.size())throw std::out_of_range("Controller spec index outside retained group");
    return c.systems[spec].alive ? c.systems[spec].simulation->Snapshot() : std::vector<ParticleSnapshot>{};
}
std::vector<ParticleControllers::RenderEntry> ParticleControllers::RetainRenderers()
{
    impl_->Check();if(impl_->renderer)throw std::logic_error("Particle controller renderer already retained");
    std::vector<RenderEntry> result;
    for(const auto& c:impl_->controllers)for(const auto& s:c->systems)if(c->live&&s.alive)
        result.push_back({s.simulation,c->token,c->live&&s.alive&&s.visible&&!c->frame.disabled});
    impl_->renderer=true;return result;
}
std::vector<ParticleControllers::RenderEntry> ParticleControllers::RenderEntries() const
{
    impl_->Check();if(!impl_->renderer)throw std::logic_error("No retained controller renderer");
    std::vector<RenderEntry> result;
    for(const auto& c:impl_->controllers)for(const auto& s:c->systems)if(c->live&&s.alive)
        result.push_back({s.simulation,c->token,c->live&&s.alive&&s.visible&&!c->frame.disabled});
    return result;
}
void ParticleControllers::BeginRenderDrain(){impl_->Check(true);if(!impl_->renderer)throw std::logic_error("No controller renderer");impl_->frame=true;}
void ParticleControllers::BeginRenderFrame(){impl_->Check();if(!impl_->renderer)throw std::logic_error("No controller renderer");impl_->frame=true;}
void ParticleControllers::FailRenderFrame()
{
    if(impl_->thread!=std::this_thread::get_id()||!impl_->frame)throw std::logic_error("No controller render frame to fail");
    impl_->failed=true;
}
void ParticleControllers::EndRenderFrame()
{
    if(impl_->thread!=std::this_thread::get_id()||impl_->busy)throw std::logic_error("Controller render owner thread required");
    impl_->frame=false;
}
void ParticleControllers::ReleaseRenderers()
{
    impl_->Check(true);impl_->renderer=false;std::erase_if(impl_->controllers,[](const auto& c){return !c->live;});
}
}
