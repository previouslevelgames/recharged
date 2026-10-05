#include "runtime/boot_effects.h"
#include "runtime/startup.h"
#include "runtime/startup_files.h"
#include "runtime/graphics_memory.h"
#include "runtime/graphics_state.h"
#include "runtime/materials.h"
#include "runtime/views.h"
#include "runtime/frames.h"
#include "runtime/static_inventory.h"
#include "runtime/effects_vertex.h"
#include "Game/GL/GLVertexAnim.h"
#include "Game/GL/GLInventory.h"
#include "Game/AsyncLoadingShared.h"
#include "NL/gl/glMemoryInit.h"
#include "NL/gl/gl.h"
#include "NL/gl/glTextureManager.h"
#include "NL/nlFileGC.h"
#include "NL/nlFile.h"
#include <aurora/aurora.h>
#include <aurora/dvd.h>
#include <SDL3/SDL.h>
#include <algorithm>
#include <chrono>
#include <cstring>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <thread>
namespace aurora {extern AuroraConfig g_config;}
void AuroraOSShutdown();
using namespace mscharged;
namespace
{
unsigned checks=0;
void Check(bool value,const char* message){++checks;if(!value)throw std::runtime_error(message);}
template<class F>void Reject(F fn){++checks;try{fn();}catch(const std::exception&){return;}throw std::runtime_error("Invalid boot effects operation accepted");}
void Invalidate(){} // CPU fixture has no GPU-backed caches or outstanding commands.
struct Backend:FrameBackend
{
    unsigned presented=0,discarded=0,waits=0;bool fail_wait=false;
    std::function<void()> callback;
    bool Acquire()override{return true;}void Render()override{}void Drain()override{}void Cancel()noexcept override{}
    void Finish(bool present)override{present?++presented:++discarded;}
    void WaitIdle()override{++waits;if(callback)callback();if(fail_wait)throw std::runtime_error("Injected actual drain failure");}
};
std::vector<std::uint8_t> Read(const char* filename)
{std::ifstream f(filename,std::ios::binary);Check(bool(f),"Cannot read boot script");return{std::istreambuf_iterator<char>(f),{}};}
struct Host
{
    bool dvd=false,files=false;
    std::vector<std::uint64_t> standard=std::vector<std::uint64_t>(4*1024*1024),virtual_arena=std::vector<std::uint64_t>(8*1024*1024);
    Host(const char* path)
    {
        Check(SDL_Init(0),"SDL initialization failed");
        aurora::g_config.mem1Size=MEM1_DEFAULT_SIZE;aurora::g_config.mem2Size=0;
        InitializeStartupOS();
        Check(aurora_dvd_open(path),"Cannot open owned/generated Wii partition");dvd=true;
    }
    ~Host(){if(files)ResetStartupFiles();if(dvd)aurora_dvd_close();if(glGetResourcePools())glShutdownMemory();ResetStartupMemory();AuroraOSShutdown();SDL_Quit();}
};
void Pump(BootLoading& boot)
{
    const auto until=std::chrono::steady_clock::now()+std::chrono::seconds(15);
    while(boot.State()==BootLoadingState::Running)
    {
        nlServiceFileSystem();boot.Update();
        if(std::chrono::steady_clock::now()>=until)throw std::runtime_error("Boot effects timed out");
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}
void Frame(OriginalFrames& frames)
{Check(frames.Acquire(),"CPU frame acquisition failed");glBeginFrame();glEndFrame();glSendFrame();}
}
int main(int argc,char** argv)
{
    try
    {
        std::cout<<std::unitbuf;
        Check(argc==3||argc==4,"Supply disc and boot script, optional failure/owned mode");
        const std::string mode=argc==4?argv[3]:"generated";Host host(argv[1]);
        StandardAllocator.Initialize(host.standard.data(),host.standard.size()*8);VirtualAllocator.Initialize(host.virtual_arena.data(),host.virtual_arena.size()*8);gMemoryInitialized=1;
        nlInitFileSystem();host.files=true;
        const GLMemoryRequirement req[]={{GLM_Header,65536},{GLM_VertexData,65536},{GLM_TextureData,65536}};
        const GLMemoryConfig config{262144,262144,req,3,512};glInitResourcePools();glInitMemory(&config);
        InitializeOriginalGraphicsState();SetGraphicsCacheInvalidator(Invalidate);MaterialPrograms programs;
        OriginalViews views(640,480,Invalidate);Backend backend;OriginalFrames frames(backend);
        const auto a=StandardAllocator.TotalFreeMemory(),b=VirtualAllocator.TotalFreeMemory();
        const auto slots=glGetTextureManager()->mFreeIndices->mCount;
        auto* previous=glGetCurrentResourcePool();const auto script=Read(argv[2]);
        const auto clean=[&]{Check(StandardAllocator.TotalFreeMemory()==a&&VirtualAllocator.TotalFreeMemory()==b
            &&glGetTextureManager()->mFreeIndices->mCount==slots&&glGetCurrentResourcePool()==previous,
            "Boot effects leaked native arenas, texture slots or current pool");};
        if(mode=="failure")
        {
            BootEffectsResources effects;BootLoading boot(script,{}, {},effects.Binding(),BootEffectsResources::NativeMemory);
            boot.Begin();boot.Update();Pump(boot);
            Check(boot.State()==BootLoadingState::Failed&&!boot.Error().empty(),"Malformed effects input became readiness");
            Reject([&]{effects.Registry();});boot.Cancel();
            Check(effects.State()==BootEffectsState::Released||effects.State()==BootEffectsState::Idle,"Failed transaction retained active reads");clean();
            std::cout<<"Expected failure: "<<boot.Error()<<'\n';
        }
        else
        {
            { BootLoading unbound(script);unbound.Begin();unbound.Update();Pump(unbound);
              Check(unbound.State()==BootLoadingState::Blocked&&unbound.Stop()->service==41,
                    "Ordinary boot silently bound particle services");unbound.Cancel();clean(); }
            for(const auto budget:{BootLoadingMemory{0,32},BootLoadingMemory{32,0},BootLoadingMemory{67108865,1}})
                Reject([&]{BootLoading invalid(script,{}, {},{},budget);});
            // A true small fixed pool fails and restores both native allocators.
            { BootEffectsResources effects;BootLoading boot(script,{}, {},effects.Binding(),{128,128});
              boot.Begin();boot.Update();Pump(boot);Check(boot.State()==BootLoadingState::Failed,"Undersized pool grew or succeeded");
              boot.Cancel();clean(); }
            if(mode=="owned")
            { BootEffectsResources effects;BootLoading boot(script,{}, {},effects.Binding());
              boot.Begin();boot.Update();Pump(boot);Check(boot.State()==BootLoadingState::Failed,"Console budget unexpectedly admitted native effects");
              Check(boot.Error().find("bad_alloc")!=std::string::npos,"Default budget failed for a different reason");
              boot.Cancel();clean(); }
            // No NL service has run: cancel outstanding real four-file reads,
            // then restart the same binding and prove it can publish again.
            { BootEffectsResources effects;BootLoading boot(script,{}, {},effects.Binding(),BootEffectsResources::NativeMemory);
              boot.Begin();boot.Update();Check(effects.State()==BootEffectsState::Loading,"Boot did not begin asynchronous effects");
              boot.Cancel();Check(effects.State()==BootEffectsState::Released,"Cancellation retained effects reads");clean();
              boot.Reset();Check(boot.State()==BootLoadingState::Idle,"Reset did not restore original sequence state");
              boot.Begin();boot.Update();Pump(boot);Check(boot.State()==BootLoadingState::Blocked&&boot.Stop()->service==1,"Restart did not reach NPC boundary");
              Frame(frames);boot.Cancel();clean(); }
            for(unsigned repeat=0;repeat<2;++repeat)
            {
                BootEffectsResources effects;const auto binding=effects.Binding();
                Check(effects.Binding()==binding,"Provider minted two conflicting bindings");
                BootLoading boot(script,{}, {},binding,BootEffectsResources::NativeMemory);
                Reject([&]{BootLoading duplicate(script,{}, {},binding);});
                backend.callback=[&]{Reject([&]{boot.Reset();});Reject([&]{effects.Binding();});};
                boot.Begin();boot.Update();backend.callback={};Pump(boot);
                if(auto* observed=const_cast<GLResourcePool*>(boot.PersistentPool()))for(unsigned which=0;which<2;++which)
                {const char* name;unsigned long total,free,peak;observed->GetPoolMemoryInfo(which,&name,&total,&free,&peak,nullptr);
                 std::cout<<name<<" total="<<total<<" used="<<(total-free)<<" peak="<<peak<<'\n';}
                Check(boot.State()==BootLoadingState::Blocked&&boot.Stop()->service==1,"Owned VM did not reach actual NPC-template service1");
                Check(effects.State()==BootEffectsState::Registered&&effects.Pool()==boot.PersistentPool(),"Registered effects used a substitute pool");
                const auto counts=effects.Counts();std::cout<<"Registered templates="<<counts.templates<<" groups="<<counts.groups<<" textures="<<counts.textures<<" models="<<counts.models<<" animations="<<counts.animations<<'\n';
                Check(counts.completed_files==4&&counts.models&&counts.animations&&counts.textures,"Incomplete effects registration");
                auto registry=effects.Registry();Check(registry->Files()!=nullptr,"Original bundle storage was not retained");
                Check(glGetCurrentResourcePool()==previous,"Effects registration leaked current pool");
                const auto calls=boot.Calls();Check(std::count(calls.begin(),calls.end(),41)==1&&std::count(calls.begin(),calls.end(),28)>=1,
                    "Original begin/finalize call order was lost");
                const auto before_calls=calls.size();boot.Update();Check(boot.Calls().size()==before_calls,"Blocked update restarted file registration");
                auto* pool=const_cast<GLResourcePool*>(boot.PersistentPool());
                const auto decoded=resources::ReadEffectsGeometry((*registry->Files())[ParticleFileKind::Geometry]);
                for(const auto& animation:decoded.animations)
                {
                    auto* native=pool->m_inventory->GetVertexAnim(animation.model);
                    Check(native&&native->m_pModel==pool->m_inventory->GetModel(animation.model)&&native->m_pVertices,
                        "Original animation/model inventory link absent");
                    Check(std::memcmp(native->m_pVertices,animation.positions.data(),animation.positions.size()*sizeof(animation.positions[0]))==0,
                        "Retained native animation positions differ from decoded source");
                }
                std::thread foreign([&]{Reject([&]{effects.State();});Reject([&]{boot.Update();});});foreign.join();
                { const auto mark=pool->MarkResource();Reject([&]{effects.Registry();});Reject([&]{boot.Cancel();});pool->ReleaseResource(mark); }
                Check(effects.Registry()==registry,"External-marker rejection changed published ownership");
                // A second transaction cannot shadow an existing actual texture.
                { BootEffectsResources other;BootLoading collision(script,{}, {},other.Binding(),BootEffectsResources::NativeMemory);
                  collision.Begin();collision.Update();Pump(collision);
                  Check(collision.State()==BootLoadingState::Failed&&collision.Error().find("already registered")!=std::string::npos,
                    "Foreign texture registration was not rejected before publication");collision.Cancel(); }
                Check(effects.Registry()==registry&&pool->m_inventory->GetVertexAnim(decoded.animations[0].model),"Rejected collision corrupted prior generation");
                const auto discarded=backend.discarded,presented=backend.presented;
                Frame(frames);Check(backend.discarded==discarded+1,"Original finalize did not discard exactly one frame");
                Frame(frames);Check(backend.presented==presented+1,"Finalize kept discarding later frames");
                backend.fail_wait=true;Reject([&]{boot.Cancel();});backend.fail_wait=false;
                Check(effects.Registry()==registry&&effects.Pool()==pool,"Failed GPU drain retired live resources");
                backend.callback=[&]{Reject([&]{boot.Cancel();});Reject([&]{effects.Registry();});};
                boot.Cancel();backend.callback={};Check(effects.State()==BootEffectsState::Released&&!effects.Pool(),"Boot did not release effects before pool");
                Reject([&]{effects.Registry();});
                Check(registry->Groups()>0,"External retained CPU bundle expired at boot teardown");clean();
            }
            // The concrete provider remains alive through the opaque binding,
            // even if its observation wrapper is destroyed during actual reads.
            { auto effects=std::make_unique<BootEffectsResources>();auto binding=effects->Binding();
              BootLoading boot(script,{}, {},binding,BootEffectsResources::NativeMemory);boot.Begin();boot.Update();effects.reset();
              Pump(boot);Check(boot.State()==BootLoadingState::Blocked&&boot.Stop()->service==1,"Binding failed to retain provider lifetime");
              Frame(frames);boot.Cancel();clean(); }
        }
        frames.Release();views.Release();programs.Release();glShutdownMemory();ResetStartupFiles();host.files=false;
        ResetStartupMemory();std::cout<<checks<<" boot effects checks passed\n";
    }
    catch(const std::exception& e){std::cerr<<"FAILED: "<<e.what()<<" check "<<checks<<'\n';return 1;}
}
