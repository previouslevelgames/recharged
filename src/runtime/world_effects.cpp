#include "runtime/world_effects.h"
#include "Game/World/WorldEffectSteps.h"
#include "Game/Render/Frustum.h"
#include "Game/Effects/EffectsGroup.h"
#include "Game/Effects/EffectsTemplate.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <thread>
namespace mscharged
{
namespace
{
struct BusyGuard{bool& state;explicit BusyGuard(bool& s):state(s){state=true;}~BusyGuard(){state=false;}};
std::uint64_t next_identity=1;
void ValidateView(const std::array<std::array<float,4>,6>& planes)
{
    for(const auto& p:planes)
    {
        for(float x:p)if(!std::isfinite(x)||std::abs(x)>1e7f)throw std::invalid_argument("Invalid world-effect frustum");
        const double length=double(p[0])*p[0]+double(p[1])*p[1]+double(p[2])*p[2];
        if(std::abs(length-1)>1e-4)throw std::invalid_argument("World-effect frustum planes must be normalized");
    }
}
ParticleEmitterFrame ControllerFrame(const WorldEffectFrame& source,float radius)
{
    ParticleEmitterFrame frame;frame.position={source.matrix[12],source.matrix[13],source.matrix[14]};
    frame.direction={source.matrix[8],source.matrix[9],source.matrix[10]};
    for(const auto& v:{frame.position,frame.direction})for(float x:v)
        if(!std::isfinite(x)||std::abs(x)>10000)throw std::invalid_argument("World effect exceeds the controller coordinate profile");
    if(!source.active)frame.visible=false;
    else if(source.always_visible)frame.visible=true;
    else
    {
        if(!source.frustum)throw std::logic_error("World-effect visibility requires the actual camera frustum");
        ValidateView(*source.frustum);std::array<nlVector4,6> planes;
        for(unsigned i=0;i<6;++i){const auto& p=(*source.frustum)[i];planes[i]={p[0],p[1],p[2],p[3]};}
        const nlVector3 position{frame.position[0],frame.position[1],frame.position[2]};
        frame.visible=ClassifySphereInFrustum(planes.data(),&position,radius)!=FRUSTUM_OUTSIDE&&source.rendering_enabled;
    }
    return frame;
}
}
struct WorldEffects::Implementation
{
    struct Effect
    {
        resources::WorldEffectRecord record;
        float m_fEmissionInterval,m_fEmissionTime=0,m_fPreviousEmissionTime=0,radius=0;
        std::int32_t m_nTimingMode,m_nEmissionCount,m_nRemainingEmissions=0,emission_id=-1;
        std::uint32_t m_uProbability;
        bool m_bActive=true;
        explicit Effect(const resources::WorldEffectRecord& r):record(r),m_fEmissionInterval(r.interval),m_nTimingMode(r.timing),m_nEmissionCount(r.count),m_uProbability(r.probability)
        {WorldEffectInitializeClock(*this);}
    };
    resources::WorldEffectData::Handle data;std::shared_ptr<WorldEffectEmitter> emitter;
    std::vector<Effect> effects;std::optional<std::array<std::array<float,4>,6>> planes;
    std::thread::id thread=std::this_thread::get_id();std::uint64_t identity;
    bool rendering=true,live=false,busy=false,failed=false;
    Implementation(resources::WorldEffectData::Handle d,std::shared_ptr<WorldEffectEmitter> e):data(std::move(d)),emitter(std::move(e))
    {
        if(!data)throw std::invalid_argument("World effects require retained decoded world records");
        if(next_identity==std::numeric_limits<std::uint64_t>::max())throw std::logic_error("World-effect owner identity exhausted");
        effects.reserve(data->Records().size());for(auto it=data->Records().rbegin();it!=data->Records().rend();++it)effects.emplace_back(*it);
        identity=next_identity++;live=true;
    }
    void Check(bool allow_failed=false)const
    {
        if(thread!=std::this_thread::get_id()||!live||busy)throw std::logic_error("World effects require their idle live owner thread");
        if(failed&&!allow_failed)throw std::logic_error("World-effect update failed; release its retained services");
    }
    WorldEffectFrame Frame(std::size_t index)const
    {Check(true);const auto& e=effects.at(index);return{e.record.matrix,e.m_bActive,e.record.always_visible,rendering,planes};}
    struct Busy{bool& state;explicit Busy(bool& s):state(s){state=true;}~Busy(){state=false;}};
    void Clean()
    {
        if(!live)return;Check(true);Busy guard(busy);if(emitter)emitter->Release(identity);
        // Guard destruction of external service/callback storage as well.
        emitter.reset();effects.clear();data.reset();live=false;
    }
    ~Implementation(){try{Clean();}catch(...){std::terminate();}}
};
WorldEffects::WorldEffects(resources::WorldEffectData::Handle data,std::shared_ptr<WorldEffectEmitter> service)
    :impl_(std::make_shared<Implementation>(std::move(data),std::move(service))){}
WorldEffects::~WorldEffects()=default;
std::size_t WorldEffects::Trigger(std::uint32_t type)
{impl_->Check();std::size_t matches=0;for(auto& e:impl_->effects)matches+=WorldEffectTriggerClock(e,type);return matches;}
void WorldEffects::Reset(){impl_->Check();for(auto& e:impl_->effects)WorldEffectResetClock(e);}
void WorldEffects::SetActive(std::uint32_t id,bool active)
{
    impl_->Check();for(auto& e:impl_->effects)if(e.record.id==id){e.m_bActive=active;return;}throw std::out_of_range("World-effect object is absent");
}
void WorldEffects::SetView(const std::array<std::array<float,4>,6>& planes,bool enabled)
{impl_->Check();ValidateView(planes);impl_->planes=planes;impl_->rendering=enabled;}
unsigned WorldEffects::Update(float dt)
{
    impl_->Check();if(!std::isfinite(dt)||dt<0||dt>1)throw std::invalid_argument("World-effect delta must be within [0,1]");
    if(!impl_->emitter)throw std::logic_error("World-effect emission service is unavailable");
    Implementation::Busy guard(impl_->busy);unsigned emitted=0;
    try
    {
        for(std::size_t i=0;i<impl_->effects.size();++i)
        {
            auto& e=impl_->effects[i];const bool emit=WorldEffectUpdateClock(e,dt,&nlDefaultSeed);
            if(!std::isfinite(e.m_fEmissionTime))throw std::overflow_error("World-effect emission time overflow");
            if(!emit)continue;
            std::weak_ptr<Implementation> weak=impl_;
            WorldEffectRequest request{impl_->identity,e.record.id,e.record.group,e.record.animated,
                {e.record.matrix,e.m_bActive,e.record.always_visible,impl_->rendering,impl_->planes},
                [weak,i]{auto state=weak.lock();if(!state)throw std::logic_error("World-effect callback owner expired");return state->Frame(i);}};
            const auto result=impl_->emitter->Emit(request);
            if(result.id < -1||result.id>65535||!std::isfinite(result.radius)||result.radius<0||result.radius>1e7f)
                throw std::logic_error("Invalid world-effect emission result");
            e.emission_id=result.id;if(result.id!=-1)e.radius=result.radius;WorldEffectEmitted(e);++emitted;
        }
    }
    catch(...){impl_->failed=true;throw;}
    return emitted;
}
std::vector<WorldEffectSnapshot> WorldEffects::Snapshot()const
{
    impl_->Check(true);std::vector<WorldEffectSnapshot> out;for(const auto& e:impl_->effects)
        out.push_back({e.record.id,e.record.group,e.m_nTimingMode,e.m_nRemainingEmissions,e.emission_id,e.m_fEmissionTime,e.m_fPreviousEmissionTime,e.radius,e.m_bActive});return out;
}
std::uint32_t WorldEffects::Seed()const{impl_->Check(true);return nlDefaultSeed;}
bool WorldEffects::Failed()const{impl_->Check(true);return impl_->failed;}
void WorldEffects::Release(){impl_->Clean();}

struct WorldBillboardEffects::Implementation
{
    struct Binding{std::uint64_t owner;ParticleControllers::Token token;std::shared_ptr<bool> alive;};
    EffectsRegistry::Handle registry;std::shared_ptr<ParticleControllers> controllers;std::vector<Binding> bindings;
    std::thread::id thread=std::this_thread::get_id();bool busy=false;
    void Check()const{if(thread!=std::this_thread::get_id()||busy||!controllers->Active())throw std::logic_error("World billboard service requires its live owner thread");}
    void Clean(std::uint64_t identity)
    {
        if(thread!=std::this_thread::get_id()||busy)throw std::logic_error("World billboard service owner thread required");
        BusyGuard guard(busy);
        for(auto it=bindings.begin();it!=bindings.end();)
        {
            if(it->owner!=identity){++it;continue;}
            if(*it->alive)
            {
                // Original World clears particles/backlinks while its global
                // manager retains controllers. This dedicated native service
                // owns them, so real destruction retires both storage and
                // callbacks, and also works after a failed controller update.
                controllers->Destroy(it->token);
            }
            it=bindings.erase(it);
        }
    }
};
WorldBillboardEffects::WorldBillboardEffects(EffectsRegistry::Handle registry,std::shared_ptr<ParticleControllers> controllers):impl_(std::make_unique<Implementation>())
{
    if(!registry||!controllers||!controllers->Active())throw std::invalid_argument("World billboard service requires actual retained effects/controllers");
    impl_->registry=std::move(registry);impl_->controllers=std::move(controllers);
}
WorldBillboardEffects::~WorldBillboardEffects()
{try{while(!impl_->bindings.empty())impl_->Clean(impl_->bindings.back().owner);}catch(...){std::terminate();}}
WorldEffectEmission WorldBillboardEffects::Emit(const WorldEffectRequest& request)
{
    impl_->Check();if(!request.owner||!request.current||request.animated)throw std::invalid_argument("Animated world effects require their actual animation service");
    if(impl_->registry->UnavailableGroups().contains(request.group))throw std::logic_error("World-effect group requires unprovided user effects");
    auto group=impl_->registry->FindGroup(request.group);if(!group)return{};
    float radius=0;for(unsigned i=0;i<group->m_numSpecs;++i)
    {
        if(!group->m_specs[i].m_pTemplate)throw std::logic_error("World-effect template is unresolved");
        const float r=group->m_specs[i].m_pTemplate->GetBoundingRadius();
        if(!std::isfinite(r)||r<0||r>1e7f)throw std::invalid_argument("Invalid original world-effect bounding radius");if(r>radius)radius=r;
    }
    auto initial=ControllerFrame(request.initial,radius);initial.visible=true; // Original constructor: visibility callback runs on Update.
    BusyGuard guard(impl_->busy);std::erase_if(impl_->bindings,[](const auto& b){return !*b.alive;});
    impl_->bindings.reserve(impl_->bindings.size()+1);auto alive=std::make_shared<bool>(true);
    const auto token=impl_->controllers->Start(impl_->registry,request.group,{true,0});
    try
    {
        impl_->controllers->SetFrame(token,initial);
        impl_->controllers->SetCallbacks(token,[current=request.current,radius](ParticleEmitterFrame& frame){frame=ControllerFrame(current(),radius);},[alive](auto,int reason){if(reason==2)*alive=false;});
        const auto snapshots=impl_->controllers->Snapshot();auto it=std::find_if(snapshots.begin(),snapshots.end(),[&](const auto& c){return c.token==token;});
        if(it==snapshots.end())throw std::logic_error("Original world-effect controller was not retained");
        impl_->bindings.push_back({request.owner,token,alive});return{it->id,radius};
    }
    catch(...){impl_->controllers->Destroy(token);throw;}
}
void WorldBillboardEffects::Release(std::uint64_t owner){impl_->Clean(owner);}
}
