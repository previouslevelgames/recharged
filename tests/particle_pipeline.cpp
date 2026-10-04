#include "runtime/particle_render.h"
#include "runtime/frames.h"
#include "runtime/views.h"
#include "runtime/materials.h"
#include "runtime/static_inventory.h"
#include "runtime/graphics_memory.h"
#include "runtime/graphics_state.h"
#include "runtime/startup.h"
#include "runtime/startup_files.h"
#include "Game/GraphicsMemoryStartup.h"
#include "NL/gl/gl.h"
#include "NL/gl/glMemoryInit.h"
#include "NL/gl/glMatrix.h"
#include "NL/gl/glState.h"
#include <aurora/aurora.h>
#include <aurora/event.h>
#include <aurora/gfx.h>
#include <dolphin/gx.h>
#include <dolphin/gx/GXAurora.h>
#include <dolphin/vi.h>
#include <SDL3/SDL.h>
#include <imgui.h>
#include <array>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
using namespace mscharged;
namespace
{
std::atomic_uint errors=0;unsigned checks=0;
void Check(bool yes,const char* message){++checks;if(!yes)throw std::runtime_error(message);}
void Log(AuroraLogLevel level,const char*,const char* message,unsigned n){if(level>=LOG_ERROR)++errors;std::cerr.write(message,n);std::cerr<<'\n';}
void Drain(){AuroraGXSync();}void Invalidate(){GXInvalidateVtxCache();GXInvalidateTexAll();}
struct Session{bool live=false;~Session(){if(live){Drain();glShutdownMemory();ResetStartupFiles();ResetStartupMemory();aurora_shutdown();}}};
std::vector<std::uint8_t> Read(const std::filesystem::path& path){std::ifstream f(path,std::ios::binary);if(!f)throw std::runtime_error("Missing particle GPU fixture");return {std::istreambuf_iterator<char>(f),{}};}
EffectsRegistry::Handle Load(const std::filesystem::path& folder,const std::string& mode)
{
 auto f=std::make_shared<ParticleFiles>();const std::array<std::string,4> names{mode+".bun",mode=="uv"?"uvnonresident.bun":"nonresident.bun","geometry.bun","textures.rlt"};
 for(unsigned i=0;i<4;++i){f->data[i]=Read(folder/names[i]);f->source_sizes[i]=f->data[i].size();}return EffectsRegistry::FromFiles(f);
}
void Acquire(OriginalFrames& frames)
{
 const auto end=std::chrono::steady_clock::now()+std::chrono::seconds(10);
 for(;;){for(const auto* e=aurora_update();e->type!=AURORA_NONE;++e)Check(e->type!=AURORA_EXIT,"GPU test closed");
  if(frames.Acquire())return;Check(std::chrono::steady_clock::now()<end,"GPU acquisition timeout");SDL_Delay(1);}
}
resources::StaticModel Occluder()
{
 resources::Packet p;p.primitive=0;p.material.program=0x21db4385;p.material.textures[0]={7,3};p.raster=0xc0007;
 p.vertices={{{-2,-2,.5f},{0,0}},{{2,-2,.5f},{1,0}},{{0,2,.5f},{.5f,1}}};p.indices={0,1,2};return {6,{p}};
}
resources::Texture Opaque()
{
 resources::Texture t;t.id=7;t.width=t.height=4;t.levels=1;t.game_format=3;t.gx_format=6;t.bits={8,8,8,0};t.pixels.resize(64);
 for(unsigned i=0;i<16;++i){t.pixels[i*2]=255;t.pixels[i*2+1]=40;t.pixels[i*2+32]=80;t.pixels[i*2+33]=120;}return t;
}
void Pixel(const ColourSamples& colours,std::array<int,3> expected,const char* mode)
{
 const auto& c=colours[4];std::cout<<mode<<" pixel="<<unsigned(c[0])<<','<<unsigned(c[1])<<','<<unsigned(c[2])<<'\n';
 for(unsigned i=0;i<3;++i)Check(std::abs(int(c[i])-expected[i])<=4,"Particle material pixel oracle differs");
}
}
int main(int argc,char** argv)
{
 try{
  Check(argc==2,"Supply particle fixture directory");const auto folder=std::filesystem::path(argv[1]);
  const char* base=SDL_GetBasePath();Check(base,"Missing binary directory");const std::string path=std::string(base)+"particle-test-data";std::filesystem::create_directories(path);
  AuroraConfig cfg{};cfg.appName="Charged original billboard particles";cfg.userPath=cfg.cachePath=path.c_str();cfg.resourcesPath=base;
  cfg.desiredBackend=BACKEND_VULKAN;cfg.enableBackendValidation=true;cfg.windowWidth=640;cfg.windowHeight=480;cfg.windowPosX=cfg.windowPosY=-1;
  cfg.logLevel=LOG_WARNING;cfg.logCallback=Log;cfg.vsync=true;cfg.mem1Size=MEM1_DEFAULT_SIZE;cfg.mem2Size=64*1024*1024;
  Session session;const auto info=aurora_initialize(argc,argv,&cfg);session.live=true;Check(info.window&&info.backend==BACKEND_VULKAN,"Particle GPU test requires Vulkan");
  ImGui::GetIO().IniFilename=nullptr;ImGui::GetIO().LogFilename=nullptr;InitializeStartupOS();nlInitMemory();
  const auto free1=StandardAllocator.TotalFreeMemory(),free2=VirtualAllocator.TotalFreeMemory();
  glInitResourcePools();InitializeOriginalGraphicsMemory();InitializeOriginalGraphicsState();VIInit();VIConfigure(&GXNtsc480IntDf);
  alignas(32)std::array<unsigned char,65536> fifo{};GXInit(fifo.data(),fifo.size());AuroraSetViewportPolicy(AURORA_VIEWPORT_FIT);SetGraphicsCacheInvalidator(Invalidate);
  MaterialPrograms materials;StaticInventory inventory(*glGetCurrentResourcePool(),{Occluder()},{Opaque()},Drain);
  OriginalViews views(640,480,Drain);ViewMatrices matrices;matrices.view.e[14]=-3;
  glMatrixOrthographicCentered(matrices.projection,4,3,0,10);
  auto* view=new(8,false)GLView(&matrices,{},GLViewSort_None);gRootView.AddChild(view);
  AuroraFrames backend;OriginalFrames frames(backend);unsigned total_draws=0;
  const std::array<std::string,8> modes{"normal","additive","transparent","order","occluded","infront","disabled-infront","uv"};
  for(const auto& mode:modes)
  {
   const bool blocked=mode=="occluded"||mode=="infront"||mode=="disabled-infront";
   auto registry=Load(folder,mode=="occluded"?"normal":mode=="disabled-infront"?"infront":mode);
   ParticleSimulation simulation(registry,0x81f2a311,0);simulation.Advance(.25f);if(mode=="order")simulation.Advance(.25f);
   ParticleRenderer renderer(*glGetCurrentResourcePool(),simulation,Drain);
   const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(15);
   unsigned quiet=0;
   for(unsigned i=0;;++i)
   {
    Check(std::chrono::steady_clock::now()<deadline,"Particle pipeline warmup timed out");
    Acquire(frames);glBeginFrame();GXSetPixelFmt(GX_PF_RGB8_Z24,GX_ZC_LINEAR);GXSetCopyClear({20,24,30,255},GX_MAX_Z24);
    if(blocked){nlMatrix4 identity;identity.SetIdentity();auto* m=inventory.Model(6);glModelSetMatrix(m,identity);view->AttachModel(m,0);}
    Check(renderer.Submit(*view,true,1,mode!="disabled-infront")==unsigned(mode=="order"?2:1),"Original GPU particle count differs");
    const bool sample=i>=12&&quiet>=2;
    backend.read_colours=sample;glEndFrame();glSendFrame();renderer.FinishFrame();total_draws+=aurora_get_stats()->drawCallCount;
    const auto pending=std::atomic_ref<const std::uint32_t>(aurora_get_stats()->queuedPipelines).load();
    quiet=pending?0:quiet+1;
    if(sample&&!pending)break;
   }
   if(mode=="normal")Pixel(backend.colours,{110,62,40},mode.c_str());
   else if(mode=="additive")Pixel(backend.colours,{120,74,55},mode.c_str());
   else if(mode=="transparent")Pixel(backend.colours,{20,24,30},mode.c_str());
   else if(mode=="order")Pixel(backend.colours,{55,6,108},mode.c_str());
   else if(mode=="infront")Pixel(backend.colours,{120,90,85},mode.c_str());
   else if(mode=="uv")Pixel(backend.colours,{160,32,0},mode.c_str());
   else Pixel(backend.colours,{40,80,120},mode.c_str());
   // Actual failed-frame cancellation must clear queued original packets before
   // renderer texture release, and a subsequent frame must remain usable.
   Acquire(frames);glBeginFrame();renderer.Submit(*view);frames.Cancel();renderer.FinishFrame();renderer.Release();simulation.Release();
  }
  Check(total_draws>=96,"No actual particle material draws reached GX");
  glFinish();frames.Release();views.Release();inventory.Release();materials.Release();glShutdownMemory();
  Check(StandardAllocator.TotalFreeMemory()==free1&&VirtualAllocator.TotalFreeMemory()==free2,"Particle GPU pipeline leaked game arenas");
  ResetStartupFiles();ResetStartupMemory();aurora_shutdown();session.live=false;Check(!errors,"GPU validation reported an error");
  std::cout<<checks<<" particle GPU checks passed, "<<total_draws<<" draws\n";
 }catch(const std::exception&e){std::cerr<<"FAILED: "<<e.what()<<'\n';return 1;}
}
