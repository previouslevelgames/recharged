#include "model_particles_fixture.h"
#include "runtime/frames.h"
#include "runtime/views.h"
#include "runtime/materials.h"
#include "runtime/graphics_state.h"
#include "runtime/graphics_memory.h"
#include "runtime/startup.h"
#include "Game/Effects/ParticleModelSteps.h"
#include "NL/gl/gl.h"
#include "NL/gl/glMemory.h"
#include "NL/gl/glMemoryInit.h"
#include "NL/gl/glMatrix.h"
#include "NL/glx/GXConstantColourMaterialProgram.h"
#include <bit>
#include <cmath>
#include <iostream>
#include <numbers>
#include <source_location>
#include <thread>
using namespace mscharged;
namespace f=model_particle_fixture;
namespace
{
unsigned checks=0;bool fail_drain=false;ModelParticles* active=nullptr;
void Check(bool yes,const char* text){++checks;if(!yes)throw std::runtime_error(text);}
template<class F>void Reject(F fn,std::source_location at=std::source_location::current())
{++checks;try{fn();}catch(const std::exception&){return;}throw std::runtime_error("Invalid model-particle operation accepted at "+std::to_string(at.line()));}
void Near(double actual,double expected,double tolerance=3e-4)
{Check(std::isfinite(actual)&&std::abs(actual-expected)<=tolerance*std::max(1.,std::abs(expected)),"Independent model-particle numerical oracle differs");}
void Invalidate(){} // CPU-only fixture; there are no GPU references.
void Drain(){if(active){Reject([&]{active->Release();});Reject([&]{active->FinishFrame();});}if(fail_drain)throw std::runtime_error("Controlled drain failure");}
const glModelPacket* observed=nullptr;unsigned packets=0;
void Inspect(GLView*,unsigned long,const glModelPacket* packet){if(packet){observed=packet;++packets;}}
struct Backend:FrameBackend
{bool Acquire()override{return true;}void Render()override{}void Finish(bool)override{}void Drain()override{}void WaitIdle()override{}void Cancel()noexcept override{}};
void Begin(OriginalFrames& frames){Check(frames.Acquire(),"CPU model frame acquisition failed");glBeginFrame();}
std::uint32_t Random(std::uint32_t seed,unsigned count)
{
    while(count--){const auto x=seed^0x1d872b41U;const auto y=x^(x>>5);seed=y^x^(y<<27);}return seed;
}
using Vec=std::array<double,3>;using Mat=std::array<double,16>;
Vec Cross(Vec a,Vec b){return{a[1]*b[2]-a[2]*b[1],a[2]*b[0]-a[0]*b[2],a[0]*b[1]-a[1]*b[0]};}
Vec Unit(Vec a){const double length=std::sqrt(a[0]*a[0]+a[1]*a[1]+a[2]*a[2]);for(auto& x:a)x/=length;return a;}
Mat Coordinate(Vec direction,Vec position)
{
    auto d=Unit(direction);Vec ref{0,0,1};if(std::abs(d[2])>.99)ref={0,1,0};
    auto right=Unit(Cross(d,ref));auto up=Unit(Cross(right,d));Mat m{};
    for(unsigned i=0;i<3;++i){m[i]=right[i];m[4+i]=up[i];m[8+i]=-d[i];m[12+i]=position[i];}m[15]=1;return m;
}
Mat Multiply(const Mat& a,const Mat& b)
{Mat out{};for(unsigned y=0;y<4;++y)for(unsigned x=0;x<4;++x)for(unsigned k=0;k<4;++k)out[y*4+x]+=a[y*4+k]*b[k*4+x];return out;}
void Numerics(const std::vector<ParticleSnapshot>& particles,const std::vector<ModelParticleSample>& samples,
    Vec direction,Vec position,bool life=false,bool facing=false,std::uint16_t angle=0)
{
    Check(particles.size()==samples.size(),"Model sample count differs");auto coordinate=Coordinate(direction,position);
    for(unsigned i=0;i<particles.size();++i)
    {
        const auto& p=particles[i];const auto& sample=samples[i];
        // Independent continuous curves/byte interpolation: generated timestamps
        // and lifespan are powers of two, so all selected colour knots are exact.
        const unsigned knot=unsigned(24*p.elapsed/p.lifespan);
        Check(sample.colour==std::array<std::uint8_t,4>{std::uint8_t(knot*8),std::uint8_t(knot*4),std::uint8_t(knot*2),255},"Original model colour differs");
        const unsigned frame=life?unsigned(double(p.elapsed)/p.lifespan*2):unsigned(std::fmod(double(p.fps)*p.elapsed,3.));
        Check(sample.frame==frame,"Original model animation frame differs");
        Mat basis=coordinate;basis[2]=-basis[2];basis[6]=-basis[6];basis[10]=-basis[10];
        double radians=double(p.rotation)*std::numbers::pi/180.;if(facing)radians+=double(std::uint16_t(angle+0x8000))*2*std::numbers::pi/65536.;
        // The source quantizes to16bit angles. The scalar trig oracle uses the
        // independently expected angle and permits <3e-4 relative quantization.
        const double s=std::sin(radians),c=std::cos(radians),size=double(p.size)*p.size_scale;
        Mat scale_rotation{size*c,size*s,0,0,-size*s,size*c,0,0,0,0,size,0,0,0,0,1};auto expected=Multiply(basis,scale_rotation);
        for(unsigned axis=0;axis<3;++axis)expected[12+axis]=double(p.position[0])*coordinate[axis]+double(p.position[1])*coordinate[4+axis]+double(p.position[2])*coordinate[8+axis]+coordinate[12+axis];
        for(unsigned element=0;element<16;++element)Near(sample.matrix.e[element],expected[element]);
    }
}
void Generated(const std::filesystem::path& folder,GLView& view,OriginalFrames& frames)
{
    auto geometry=std::make_shared<EffectsVertexResources>(f::Geometry(),resources::ReadTextureBundle(f::Read(folder/"textures.rlt")),Drain);
    ModelParticleInputs inputs;inputs.pose=f::Pose();const auto saved=gEffectsRandomSeed;
    // Prime the original view's persistent sorter block before measuring each
    // particle lifetime; the containing view session verifies its final release.
    Begin(frames);view.AttachModel(geometry->Model(0x10203040,0),1);frames.Cancel();geometry->FinishFrame();
    const auto a=StandardAllocator.TotalFreeMemory(),b=VirtualAllocator.TotalFreeMemory();
    for(const char* mode:{"missing_model","nonanimated","missing_texture","negative_fps","zero_life","ascend","light","terrain","axis","layer"})
        Reject([&]{ModelParticles value(f::Registry(folder,mode),geometry,0x81f2a311,0,inputs);});
    auto missing=inputs;missing.pose.reset();Reject([&]{ModelParticles value(f::Registry(folder,"basic"),geometry,0x81f2a311,0,missing);});
    Reject([&]{ModelParticles value(f::Registry(folder,"ground"),geometry,0x81f2a311,0,inputs);});
    for(const char* mode:{"basic","lifeframe","facing","depth","additive"})
    {
        ModelParticles value(f::Registry(folder,mode),geometry,0x81f2a311,0,inputs,{32,123});active=&value;
        Reject([&]{ModelParticles duplicate(f::Registry(folder,"basic"),geometry,0x81f2a311,0,inputs);});
        Reject([&]{value.Advance(-1);});Reject([&]{value.Advance(INFINITY);});
        std::thread wrong([&]{Reject([&]{value.Release();});Reject([&]{value.SetInputs(inputs);});});wrong.join();
        for(unsigned tick=0;tick<7;++tick)
        {
            value.Advance(.125f);auto state=value.Snapshot();auto sample=value.Sample();
            Check(state.size()==tick+1,"Original live model-particle count differs");Check(value.Seed()==Random(123,(tick+1)*27),"Model particle RNG order differs");
            Numerics(state,sample,{0,0,1},{3,4,5},std::string_view(mode)=="lifeframe",std::string_view(mode)=="facing");
        }
        auto retained=value.Sample();auto malformed=inputs;malformed.joint_override=0xdeadbeef;Reject([&]{value.SetInputs(malformed);});
        malformed=inputs;malformed.direction[1]=NAN;Reject([&]{value.SetInputs(malformed);});
        auto forged=std::make_shared<AnimationPoseFrame>(*inputs.pose);forged->matrices.resize(1);malformed=inputs;malformed.pose=forged;Reject([&]{value.SetInputs(malformed);});
        Check(value.Sample().size()==retained.size(),"Invalid inputs changed publication");
        auto mirrored=inputs;mirrored.mirror=true;value.SetInputs(mirrored);Numerics(value.Snapshot(),value.Sample(),{0,-1,0},{-4,1,2},std::string_view(mode)=="lifeframe",std::string_view(mode)=="facing");
        auto overridden=inputs;overridden.joint_override=0x22223333;value.SetInputs(overridden);Numerics(value.Snapshot(),value.Sample(),{0,-1,0},{-4,1,2},std::string_view(mode)=="lifeframe",std::string_view(mode)=="facing");
        value.SetInputs(inputs);const auto expected=value.Sample();Begin(frames);glStateBundle before;glStateSave(before);
        Check(value.Submit(view)==7,"Original model render dropped particles");packets=0;view.Iterate(Inspect);Check(packets==7&&observed,"Original packet count differs");
        const auto& colour=static_cast<const GXConstantColourParameters*>(observed->materialParameters)->constantColour;
        for(unsigned c=0;c<4;++c)Near(colour.c[c],double(expected.back().colour[c])/255,1e-6);
        Check(glGetRasterState(observed->rasterState,GLS_Culling)==0&&glGetRasterState(observed->rasterState,GLS_AlphaTest)==1
            &&glGetRasterState(observed->rasterState,GLS_AlphaTestRef)==3,"Original model raster rules differ");
        Check(glGetRasterState(observed->rasterState,GLS_AlphaBlend)==(std::string_view(mode)=="additive"?3:1),"Original model blend mapping differs");
        if(std::string_view(mode)=="depth")Check(glGetRasterState(observed->rasterState,GLS_DepthWrite)==0,"Model flag4 lost depth-write rule");
        nlMatrix4 matrix;glGetMatrix(observed->matrix,matrix);for(unsigned i=0;i<16;++i)Near(matrix.e[i],expected.back().matrix.e[i],1e-6);
        Check(glGetCurrentRasterState()==before.raster,"Model submission leaked global raster state");
        Reject([&]{value.SetInputs(inputs);});Reject([&]{value.Release();});Reject([&]{value.FinishFrame();});frames.Cancel();
        fail_drain=true;Reject([&]{value.FinishFrame();});fail_drain=false;value.FinishFrame();
        value.Die();Check(!value.Advance(1)&&value.Sample().empty(),"Model particles did not drain after Die");
        for(unsigned repeat=0;repeat<3;++repeat){value.Reset(123);value.Advance(.125f);Check(value.Seed()==Random(123,27),"Model Reset changed RNG state");}
        value.Release();active=nullptr;Reject([&]{value.Advance(0);});
        Check(StandardAllocator.TotalFreeMemory()==a&&VirtualAllocator.TotalFreeMemory()==b&&gEffectsRandomSeed==saved,"Model particles leaked pools, arena or RNG ownership");
    }
    {
        auto ground=inputs;ground.ground=ModelParticleGround{true,2,.5};ModelParticles value(f::Registry(folder,"ground"),geometry,0x81f2a311,0,ground);
        value.Advance(.125f);Numerics(value.Snapshot(),value.Sample(),{0,0,1},{3,4,2.75});
        ground.ground->enabled=false;value.SetInputs(ground);Numerics(value.Snapshot(),value.Sample(),{0,0,1},{3,4,5.25});value.Release();
    }
    // Real tiny MEM2 allocators exercise original atlas/list construction
    // rollback without altering the actual containing graphics pool.
    for(unsigned bytes:{64,256,1024,4096,8192})
    {
        alignas(64)unsigned char storage[16384];MemoryAllocator tiny;tiny.Initialize(storage,bytes);
        const auto old=VirtualAllocator;VirtualAllocator=tiny;
        try{Reject([&]{ModelParticles value(f::Registry(folder,"basic"),geometry,0x81f2a311,0,inputs,{4096,0});});}
        catch(...){VirtualAllocator=old;throw;}
        const bool recovered=VirtualAllocator.TotalFreeMemory()==tiny.TotalFreeMemory();VirtualAllocator=old;
        Check(recovered&&gEffectsRandomSeed==saved&&ParticleSystem::m_NumInstances==0,"Model construction rollback lost native ownership");
    }
    {
        ModelParticles value(f::Registry(folder,"basic"),geometry,0x81f2a311,0,inputs,{32,123});value.Advance(.125f);
        Begin(frames);glFrameAlloc(262144-64,GLM_Header);Reject([&]{value.Submit(view);});frames.Cancel();value.FinishFrame();
        Check(value.Failed(),"Failed model clone did not poison its session");Reject([&]{value.Advance(0);});value.Reset(123);value.Advance(.125f);
        Begin(frames);Check(value.Submit(view)==1,"Model clone failure did not recover after reset");frames.Cancel();value.FinishFrame();value.Release();
    }
    {
        auto registry=f::Registry(folder,"basic");auto group=registry->FindGroup(0x81f2a311);nlDLListSlotPool<Particle*> free;
        ParticleSystem original(ParticleSystem::NativeSimulation{},group->m_specs[0].m_pTemplate,&free,&group->m_specs[0],1);
        Particle particle{};particle.pTemplate=group->m_specs[0].m_pTemplate;particle.lifeSpan=1;particle.timeElapsed=1;particle.size=2;particle.sizeScale=1;
        ParticleReturn result{};Reject([&]{original.UpdateParticle(&result,&particle,particle.pTemplate,{1,0,0},{0,1,0},nullptr);});
        particle.timeElapsed=.03125f;original.UpdateParticle(&result,&particle,particle.pTemplate,{1,0,0},{0,1,0},nullptr);
        Check(result.c.c[0]==6&&result.c.c[1]==3&&result.c.c[2]==1,"Fractional model colour interpolation differs");
    }
    geometry->Release();
    // Direct shared-kernel endpoint proof is separate from live-particle removal:
    // live Update removes elapsed>=life before Sample can consume it.
    Check(fxModelParticleLifeFrame(0,1,61)==0&&fxModelParticleLifeFrame(1,1,61)==60,"Original lifetime endpoint differs");
    Check(fxModelParticleLifeFrame(std::nextafter(1.f,0.f),1,61)==59,"Original near-endpoint frame differs");
    for(float elapsed:{0.f,.125f,.5f,1.f,7.5f})Check(fxModelParticleFpsFrame(4,elapsed,3)==int(std::fmod(double(elapsed)*4,3.)),"Original repeated frame subtraction differs");
}
void Owned(const std::filesystem::path& folder,const std::filesystem::path& hierarchy,GLView& view,OriginalFrames& frames)
{
    auto registry=f::Registry(folder,"",true);auto files=registry->Files();
    auto geometry=std::make_shared<EffectsVertexResources>(resources::ReadEffectsGeometry(files->data[2]),resources::ReadTextureBundle(files->data[3]),Drain);
    auto rig=HierarchyAsset::Decode(f::Read(hierarchy));const auto& original=rig->Data();Check(original.GetNodeIndexByID(0x6dc7e5f8)>=0,"Owned holotron joint is absent");
    ModelParticleInputs inputs;inputs.pose=f::RestPose(rig);unsigned total=0,peak=0;
    for(auto group:{0x9fb76e54U,0xefdbdc68U})for(unsigned spec=0;spec<2;++spec)
    {
        ModelParticles value(registry,geometry,group,spec,inputs,{64,0x9184eb0c});
        for(unsigned tick=0;tick<180;++tick)
        {
            value.Advance(1.f/60);const auto state=value.Snapshot();const auto sample=value.Sample();Check(state.size()==sample.size(),"Owned model samples differ");
            peak=std::max(peak,unsigned(sample.size()));total+=sample.size();
            for(unsigned i=0;i<sample.size();++i)
            {
                const float raw=state[i].fps*state[i].elapsed;float oracle=spec==1?(state[i].elapsed/state[i].lifespan):std::fmod(raw,2.f);
                Check(sample[i].frame==unsigned(oracle),"Owned actual particle frame differs from independent scalar expression");
                for(float f:sample[i].matrix.e)Check(std::isfinite(f),"Owned rest-pose model matrix nonfinite");
            }
            Begin(frames);Check(value.Submit(view)==sample.size(),"Owned model packet count differs");frames.Cancel();value.FinishFrame();
        }
        value.Die();unsigned drain=0;while(value.Advance(1.f/60)&&drain<120)++drain;
        Check(drain<120&&value.Sample().empty(),"Owned model particles did not drain");value.Release();
    }
    Check(total>0&&peak>0,"Owned holotron rest-pose gate emitted nothing");geometry->Release();
    std::cout<<"Owned holotron rest pose: "<<total<<" samples, peak "<<peak<<", four authored specs; live NIS actor attachment remains pending\n";
}
}
int main(int argc,char** argv)
{
    try
    {
        const bool owned=argc==4&&std::string_view(argv[1])=="--owned";Check(owned||argc==2,"Use model_particles_tests FIXTURE or --owned EFFECTS_FOLDER HOLOTRON_HIERARCHY");
        std::vector<std::uint64_t> mem1(4*1024*1024),mem2(8*1024*1024);ResetStartupMemory();StandardAllocator.Initialize(mem1.data(),mem1.size()*8);VirtualAllocator.Initialize(mem2.data(),mem2.size()*8);gMemoryInitialized=1;
        const auto a=StandardAllocator.TotalFreeMemory(),b=VirtualAllocator.TotalFreeMemory();
        for(unsigned repeat=0;repeat<3;++repeat)
        {
            const GLMemoryRequirement req[]={{GLM_Header,65536},{GLM_VertexData,65536},{GLM_TextureData,65536}};
            const GLMemoryConfig config{262144,262144,req,3,512};glInitResourcePools();glInitMemory(&config);InitializeOriginalGraphicsState();SetGraphicsCacheInvalidator(Invalidate);
            MaterialPrograms materials;OriginalViews views(640,480,Invalidate);ViewMatrices matrices;auto* view=new(8,false)GLView(&matrices,{},GLViewSort_None);gRootView.AddChild(view);
            Backend backend;OriginalFrames frames(backend);if(owned)Owned(argv[2],argv[3],*view,frames);else Generated(argv[1],*view,frames);
            frames.Release();views.Release();materials.Release();glShutdownMemory();Check(StandardAllocator.TotalFreeMemory()==a&&VirtualAllocator.TotalFreeMemory()==b,"Model-particle graphics session leaked arenas");
        }
        ResetStartupMemory();std::cout<<checks<<" model-particle checks passed\n";
    }
    catch(const std::exception& e){std::cerr<<"FAILED: "<<e.what()<<" (check "<<checks<<")\n";return 1;}
}
