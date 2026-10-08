#include "platform/graphics_stats.h"
#include "model_particles_fixture.h"
#include "runtime/frames.h"
#include "runtime/views.h"
#include "runtime/materials.h"
#include "runtime/graphics_memory.h"
#include "runtime/graphics_state.h"
#include "runtime/startup.h"
#include "runtime/startup_files.h"
#include "Game/GraphicsMemoryStartup.h"
#include "NL/gl/gl.h"
#include "NL/gl/glMemory.h"
#include "NL/gl/glMemoryInit.h"
#include "NL/gl/glMatrix.h"
#include <aurora/aurora.h>
#include <aurora/event.h>
#include <aurora/gfx.h>
#include <dolphin/gx.h>
#include <dolphin/gx/GXAurora.h>
#include <dolphin/vi.h>
#include <SDL3/SDL.h>
#include <imgui.h>
#include <atomic>
#include <chrono>
#include <cmath>
#include <iostream>
using namespace mscharged;
namespace f=model_particle_fixture;
namespace
{
unsigned checks=0,draws=0;std::atomic_uint errors=0;
void Check(bool yes,const char* message){++checks;if(!yes)throw std::runtime_error(message);}
void Log(AuroraLogLevel level,const char*,const char* text,unsigned length){if(level>=LOG_ERROR)++errors;std::cerr.write(text,length);std::cerr<<'\n';}
void Drain(){AuroraGXSync();}void Invalidate(){GXInvalidateVtxCache();GXInvalidateTexAll();}
struct Session{bool live=false;~Session(){if(live){Drain();glShutdownMemory();ResetStartupFiles();ResetStartupMemory();aurora_shutdown();}}};
void Acquire(OriginalFrames& frames)
{
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(10);
    for(;;){for(const auto* e=aurora_update();e->type!=AURORA_NONE;++e)Check(e->type!=AURORA_EXIT,"Model-particle window closed");
        if(frames.Acquire())return;Check(std::chrono::steady_clock::now()<deadline,"Model-particle acquisition timed out");SDL_Delay(1);}
}
unsigned Render(ModelParticles& particles,GLView& view,OriginalFrames& frames,AuroraFrames& backend,bool read)
{
    try
    {
        Acquire(frames);glBeginFrame();GXSetPixelFmt(GX_PF_RGB8_Z24,GX_ZC_LINEAR);GXSetCopyClear({20,24,30,255},GX_MAX_Z24);
        const auto count=particles.Submit(view);backend.read_colours=read;glEndFrame();glSendFrame();particles.FinishFrame();draws+=aurora_get_stats()->drawCallCount;return count;
    }
    catch(...){frames.Cancel();particles.FinishFrame();throw;}
}
void Pixel(const ColourSamples& pixels,unsigned index,std::array<int,3> rgb)
{for(unsigned i=0;i<3;++i)Check(std::abs(int(pixels[index][i])-rgb[i])<=3,"Original model-particle GPU pixel differs");}
void Case(ModelParticles& particles,GLView& view,OriginalFrames& frames,AuroraFrames& backend,unsigned location)
{
    unsigned quiet=0;const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(15);
    for(unsigned n=0;;++n)
    {
        Check(std::chrono::steady_clock::now()<deadline,"Model-particle pipeline warmup timed out");
        const bool read=n>=12&&quiet>=2;Check(Render(particles,view,frames,backend,read)==1,"Generated model particle was not submitted");
        const auto pending=mscharged::platform::GetQueuedPipelineCount();quiet=pending?0:quiet+1;if(read&&!pending)break;
    }
    for(unsigned i=3;i<6;++i)Pixel(backend.colours,i,i==location?std::array<int,3>{100,100,25}:std::array<int,3>{20,24,30});
    Pixel(backend.colours,0,{20,24,30});Pixel(backend.colours,8,{20,24,30});
    std::cout<<"Original particle frame "<<particles.Sample().front().frame<<": expected grid"<<location<<" RGB100,100,25\n";
}
void Generated(const std::filesystem::path& folder,GLView& view,OriginalFrames& frames,AuroraFrames& backend)
{
    auto geometry=std::make_shared<EffectsVertexResources>(f::Geometry(),resources::ReadTextureBundle(f::Read(folder/"textures.rlt")),Drain);
    ModelParticleInputs inputs;inputs.position={0,0,-.3f};
    for(unsigned repeat=0;repeat<2;++repeat)
    {
        ModelParticles particles(f::Registry(folder,"gpu"),geometry,0x81f2a311,0,inputs,{8,123});
        particles.Advance(.125f);Case(particles,view,frames,backend,5);
        particles.Advance(.125f);Case(particles,view,frames,backend,4);
        particles.Advance(.375f);Case(particles,view,frames,backend,3);
        particles.Advance(.25f);Case(particles,view,frames,backend,5);
        Acquire(frames);glBeginFrame();particles.Submit(view);frames.Cancel();particles.FinishFrame();
        particles.Reset(123);particles.Advance(.125f);Case(particles,view,frames,backend,5);
        particles.Die();Check(!particles.Advance(1)&&particles.Sample().empty(),"Original model particle did not drain");particles.Release();
    }
    geometry->Release();
}
void Fit(ViewMatrices& matrices,const std::vector<ModelParticleSample>& samples,const resources::EffectsVertexAnimation& animation)
{
    if(samples.empty())return;
    std::array<float,3> low{INFINITY,INFINITY,INFINITY},high{-INFINITY,-INFINITY,-INFINITY};
    for(const auto& s:samples)for(const auto& p:animation.positions)
        for(unsigned axis=0;axis<3;++axis)
        {
            const float value=p[0]*s.matrix.e[axis]+p[1]*s.matrix.e[4+axis]+p[2]*s.matrix.e[8+axis]+s.matrix.e[12+axis];
            low[axis]=std::min(low[axis],value);high[axis]=std::max(high[axis],value);
        }
    const float radius=std::max({high[0]-low[0],high[1]-low[1],high[2]-low[2],.001f});
    const nlVector3 centre{(low[0]+high[0])*.5f,(low[1]+high[1])*.5f,(low[2]+high[2])*.5f};
    const nlVector3 eye{centre.x+radius,centre.y+radius,centre.z+radius},up{0,0,1};
    glMatrixLookAt(matrices.view,eye,centre,up);glMatrixOrthographicCentered(matrices.projection,radius*2,radius*1.5f,.001f,radius*6);
}
void Owned(const std::filesystem::path& folder,const std::filesystem::path& hierarchy,GLView& view,ViewMatrices& matrices,OriginalFrames& frames,AuroraFrames& backend)
{
    auto registry=f::Registry(folder,"",true);auto files=registry->Files();const auto decoded=resources::ReadEffectsGeometry(files->data[2]);
    auto geometry=std::make_shared<EffectsVertexResources>(decoded,resources::ReadTextureBundle(files->data[3]),Drain);
    const resources::EffectsVertexAnimation* animation=nullptr;for(const auto& a:decoded.animations)if(a.model==0xb294c7b7)animation=&a;
    Check(animation,"Owned model animation is absent");ModelParticleInputs inputs;inputs.pose=f::RestPose(HierarchyAsset::Decode(f::Read(hierarchy)));
    unsigned total=0,visible=0;
    for(auto group:{0x9fb76e54U,0xefdbdc68U})for(unsigned spec=0;spec<2;++spec)
    {
        ModelParticles particles(registry,geometry,group,spec,inputs,{64,0x9184eb0c});unsigned changed=0,submitted=0,quiet=0;
        const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(15);
        for(unsigned n=0;n<180;++n)
        {
            Check(std::chrono::steady_clock::now()<deadline,"Owned model-particle gate timed out");particles.Advance(1.f/60);
            Fit(matrices,particles.Sample(),*animation);submitted+=Render(particles,view,frames,backend,n>=12&&quiet>=2);
            const auto pending=mscharged::platform::GetQueuedPipelineCount();quiet=pending?0:quiet+1;
            if(backend.read_colours&&!pending)for(const auto& pixel:backend.colours)
                if(std::abs(int(pixel[0])-20)>5||std::abs(int(pixel[1])-24)>5||std::abs(int(pixel[2])-30)>5)++changed;
        }
        Check(submitted>0&&changed>0,"Owned holotron model-particle pixels were not established");
        total+=submitted;visible+=changed;particles.Die();unsigned drained=0;while(particles.Advance(1.f/60)&&drained<120)++drained;
        Check(drained<120&&particles.Sample().empty(),"Owned model particles failed to drain");particles.Release();
        std::cout<<"Owned group"<<std::hex<<group<<std::dec<<" spec"<<spec<<": "<<submitted<<" models, "<<changed<<" changed-grid samples\n";
    }
    geometry->Release();std::cout<<"Owned retained holotron rest pose: "<<total<<" model submissions, "<<visible<<" visible grid samples; no live NIS actor claim\n";
}
}
int main(int argc,char** argv)
{
    try
    {
        const bool owned=argc==4&&std::string_view(argv[1])=="--owned";Check(owned||argc==2,"Use model_particle_pipeline_tests FIXTURE or --owned EFFECTS_FOLDER HOLOTRON_HIERARCHY");
        const char* base=SDL_GetBasePath();Check(base,"Missing executable directory");const std::string data=std::string(base)+"model-particle-test-data";std::filesystem::create_directories(data);
        AuroraConfig cfg{};cfg.appName="Charged original model-particle checks";cfg.userPath=cfg.cachePath=data.c_str();cfg.resourcesPath=base;cfg.desiredBackend=BACKEND_VULKAN;
        cfg.enableBackendValidation=true;cfg.windowWidth=640;cfg.windowHeight=480;cfg.windowPosX=cfg.windowPosY=-1;cfg.logLevel=LOG_WARNING;cfg.logCallback=Log;cfg.vsync=true;cfg.mem1Size=MEM1_DEFAULT_SIZE;cfg.mem2Size=64*1024*1024;
        Session session;const auto info=aurora_initialize(argc,argv,&cfg);session.live=true;Check(info.window&&info.backend==BACKEND_VULKAN,"Model-particle gate requires Vulkan");
        ImGui::GetIO().IniFilename=nullptr;ImGui::GetIO().LogFilename=nullptr;InitializeStartupOS();nlInitMemory();const auto free1=StandardAllocator.TotalFreeMemory(),free2=VirtualAllocator.TotalFreeMemory();
        glInitResourcePools();InitializeOriginalGraphicsMemory();InitializeOriginalGraphicsState();VIInit();VIConfigure(&GXNtsc480IntDf);alignas(32)std::array<unsigned char,65536> fifo{};GXInit(fifo.data(),fifo.size());AuroraSetViewportPolicy(AURORA_VIEWPORT_FIT);SetGraphicsCacheInvalidator(Invalidate);
        MaterialPrograms materials;OriginalViews views(640,480,Drain);ViewMatrices matrices;glMatrixOrthographicCentered(matrices.projection,4,3,0,1);
        auto* view=new(8,false)GLView(&matrices,{},GLViewSort_None);gRootView.AddChild(view);AuroraFrames backend;OriginalFrames frames(backend);
        if(owned)Owned(argv[2],argv[3],*view,matrices,frames,backend);else Generated(argv[1],*view,frames,backend);
        Check(draws>0,"Original model particles produced no GX draws");glFinish();frames.Release();views.Release();materials.Release();glShutdownMemory();
        Check(StandardAllocator.TotalFreeMemory()==free1&&VirtualAllocator.TotalFreeMemory()==free2,"Model-particle GPU session leaked arenas");ResetStartupFiles();ResetStartupMemory();aurora_shutdown();session.live=false;
        Check(!errors,"Model-particle GPU gate emitted errors");std::cout<<checks<<" model-particle GPU checks passed, "<<draws<<" draws\n";
    }
    catch(const std::exception& e){std::cerr<<"FAILED: "<<e.what()<<'\n';return 1;}
}
