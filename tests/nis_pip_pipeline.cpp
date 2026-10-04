// Real two-camera GX rendering with independent solid-colour composite oracles.
#include "runtime/nis_pip_scene.h"
#include "runtime/materials.h"
#include "runtime/graphics_memory.h"
#include "runtime/graphics_state.h"
#include "runtime/startup.h"
#include "runtime/startup_files.h"
#include "runtime/gpu_readback.h"
#include "Game/GraphicsMemoryStartup.h"
#include "Game/Camera/CameraMan.h"
#include "NL/gl/glMemoryInit.h"
#include "NL/gl/glMatrix.h"
#include "NL/glx/glxTarget.h"
#include "nis_pip_fixture.h"
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
#include <filesystem>
#include <iostream>
using namespace mscharged;
namespace
{
std::atomic_uint errors=0;
void Check(bool okay,const char* message){if(!okay)throw std::runtime_error(message);}
void Log(AuroraLogLevel level,const char*,const char* text,unsigned length)
{if(level>=LOG_ERROR)++errors;std::cerr.write(text,length);std::cerr<<'\n';}
void Drain(){AuroraGXSync();}
void Invalidate(){GXInvalidateVtxCache();GXInvalidateTexAll();}
struct Session
{
 bool live=false,gx=false;
 ~Session(){if(live){if(gx)Drain();glShutdownMemory();ResetStartupFiles();ResetStartupMemory();aurora_shutdown();}}
};
resources::Texture Texture(unsigned id,bool blue)
{
 resources::Texture t;t.id=id;t.width=t.height=4;t.levels=1;t.game_format=3;t.gx_format=6;t.bits={8,8,8,0};t.pixels.resize(64);
 for(unsigned i=0;i<16;++i){t.pixels[2*i]=255;t.pixels[2*i+1]=blue?0:255;t.pixels[32+2*i]=id==102?255:0;t.pixels[33+2*i]=blue?255:0;}return t;
}
resources::StaticModel Plane(unsigned id,float x)
{
 resources::Packet p;p.material.program=0x21db4385;p.material.textures[0]={id,3};p.raster=0xc0007;
 p.vertices={{{x-5,-3,0},{0,0}},{{x+5,-3,0},{1,0}},{{x+5,7,0},{1,1}},{{x-5,7,0},{0,1}}};p.indices={0,1,2,0,2,3};return{id,{p}};
}
void Run()
{
 OriginalCameras core;NisCameraAssets assets(nis_pip_fixture::Fixture(),"pip");
 NisCameraBinding a(assets,0),b(assets,1);NisCameras cameras(core);
 cameras.Select(0,a,0);cameras.Select(1,b,1);cameras.Activate();
 NisPlayback playback(cameras);NisPip pip(playback,1);
 auto lower=Plane(101,20),upper=Plane(102,20);
 for(auto& v:lower.packets[0].vertices)if(v.position[1]>2)v.position[1]=2;
 for(auto& v:upper.packets[0].vertices)if(v.position[1]<2)v.position[1]=2;
 StaticInventory inventory(*glGetCurrentResourcePool(),{Plane(100,10),lower,upper},{Texture(100,false),Texture(101,true),Texture(102,true)},Drain);
 const auto free1=StandardAllocator.TotalFreeMemory(),free2=VirtualAllocator.TotalFreeMemory();
 for(unsigned repeat=0;repeat<2;++repeat)
 {
  NisPipScene scene;
  ViewMatrices primary;
  auto view=std::make_unique<GLView>(&primary,GLRenderPair{},GLViewSort_None);
  view->m_ClearColour=view->m_ClearDepth=true;gRootView.AddChild(view.get());scene.AttachOverlay();
  auto snapshot=[&](const char* name,unsigned blue_mask,unsigned cyan_mask=0,unsigned boundary_mask=0)
  {
   core.Advance(0,0);primary.view=cCameraManager::m_matView;
   glMatrixPerspective(primary.projection,cCameraManager::m_fFOV*3.1415927f/180,4.f/3,.25f,4096.f);
   primary.material_camera=cCameraManager::m_cameraPosition;scene.SetCamera(*cameras.Camera(1));
   ColourSamples samples{};unsigned draws=0;
   const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(12);
   for(unsigned frame=0;frame<20;)
   {
    Check(std::chrono::steady_clock::now()<deadline,"PIP GPU deadline");
    for(auto* event=aurora_update();event->type!=AURORA_NONE;++event)Check(event->type!=AURORA_EXIT,"PIP window closed");
    if(!aurora_begin_frame()){SDL_Delay(1);continue;}
    glplatFrameAllocNextFrame();
    try
    {
     nlMatrix4 identity;identity.SetIdentity();
     for(unsigned id:{100u,101u,102u}){auto* model=inventory.Model(id);glModelSetMatrix(model,identity);scene.Opaque().AttachModel(model,0);view->AttachModel(model,0);}
     scene.Submit(pip);RenderOriginalViews(0,{});GXDrawDone();
    }
    catch(...){aurora_end_frame_no_present();Drain();throw;}
    if(frame==19)samples=EndFrameAndReadColours();else aurora_end_frame();
    draws+=aurora_get_stats()->drawCallCount;++frame;
   }
   std::cout<<name<<":";
   for(unsigned i=0;i<samples.size();++i)
   {
    const auto& c=samples[i];const bool blue=blue_mask&(1u<<i);
    std::cout<<' '<<unsigned(c[0])<<','<<unsigned(c[1])<<','<<unsigned(c[2]);
    Check(std::abs(int(c[0])-(blue?0:255))<=3&&(boundary_mask&(1u<<i)||std::abs(int(c[1])-((cyan_mask&(1u<<i))?255:0))<=3)&&std::abs(int(c[2])-(blue?255:0))<=3,"PIP composite pixel differs from independent rectangle/camera expectation");
   }
   Check(draws>0,"PIP submitted no draws");std::cout<<" draws="<<draws<<'\n';
  };
  pip.SetMode(NisPipMode::Pip);snapshot("fixed PIP",1u<<8);
  pip.SetMode(NisPipMode::Expand);pip.Update(.5f);snapshot("half expansion",(1u<<4)|(1u<<5)|(1u<<7)|(1u<<8),(1u<<4)|(1u<<5));
  pip.Update(.5f);snapshot("exact expansion endpoint",511,7,56);
  pip.Update(.001f);snapshot("post-expansion swapped primary",511,7,56);
  pip.SetMode(NisPipMode::Swap);pip.Update(0);snapshot("swap back and restore PIP",1u<<8);
  pip.SetMode(NisPipMode::Expand);pip.Update(.25f);pip.ResetExpansion();snapshot("reset expansion",1u<<8);
  // Reject the secondary resolve after its geometry was submitted, then restart.
  scene.Alpha().SetViewport(0,0,513,256);
  bool rejected=false;
  try{snapshot("invalid resolve must stop",0);}catch(const std::exception&){rejected=true;}
  scene.Alpha().SetViewport(0,0,512,256);Check(rejected,"Invalid PIP resolve unexpectedly rendered");
  snapshot("recover after rejected frame",1u<<8);
  view.reset();
 }
 Check(StandardAllocator.TotalFreeMemory()==free1&&VirtualAllocator.TotalFreeMemory()==free2,"PIP graph/target restart leaked game arenas");
}
}
int main(int argc,char** argv)
{
 try
 {
  const auto directory=std::filesystem::path(SDL_GetBasePath())/"nis-pip-test-data";std::filesystem::create_directories(directory);const auto path=directory.string();
  AuroraConfig cfg{};cfg.appName="Charged NIS PIP pixel checks";cfg.userPath=cfg.cachePath=path.c_str();cfg.resourcesPath=SDL_GetBasePath();
  cfg.desiredBackend=BACKEND_VULKAN;cfg.enableBackendValidation=true;cfg.windowWidth=640;cfg.windowHeight=480;cfg.windowPosX=cfg.windowPosY=-1;
  cfg.vsync=true;cfg.logLevel=LOG_WARNING;cfg.logCallback=Log;cfg.mem1Size=MEM1_DEFAULT_SIZE;cfg.mem2Size=64*1024*1024;
  Session session;const auto info=aurora_initialize(argc,argv,&cfg);session.live=true;Check(info.backend==BACKEND_VULKAN&&info.window,"PIP requires real Vulkan");
  ImGui::GetIO().IniFilename=nullptr;ImGui::GetIO().LogFilename=nullptr;InitializeStartupOS();nlInitMemory();
  const auto free1=StandardAllocator.TotalFreeMemory(),free2=VirtualAllocator.TotalFreeMemory();
  glInitResourcePools();InitializeOriginalGraphicsMemory();InitializeOriginalGraphicsState();VIInit();VIConfigure(&GXNtsc480IntDf);
  alignas(32)std::array<unsigned char,65536> fifo{};GXInit(fifo.data(),fifo.size());session.gx=true;AuroraSetViewportPolicy(AURORA_VIEWPORT_FIT);SetGraphicsCacheInvalidator(Invalidate);
  MaterialPrograms programs;OriginalViews views(640,480,Drain);Run();views.Release();programs.Release();glShutdownMemory();
  Check(StandardAllocator.TotalFreeMemory()==free1&&VirtualAllocator.TotalFreeMemory()==free2,"PIP shutdown leaked arenas");Check(errors==0,"PIP backend errors");
  std::cout<<"NIS two-camera PIP Vulkan pixel, swap, reset and recovery checks passed\n";return 0;
 }
 catch(const std::exception& e){std::cerr<<"FAILED: "<<e.what()<<'\n';return 1;}
}
