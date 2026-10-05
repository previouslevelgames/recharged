#include "runtime/model_particles.h"
#include "runtime/particle_simulation_internal.h"
#include "runtime/views.h"
#include "Game/Effects/ParticleModelSteps.h"
#include "Game/Effects/ParticleBillboard.h"
#include "Game/SHierarchy.h"
#include "NL/gl/gl.h"
#include "NL/gl/glMaterialProgram.h"
#include <climits>
#include <cmath>
#include <cstring>
#include <thread>

namespace mscharged
{
namespace
{
void Bound(float value,float maximum,const char* field)
{
    if(!std::isfinite(value)||std::abs(value)>maximum)
        throw std::invalid_argument(std::string("Unqualified model-particle ")+field);
}
void Range(const fxRange& value){Bound(value.base,10000,"range");Bound(value.range,10000,"range width");}
void Matrix(const nlMatrix4& value)
{
    for(float v:value.e)Bound(v,1e7f,"matrix");
    if(value.e[3]!=0||value.e[7]!=0||value.e[11]!=0||value.e[15]!=1)
        throw std::invalid_argument("Model-particle matrix must be affine");
}
void Qualify(const EffectsSpec& spec,const EffectsTemplate& value)
{
    if((spec.m_eAttach!=FXBind_Emitter&&spec.m_eAttach!=FXBind_Joint&&spec.m_eAttach!=3)
        ||spec.m_eJointBinding!=JB_Normal||spec.m_bLight||spec.m_uTerrainID||spec.m_nForwardAxis<0||spec.m_nForwardAxis>6)
        throw std::invalid_argument("Model-particle attachment, terrain, light or ascending-joint service is unavailable");
    if(value.m_uModelID==0xffffffff||value.m_eBillboard>EfBill_Groundboard
        ||(value.mUnidentified037&~15)||value.m_eEmitter>Emitter_Disc||value.m_eBlend>EfBlend_Additive
        ||value.mUnidentified040||value.mUnidentified044||value.mUnidentified048)
        throw std::invalid_argument("Unqualified model-particle template or event service");
    if(value.m_cColour.size()<25)throw std::invalid_argument("Model-particle colour channel is incomplete");
    Bound(value.m_fFountainLife,1e10f,"fountain lifetime");
    for(auto r:{value.m_rMass,value.m_rParticleLife,value.m_rInheritVelocity,value.m_rAcceleration,value.m_rRotation,value.m_rFPS})Range(r);
    const float life_min=value.m_rParticleLife.base-.5f*std::abs(value.m_rParticleLife.range);
    const float life_max=value.m_rParticleLife.base+.5f*std::abs(value.m_rParticleLife.range);
    const float fps_min=value.m_rFPS.base-.5f*std::abs(value.m_rFPS.range);
    if(life_min<1e-6f||life_max>1000||fps_min<0)
        throw std::invalid_argument("Model-particle lifetime/FPS is outside the qualified domain");
    Bound(value.mUnidentified030,10000,"UV flip threshold");
    for(const auto* property:value.mProperties)
    {
        if(!property)throw std::invalid_argument("Missing model-particle property");
        if(!property->mUseCurve)Range({property->base,property->range});
        else
        {
            if(!property->mNumKeys||!property->mKeys||property->mKeys[0].mTime!=0)
                throw std::invalid_argument("Model-particle curve does not cover time zero");
            for(unsigned long i=0;i<property->mNumKeys;++i)
                for(float f:{property->mKeys[i].mCubic,property->mKeys[i].mQuadratic,property->mKeys[i].mLinear,property->mKeys[i].mConstant})
                    Bound(f,10000,"curve coefficient");
        }
    }
    Bound(spec.m_fDelay,1000,"delay");Bound(spec.m_fOffset,10000,"offset");
    for(float f:{spec.m_vLocalOffset.x,spec.m_vLocalOffset.y,spec.m_vLocalOffset.z})Bound(f,10000,"local offset");
    Bound(spec.m_fLingerStart,1e10f,"linger start");Bound(spec.m_fLingerEnd,1e10f,"linger end");
    if(spec.m_fDelay<0||(spec.m_fLingerEnd>=0&&(spec.m_fLingerStart<0||spec.m_fLingerStart>spec.m_fLingerEnd))
        ||spec.m_uLayer>=INT_MAX)
        throw std::invalid_argument("Invalid model-particle delay, linger or layer");
}
std::array<float,3> Vector(const nlVector3& v){return{v.x,v.y,v.z};}
struct ResolvedInput
{
    nlVector3 position,velocity,direction;
    nlMatrix4 coordinates;
};
ResolvedInput Resolve(const EffectsSpec& spec,const ModelParticleInputs& input)
{
    for(const auto& v:{input.position,input.velocity,input.direction})for(float f:v)Bound(f,10000,"input vector");
    ResolvedInput result{{input.position[0],input.position[1],input.position[2]},
        {input.velocity[0],input.velocity[1],input.velocity[2]},
        {input.direction[0],input.direction[1],input.direction[2]}, {}};
    const bool joint=spec.m_eAttach==FXBind_Joint||spec.m_eAttach==3;
    if(joint||spec.m_nForwardAxis)
    {
        if(!input.pose||!input.pose->hierarchy)throw std::invalid_argument("Model-particle attachment requires an actual retained pose");
        const auto& hierarchy=input.pose->hierarchy->Data();
        if(input.pose->matrices.size()!=std::size_t(hierarchy.GetNumNodes()))throw std::invalid_argument("Pose matrix count differs from retained hierarchy");
        auto hash=static_cast<std::uint32_t>(spec.m_uJointID);
        if(input.mirror)
        {
            const int node=hierarchy.GetNodeIndexByID(hash);
            if(node<0||node>=hierarchy.GetNumNodes())throw std::invalid_argument("Authored mirrored attachment joint is absent");
            hash=hierarchy.GetNodeID(hierarchy.GetMirroredNode(node));
        }
        if(input.joint_override)hash=input.joint_override;
        const int node=hierarchy.GetNodeIndexByID(hash);
        if(node<0||node>=hierarchy.GetNumNodes())throw std::invalid_argument("Model-particle attachment joint is absent");
        const auto& matrix=input.pose->matrices[node];Matrix(matrix);
        if(joint)result.position=matrix.GetTranslation();
        if(spec.m_nForwardAxis)fxParticleJointDirection(result.direction,matrix,spec.m_nForwardAxis);
    }
    else if(input.mirror||input.joint_override)
        throw std::invalid_argument("Joint modifiers require an actual joint consumer");
    if(spec.m_bGround&&!input.ground)throw std::invalid_argument("Model-particle ground policy and height must be explicit");
    if(input.ground)
    {
        Bound(input.ground->height,10000,"ground height");Bound(input.ground->shadow_height,10000,"shadow height");
        fxParticleGroundPosition(result.position,input.ground->enabled,spec.m_bGround!=0,
            input.ground->height,input.ground->shadow_height,spec.m_fOffset);
    }
    else fxParticleGroundPosition(result.position,false,false,0,0,spec.m_fOffset);
    for(float f:{result.position.x,result.position.y,result.position.z,result.direction.x,result.direction.y,result.direction.z})Bound(f,1e7f,"resolved attachment");
    const double squared=double(result.direction.x)*result.direction.x+double(result.direction.y)*result.direction.y+double(result.direction.z)*result.direction.z;
    if(squared<1e-12)throw std::invalid_argument("Model-particle direction cannot be zero");
    result.coordinates.SetIdentity();fxParticleCoordinates(result.coordinates,result.direction,result.position);Matrix(result.coordinates);
    return result;
}
}
struct ModelParticles::Implementation
{
    EffectsRegistry::Handle registry;
    std::shared_ptr<EffectsVertexResources> geometry;
    std::shared_ptr<const EffectsGroup> group;
    std::shared_ptr<const resources::Texture> texture;
    std::shared_ptr<ParticleSimulationContext> context;
    EffectsSpec spec{};
    ModelParticleInputs inputs;
    ResolvedInput resolved;
    std::unique_ptr<ParticleSystem> system;
    std::thread::id thread=std::this_thread::get_id();
    std::optional<std::uint64_t> pending;
    bool live=false,failed=false,busy=false;
    std::uint32_t seed;
    unsigned frames;
    Implementation(EffectsRegistry::Handle r,std::shared_ptr<EffectsVertexResources> g,std::uint32_t hash,
        std::size_t index,ModelParticleInputs input,ParticleSimulationOptions options)
        :registry(std::move(r)),geometry(std::move(g)),inputs(std::move(input)),seed(options.seed)
    {
        if(!registry||!geometry||!geometry->Active())throw std::invalid_argument("Model particles require retained registry and real geometry");
        group=registry->FindGroup(hash);
        if(!group||index>=group->m_numSpecs||group->m_userSpecs)throw std::invalid_argument("Model-particle group/spec is unavailable");
        spec=group->m_specs[index];if(!spec.m_pTemplate)throw std::invalid_argument("Model-particle template is unresolved");
        Qualify(spec,*spec.m_pTemplate);frames=geometry->State(spec.m_pTemplate->m_uModelID).frames;
        if(!frames||frames>INT_MAX)throw std::invalid_argument("Model particles require actual supported animation frames");
        texture=registry->FindTexture(spec.m_pTemplate->m_hTexture);
        if(!texture)throw std::invalid_argument("Model-particle template texture is absent");
        resolved=Resolve(spec,inputs);
        context=std::make_shared<ParticleSimulationContext>(options);
        try{Construct();live=true;}catch(...){system.reset();context.reset();throw;}
    }
    void Check(bool allow_failed=false,bool allow_pending=false) const
    {
        if(thread!=std::this_thread::get_id()||!live||!gMemoryInitialized||busy)
            throw std::logic_error("Model particles require their idle, live owner thread");
        context->Check();
        if((failed&&!allow_failed)||(pending&&!allow_pending))throw std::logic_error("Model particles failed or have a submitted frame");
        if(!geometry->Active()||geometry->State(spec.m_pTemplate->m_uModelID).frames!=frames)
            throw std::logic_error("Retained model-particle geometry was released or changed");
    }
    void Idle(bool allow_failed=false) const
    { Check(allow_failed);if(glIsFrameActive())throw std::logic_error("Model-particle mutation requires an idle graphics frame"); }
    void Apply()
    {
        system->m_vPosition=resolved.position;system->m_vVelocity=resolved.velocity;system->m_vForward=resolved.direction;
        system->m_mCoordSys=resolved.coordinates;system->m_aFacing=inputs.facing;system->m_bVisible=inputs.visible;
    }
    void Construct()
    {
        ScopedGameAllocator arena(VirtualAllocator);
        system=std::make_unique<ParticleSystem>(ParticleSystem::NativeSimulation{},spec.m_pTemplate,&context->free,&spec,group->m_hashID);
        system->m_Particles.m_Allocator.Initialize(context->capacity,0);
        system->m_fDelay=spec.m_fDelay;system->m_uLayer=spec.m_uLayer;Apply();uSeed=seed;
    }
    void Clean()
    {
        if(!live)return;
        if(thread!=std::this_thread::get_id()||busy||pending||glIsFrameActive()||!gMemoryInitialized)
            throw std::logic_error("Finish model-particle frames before release on the owning thread");
        context->Check();system.reset();context.reset();live=false;geometry.reset();texture.reset();group.reset();registry.reset();inputs.pose.reset();
    }
    ~Implementation(){try{Clean();}catch(...){std::terminate();}}
};
ModelParticles::ModelParticles(EffectsRegistry::Handle registry,std::shared_ptr<EffectsVertexResources> geometry,
    std::uint32_t group,std::size_t spec,ModelParticleInputs inputs,ParticleSimulationOptions options)
    :impl_(std::make_unique<Implementation>(std::move(registry),std::move(geometry),group,spec,std::move(inputs),options)){}
ModelParticles::~ModelParticles()=default;
bool ModelParticles::Active() const
{if(impl_->thread!=std::this_thread::get_id())throw std::logic_error("Model-particle owner thread required");return impl_->live;}
bool ModelParticles::Failed()const{impl_->Check(true,true);return impl_->failed;}
std::uint32_t ModelParticles::Seed()const{impl_->Check(true,true);return impl_->seed;}
void ModelParticles::Release(){impl_->Clean();}
void ModelParticles::SetInputs(ModelParticleInputs inputs)
{impl_->Idle();auto resolved=Resolve(impl_->spec,inputs);impl_->inputs=std::move(inputs);impl_->resolved=resolved;impl_->Apply();}
bool ModelParticles::Advance(float delta)
{
    impl_->Idle();if(!std::isfinite(delta)||delta<0||delta>1)throw std::invalid_argument("Model-particle delta must be within [0,1]");
    try
    {
        const bool alive=impl_->system->Update(delta);impl_->seed=uSeed;
        for(const auto& p:Snapshot())for(float value:{p.elapsed,p.fraction,p.lifespan,p.rotation,p.velocity,p.position[0],p.position[1],p.position[2]})
            if(!std::isfinite(value))throw std::domain_error("Original model-particle state is nonfinite");
        return alive;
    }
    catch(...){impl_->seed=uSeed;impl_->failed=true;throw;}
}
std::vector<ParticleSnapshot> ModelParticles::Snapshot()const
{
    impl_->Check();std::vector<ParticleSnapshot> result;result.reserve(impl_->system->m_NumParticles);
    auto it=impl_->system->m_Particles.Begin();while(it.hasNext())
    {
        const auto* p=*it;it.Step();
        result.push_back({p->timeElapsed,p->timeFraction,p->lifeSpan,p->rot,p->dRot,p->size,p->sizeScale,p->velocity,p->acceleration,p->mass,p->frame,p->FPS,
            Vector(p->initialPosition),Vector(p->position),Vector(p->velDir),p->flipTexcoords});
    }
    return result;
}
std::vector<ModelParticleSample> ModelParticles::Sample()
{
    impl_->Check();std::vector<ModelParticleSample> result;result.reserve(impl_->system->m_NumParticles);
    try
    {
        nlMatrix4 coordinates;fxModelParticleCoordinates(coordinates,impl_->system->m_mCoordSys);
        auto it=impl_->system->m_Particles.Begin();while(it.hasNext())
        {
            auto* particle=*it;it.Step();ParticleReturn returned{};
            impl_->system->UpdateParticle(&returned,particle,impl_->spec.m_pTemplate,{1,0,0},{0,1,0},
                impl_->spec.m_pTemplate->IsLocalSpace()?&impl_->system->m_mCoordSys:nullptr);
            const float raw=particle->FPS*particle->timeElapsed;
            if(!(impl_->spec.m_pTemplate->mUnidentified037&8)&&(!std::isfinite(raw)||raw<0||double(raw)/impl_->frames>100000))
                throw std::domain_error("Model-particle frame loop exceeds its checked range");
            const int frame=impl_->spec.m_pTemplate->mUnidentified037&8
                ?fxModelParticleLifeFrame(particle->timeElapsed,particle->lifeSpan,int(impl_->frames))
                :fxModelParticleFpsFrame(particle->FPS,particle->timeElapsed,int(impl_->frames));
            if(frame<0||unsigned(frame)>=impl_->frames)throw std::domain_error("Original model-particle frame is out of bounds");
            ModelParticleSample sample;sample.frame=unsigned(frame);
            for(unsigned i=0;i<4;++i)sample.colour[i]=returned.c.c[i];
            for(float f:{returned.position[0].x,returned.position[0].y,returned.position[0].z,returned.position[1].x,returned.position[1].y})Bound(f,1e7f,"sample");
            fxModelParticleMatrix(sample.matrix,coordinates,returned,impl_->spec.m_pTemplate->m_eBillboard,impl_->system->m_aFacing);Matrix(sample.matrix);
            result.push_back(sample);
        }
    }
    catch(...){impl_->failed=true;throw;}
    return result;
}
unsigned ModelParticles::Submit(GLView& view)
{
    impl_->Check();if(!glIsFrameActive()||glNativeViewDispatchActive()||view.m_NativeIterating||!view.m_Interface)
        throw std::logic_error("Model-particle submission requires a collecting original view");
    if(!impl_->inputs.visible)return 0;
    const auto samples=Sample();if(samples.empty())return 0;
    impl_->busy=true;struct Leave{bool& busy;~Leave(){busy=false;}}leave{impl_->busy};
    impl_->pending=glNativeFrameGeneration();
    glStateBundle saved;glStateSave(saved);struct Restore{glStateBundle& saved;~Restore(){glStateRestore(saved);}}restore{saved};
    const auto& t=*impl_->spec.m_pTemplate;
    try
    {
        fxSetParticleRasterState(true,false,ParticleSystem::m_AllowInFront&&(t.IsInFront()||impl_->spec.m_bInFront),false,t.m_eBlend);
        for(const auto& sample:samples)
        {
            auto* model=impl_->geometry->Model(t.m_uModelID,int(sample.frame));
            for(unsigned p=0;p<model->numPackets;++p)
            {
                auto& packet=model->packets[p];auto* program=static_cast<GLMaterialProgram*>(packet.materialProgram);
                if(!program||(program!=glGetMaterialProgram(0xee9d919d)&&program!=glGetMaterialProgram(0x19065bf6))
                    ||!packet.materialParameters||program->parameterCount>16)throw std::logic_error("Model-particle material is unavailable");
                const auto* parameters=program->GetParameters();
                for(unsigned i=0;i<program->parameterCount;++i)
                {
                    const auto& parameter=parameters[i];if(parameter.hash!=0xee9d919d)continue;
                    if(parameter.metadata!=0x01040101||parameter.offset>program->parameterDataSize||program->parameterDataSize-parameter.offset<sizeof(nlVector4))
                        throw std::logic_error("Model-particle constant-colour parameter has an invalid native extent");
                    nlColour source;for(unsigned c=0;c<4;++c)source.c[c]=sample.colour[c];nlVector4 colour;fxModelParticleColour(colour,source);
                    std::memcpy(static_cast<unsigned char*>(packet.materialParameters)+parameter.offset,&colour,sizeof(colour));
                }
                fxModelParticleRaster(packet.rasterState,t.m_eBlend==EfBlend_Normal?1:3,t.mUnidentified037);
            }
            glModelSetMatrix(model,sample.matrix);view.AttachModel(model,impl_->spec.m_uLayer+1);
        }
    }
    catch(...){impl_->failed=true;throw;}
    return samples.size();
}
void ModelParticles::FinishFrame()
{
    // Geometry's drain callback can reenter; keep both owners guarded until it
    // succeeds. Failed drains retain the pending generation for an explicit retry.
    if(impl_->thread!=std::this_thread::get_id()||!impl_->live||impl_->busy)throw std::logic_error("Model-particle owner thread required");
    if(!impl_->pending)return;
    if(glIsFrameActive()||*impl_->pending==glNativeFrameGeneration())throw std::logic_error("Send or cancel the model-particle frame before finishing");
    impl_->busy=true;struct Leave{bool& busy;~Leave(){busy=false;}}leave{impl_->busy};
    impl_->geometry->FinishFrame();impl_->pending.reset();
}
void ModelParticles::Die(){impl_->Idle();impl_->system->Die();}
void ModelParticles::Reset(std::uint32_t seed)
{
    impl_->Idle(true);impl_->system.reset();impl_->seed=seed;
    try{impl_->Construct();impl_->failed=false;}catch(...){impl_->failed=true;throw;}
}
}
