#include "runtime/boot_effects.h"
#include "runtime/boot_npc.h"
#include "skin_render_fixture.h"
#include "Game/Render/NPCLoadSteps.h"
#include "NL/gl/glMemory.h"
#include "NL/glx/GXCharacterSkinCustomMaterialProgram.h"
#include "Game/SHierarchy.h"
#include "Game/SAnim.h"
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
template<class F>void Reject(F fn){++checks;try{fn();}catch(const std::exception&){return;}throw std::runtime_error("Invalid NPC operation accepted");}
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
        if(std::chrono::steady_clock::now()>=until)throw std::runtime_error("NPC loading timed out");
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}
void Write(const std::filesystem::path& path,const skin_fixture::Blob& bytes)
{std::ofstream file(path,std::ios::binary);file.write(reinterpret_cast<const char*>(bytes.data()),bytes.size());Check(bool(file),"Fixture write failed");}
void Fixtures(const char* path)
{
 using namespace skin_fixture;std::filesystem::create_directories(path);
 for(unsigned i=0;i<2;++i)
 {
  const auto name=i?"npc_b":"npc_a";auto hierarchy=Rig(2,false,false,name);auto model=Rlg();auto texture=skin_render_fixture::Rlt();
  Set(model,Find(model,0x1b003),0x10203040+i);
  for(unsigned t=0;t<2;++t){Set(model,Find(model,0x1b016)+t*8,0x12345678+i*16+t);Set(texture,16+t*16,0x12345678+i*16+t);}
  Write(std::filesystem::path(path)/(std::string(name)+".shier"),hierarchy);Write(std::filesystem::path(path)/(std::string(name)+".rlg"),model);Write(std::filesystem::path(path)/(std::string(name)+".rlt"),texture);
 }
 std::vector<Node> nodes(2);for(auto& n:nodes){n.properties=0x10|0x100;n.rotation={{{0,0,0,.5f}}};}
 Write(std::filesystem::path(path)/"npc_b.sanim",Animation(1,nodes,Hash("npc_b"),0x91929394));
}
void Rules()
{
 for(unsigned flags=0;flags<16;++flags)for(unsigned long id:{~0UL,0UL,0x12345678UL})
 {const bool expected=(!(flags&1)||(flags&2))&&(flags&4)&&(flags&8)&&id!=~0UL;Check(NPCResourcesFinished(flags&1,flags&2,flags&4,flags&8,id)==expected,"Shared original NPC completion predicate changed");}
 Check(std::string(NPCAnimationPathFormat())=="art/animation/%s.sanim.zlib"&&std::string(NPCHierarchyPathFormat())=="art/animation/%s.shier"&&std::string(NPCTexturesPathFormat())=="art/characters/npcs/%s/%s.rlt"&&std::string(NPCModelsPathFormat())=="art/characters/npcs/%s/%s.rlg","Source file path contract changed");
}
void Frame(OriginalFrames& frames)
{Check(frames.Acquire(),"CPU frame acquisition failed");glBeginFrame();glEndFrame();glSendFrame();}
}
int main(int argc,char** argv)
{
 try
 {
    std::cout<<std::unitbuf;if(argc==3&&std::string(argv[1])=="--fixtures"){Fixtures(argv[2]);return 0;}Rules();
    Check(argc>=3&&argc<=5,"Supply disc, script and optional mode/name");const std::string mode=argc>3?argv[3]:"owned";Host host(argv[1]);
    StandardAllocator.Initialize(host.standard.data(),host.standard.size()*8);VirtualAllocator.Initialize(host.virtual_arena.data(),host.virtual_arena.size()*8);gMemoryInitialized=1;
    nlInitFileSystem();host.files=true;
    const GLMemoryRequirement req[]={{GLM_Header,65536},{GLM_VertexData,65536},{GLM_TextureData,65536}};
    const GLMemoryConfig config{262144,262144,req,3,512};glInitResourcePools();glInitMemory(&config);
    InitializeOriginalGraphicsState();SetGraphicsCacheInvalidator(Invalidate);MaterialPrograms programs;
    OriginalViews views(640,480,Invalidate);Backend backend;OriginalFrames frames(backend);
    const auto a=StandardAllocator.TotalFreeMemory(),b=VirtualAllocator.TotalFreeMemory();
    const auto slots=glGetTextureManager()->mFreeIndices->mCount;auto* previous=glGetCurrentResourcePool();const auto script=Read(argv[2]);
    if(mode=="owned")
    {
      for(unsigned repeat=0;repeat<2;++repeat)
      {
        BootEffectsResources effects;BootNpcResources npcs;
        BootLoading boot(script,{}, {},effects.Binding(),BootEffectsResources::NativeMemory,npcs.Binding());
        const auto* allocator=CurrentAllocator;const auto depth=AllocatorStackDepth;
        boot.Begin();boot.Update();Pump(boot);
        std::cout<<"Boot state="<<int(boot.State())<<" created="<<npcs.Created()<<" loaded="<<npcs.Loaded()<<" pending="<<npcs.PendingName()<<" error="<<boot.Error()<<'\n';
        Check(boot.State()==BootLoadingState::Failed&&boot.Error().find("ChainChomp")!=std::string::npos&&boot.Error().find("0x22cadb20")!=std::string::npos,"Owned boot did not expose exact next unsupported NPC material");
        Check(npcs.Created()==16&&npcs.Loaded()==1&&npcs.PendingName()=="ChainChomp","Owned NPC order/count differs from function14");
        auto retained=npcs.Find("angrybanana");auto* model=npcs.Model("AngryBanana");
        Check(model&&model->id==retained->skin->Data().hash&&retained->hierarchy->Data().GetHashID()==0x75137aa1&&retained->animations.empty()&&!retained->animation_admitted,"Actual first template identity/optional animation mismatch");
        Check(CurrentAllocator==allocator&&AllocatorStackDepth==depth,"NPC loading leaked original allocator scope");
        Reject([&]{npcs.Find("ChainChomp");});
        backend.fail_wait=true;Reject([&]{boot.Cancel();});backend.fail_wait=false;
        Check(npcs.Find("AngryBanana")==retained,"Failed draw drain retired prior template resources");
        unsigned drains=0;backend.callback=[&]{Reject([&]{boot.Cancel();});if(++drains==1)Reject([&]{npcs.State();});};boot.Cancel();backend.callback={};
        Check(retained->hierarchy->Data().GetNumNodes()==3,"Retained NPC hierarchy expired on cancellation");
        Check(StandardAllocator.TotalFreeMemory()==a&&VirtualAllocator.TotalFreeMemory()==b&&glGetTextureManager()->mFreeIndices->mCount==slots&&glGetCurrentResourcePool()==previous,"NPC boot teardown leaked arenas or inventory");
      }
    }
    else
    {
      BootNpcResources npcs;auto binding=npcs.Binding();
      Check(npcs.Binding()==binding,"NPC binding identity was not stable");
      BootLoading boot(script,{}, {},{},mode=="oom"?BootLoadingMemory{4096,192}:BootEffectsResources::NativeMemory,binding);
      Reject([&]{BootLoading duplicate(script,{}, {},{},BootEffectsResources::NativeMemory,binding);});
      std::exception_ptr wrong_thread;std::thread wrong([&]{try{npcs.State();}catch(...){wrong_thread=std::current_exception();}});wrong.join();Check(bool(wrong_thread),"Foreign-thread NPC observation accepted");
      const auto* allocator=CurrentAllocator;const auto depth=AllocatorStackDepth;
      boot.Begin();boot.Update();
      if(mode=="valid"||mode=="cancel"||mode=="collision"||mode=="slots")
      {
        Check(npcs.State()==BootNpcState::Loading&&npcs.PendingName()=="npc_b","Persistent-first selection did not override transient creation order");
        Check(CurrentAllocator==allocator&&AllocatorStackDepth==depth,"Async admission left an allocator pushed");
        Check(npcs.Loaded()==0,"Pending callbacks published an NPC prematurely");
        Reject([&]{npcs.Find("npc_b");});
      }
      if(mode=="cancel")
      {
        boot.Cancel();Check(npcs.State()==BootNpcState::Released,"In-flight NPC cancellation did not release requests");
        boot.Reset();boot.Begin();boot.Update();
      }
      std::vector<u16> reserved;
      if(mode=="slots")while(glGetTextureManager()->mFreeIndices->mCount)reserved.push_back(glGetTextureManager()->mFreeIndices->RemoveStart());
      Pump(boot);
      for(auto index:reserved)glGetTextureManager()->mFreeIndices->AddEnd(index);
      std::cout<<"Mode="<<mode<<" state="<<int(boot.State())<<" loaded="<<npcs.Loaded()<<" error="<<boot.Error()<<'\n';
      const bool success=mode=="valid"||mode=="cancel"||mode=="single";
      BootNpcTemplate::Handle retained;
      if(success)
      {
        Check(boot.State()==BootLoadingState::Blocked&&boot.Stop()->service==81,"Registered NPC sequence did not stop at real cache/THP service81");
        Check(npcs.Loaded()==(mode=="single"?1:2),"Registered NPC count differs");
        const auto name=mode=="single"?std::string(argv[4]):"npc_b";retained=npcs.Find(name);const auto* model=npcs.Model(name);
        Check(model&&model->id==retained->skin->Data().hash&&model->numPackets==retained->skin->Data().packets.size(),"Native model lookup differs from retained source identity");
        for(unsigned p=0;p<model->numPackets;++p)
        {
          const auto& actual=model->packets[p];const auto& expected=retained->skin->Data().packets[p];
          Check(actual.numStreams==6&&actual.numUniqueVertices==expected.vertices.size()&&actual.numVertices==expected.indices.size(),"Native skin packet counts differ");
          const auto* material=static_cast<const GXCharacterSkinCustomParameters*>(actual.materialParameters);
          Check(material->skinMatrices==nullptr&&material->skinMatrixBytes==0,"Unposed NPC falsely published matrix readiness");
          for(unsigned v=0;v<actual.numUniqueVertices;++v)
          {
            const auto* position=static_cast<const unsigned char*>(actual.streams[0].address)+v*12;
            Check(std::memcmp(position,expected.vertices[v].position.data(),12)==0,"Native vertex bytes changed");
          }
          for(unsigned i=0;i<actual.numVertices;++i)Check(actual.indexBuffer[i]==expected.indices[i],"Native index bytes changed");
        }
        if(mode!="single")
        {
          Check(retained->animation_admitted&&retained->animations.size()==1&&retained->animations[0]->Data().GetHashID()==0x91929394,"Admitted animation was ignored or changed");
          const auto* xyz=static_cast<const float*>(model->packets[0].streams[0].address);
          Check(xyz[0]==1&&xyz[1]==-2&&xyz[2]==3&&model->packets[0].indexBuffer[0]==0&&model->packets[0].indexBuffer[1]==1&&model->packets[0].indexBuffer[2]==2,"Independent generated position/index oracle differs");
          Check(!npcs.Find("npc_a")->animation_admitted&&npcs.Find("npc_a")->animations.empty(),"Absent optional animation fabricated data");
          Check(boot.PersistentPool()->m_inventory->GetModel(0x10203041)==npcs.Model("npc_b")&&previous->m_inventory->GetModel(0x10203040)==npcs.Model("npc_a"),"Persistent/transient pool identity changed");
        }
        else Check(retained->animation_admitted==(name=="crystalbeam"),"Owned optional-animation admission changed");
        auto* pool=const_cast<GLResourcePool*>(boot.PersistentPool());const auto extra=pool->MarkResource();
        Reject([&]{boot.Cancel();});
        Check(npcs.Find(name)==retained&&(mode=="single"||npcs.Model("npc_a")!=nullptr),"Foreign marker rejection partially retired NPC registrations");
        pool->ReleaseResource(extra);
        backend.fail_wait=true;Reject([&]{boot.Cancel();});backend.fail_wait=false;
        Check(npcs.Find(name)==retained,"Failed drain retired retained model");
        Frame(frames); // Real original frame entry points with explicitly CPU-only backend.
      }
      else
      {
        Check(boot.State()==BootLoadingState::Failed,"Invalid NPC data or lifecycle falsely succeeded");
        if(mode=="collision"||mode=="unsupported")
        {
          Check(npcs.Loaded()==1&&npcs.Find("npc_b")!=nullptr,"Failed second template discarded the first");
          if(mode=="unsupported")Check(boot.Error().find("npc_a")!=std::string::npos&&boot.Error().find("0x22cadb20")!=std::string::npos,"Unsupported error omitted template/material identity");
        }
      }
      Check(CurrentAllocator==allocator&&AllocatorStackDepth==depth,"NPC callbacks changed ambient allocator identity");
      boot.Cancel();Reject([&]{npcs.Model("npc_b");});
      Check(npcs.Created()==0&&npcs.Loaded()==0,"NPC release retained registration records");
      if(retained)Check(retained->hierarchy->Data().GetNumNodes()>0&&retained->skin->Data().packets.size()>0,"Retained CPU data expired with GL pool");
      Check(StandardAllocator.TotalFreeMemory()==a&&VirtualAllocator.TotalFreeMemory()==b&&glGetTextureManager()->mFreeIndices->mCount==slots&&glGetCurrentResourcePool()==previous,"NPC teardown leaked arenas/inventory/slots");
    }
    frames.Release();views.Release();programs.Release();glShutdownMemory();ResetStartupFiles();host.files=false;ResetStartupMemory();
    std::cout<<checks<<" NPC boot checks passed\n";
 }
 catch(const std::exception& e){std::cerr<<"FAILED: "<<e.what()<<" check "<<checks<<'\n';return 1;}
}
