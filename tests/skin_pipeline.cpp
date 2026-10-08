#include "platform/graphics_stats.h"
#include "skin_render_fixture.h"
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
#include "NL/nlFile.h"
#include "NL/nlFileGC.h"
#include "NL/nlMemory.h"
#include <aurora/aurora.h>
#include <aurora/dvd.h>
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
#include <filesystem>
#include <iostream>
#include <limits>
#include <thread>
using namespace mscharged;
namespace f=skin_render_fixture;
namespace
{
unsigned checks=0,total_draws=0;std::atomic_uint errors=0;
void Check(bool yes,const char* message){++checks;if(!yes)throw std::runtime_error(message);}
void Log(AuroraLogLevel level,const char*,const char* message,unsigned n){if(level>=LOG_ERROR)++errors;std::cerr.write(message,n);std::cerr<<'\n';}
void Drain(){AuroraGXSync();}void Invalidate(){GXInvalidateVtxCache();GXInvalidateTexAll();}
struct Session
{
 bool live=false,disc=false;
 ~Session(){if(live){Drain();glShutdownMemory();ResetStartupFiles();if(disc)aurora_dvd_close();ResetStartupMemory();aurora_shutdown();}}
};
void Acquire(OriginalFrames& frames)
{
 const auto until=std::chrono::steady_clock::now()+std::chrono::seconds(10);
 for(;;){for(const auto* e=aurora_update();e->type!=AURORA_NONE;++e)Check(e->type!=AURORA_EXIT,"Skin GPU test closed");if(frames.Acquire())return;Check(std::chrono::steady_clock::now()<until,"Skin GPU frame acquisition timed out");SDL_Delay(1);}
}
void Pixel(const ColourSamples& samples,unsigned at,std::array<int,3> expected,const char* name)
{
 const auto& c=samples[at];std::cout<<name<<" sample"<<at<<'='<<unsigned(c[0])<<','<<unsigned(c[1])<<','<<unsigned(c[2])<<'\n';
 for(unsigned i=0;i<3;++i)Check(std::abs(int(c[i])-expected[i])<=4,"Original rigid skin pixel oracle differs");
}
void Render(OriginalFrames& frames,AuroraFrames& backend,GLView& view,SkinRenderer& renderer,SkinPoseFrame::Handle pose)
{
 const auto until=std::chrono::steady_clock::now()+std::chrono::seconds(15);unsigned quiet=0;
 try
 {
  for(unsigned frame=0;;++frame)
  {
   Check(std::chrono::steady_clock::now()<until,"Skin pipeline shader warmup timed out");Acquire(frames);glBeginFrame();
   GXSetPixelFmt(GX_PF_RGB8_Z24,GX_ZC_LINEAR);GXSetCopyClear({20,24,30,255},GX_MAX_Z24);
   Check(renderer.Submit(view,pose)==pose->asset->Data().packets.size(),"Skin packet submission differs");
   const bool sample=frame>=12&&quiet>=2;backend.read_colours=sample;glEndFrame();glSendFrame();renderer.FinishFrame();
   total_draws+=aurora_get_stats()->drawCallCount;
   const auto pending=mscharged::platform::GetQueuedPipelineCount();quiet=pending?0:quiet+1;
   if(sample&&!pending)break;
  }
 }
 catch(...){frames.Cancel();renderer.FinishFrame();throw;}
}
void Generated(OriginalFrames& frames,AuroraFrames& backend,GLView& view,ViewMatrices& matrices)
{
 matrices.view.SetIdentity();matrices.view.m43=-3;glMatrixOrthographicCentered(matrices.projection,4,3,0,10);
 for(const std::string mode:{"base","slot","detail","uv","transparent","alpha","ambient","directional","normal"})
 {
  const bool translated=mode=="slot",flipped=mode=="normal";
  auto asset=f::Asset(translated||flipped?1:0,mode=="detail"||mode=="uv"?0:1,mode=="transparent"?0:mode=="alpha"?.5f:1);
  auto file=f::Rlt(false,mode=="uv");std::array<resources::Bytes,1> input{file};
  auto pose=f::Frame(asset,translated?1.25f:0,flipped);SkinRenderer renderer(*glGetCurrentResourcePool(),asset,input,SkinRenderProfile::Authored,Drain);
  backend.lighting={};backend.lighting.character.emplace();
  if(mode=="ambient") {backend.lighting.enabled=true;backend.lighting.ambient={{64,128,192,0}};}
  if(mode=="directional"||flipped)
  {
   backend.lighting.enabled=true;backend.lighting.ambient={{0,0,0,0}};auto& c=*backend.lighting.character;c.light_count=1;
   c.lights[0].useWorldPosition=1;c.lights[0].worldPosition={0,0,1};c.lights[0].intensity=.5f;
  }
  Render(frames,backend,view,renderer,pose);
  if(mode=="base")Pixel(backend.colours,4,{200,100,50},mode.c_str());
  else if(translated){Pixel(backend.colours,4,{20,24,30},"matrix slot centre");Pixel(backend.colours,5,{200,100,50},"matrix slot right");}
  else if(mode=="detail")Pixel(backend.colours,4,{100,25,38},mode.c_str());
  else if(mode=="uv")Pixel(backend.colours,4,{31,78,16},mode.c_str());
  else if(mode=="transparent")Pixel(backend.colours,4,{20,24,30},mode.c_str());
  else if(mode=="alpha")Pixel(backend.colours,4,{110,62,40},mode.c_str());
  else if(mode=="ambient")Pixel(backend.colours,4,{50,50,38},mode.c_str());
  else if(mode=="directional")Pixel(backend.colours,4,{100,50,25},mode.c_str());
  else Pixel(backend.colours,4,{0,0,0},mode.c_str());
  // A failed render must cancel/drain the retained packets, then recover. No
  // implicit character manager is fabricated even for an unlit material.
  if(mode=="base")
  {
   backend.lighting.character.reset();bool rejected=false;
   try{Acquire(frames);glBeginFrame();renderer.Submit(view,pose);glEndFrame();glSendFrame();}
   catch(const std::logic_error&){rejected=true;}
   frames.Cancel();renderer.FinishFrame();Check(rejected,"Missing character light service was silently accepted");
   backend.lighting.character.emplace();Render(frames,backend,view,renderer,pose);Pixel(backend.colours,4,{200,100,50},"post-failure recovery");
  }
  Acquire(frames);glBeginFrame();renderer.Submit(view,pose);frames.Cancel();renderer.FinishFrame();renderer.Release();
 }
 Check(total_draws>=90,"Original rigid material did not issue actual GX draws");
}
f::Blob ReadDisc(const char* path)
{
 unsigned long size=0;void* data=nlLoadEntireFile(path,&size,32,AllocateStart,nullptr,0,nullptr);
 std::unique_ptr<void,void(*)(void*)> owner(data,nlFree);Check(data&&size,"Owned skin render file read failed");return f::Blob(static_cast<std::uint8_t*>(data),static_cast<std::uint8_t*>(data)+size);
}
void Owned(OriginalFrames& frames,AuroraFrames& backend,GLView& view,ViewMatrices& matrices)
{
 AnimationBundleLoad load;load.BeginCharacter(1);const auto until=std::chrono::steady_clock::now()+std::chrono::seconds(20);
 while(load.State()==AnimationBundleState::Loading){load.Service();Check(std::chrono::steady_clock::now()<until,"Owned animation load timed out");std::this_thread::yield();}
 auto bundle=load.Result();Check(CharacterAnimation(1).name=="bowser","Original Bowser association changed");
 auto bytes=ReadDisc("Art/characters/bowser/bowser_shock.rlg");auto texture=ReadDisc("Art/characters/mario/mario_shock.rlt");
 auto asset=RigidSkinAsset::Decode(bytes,bundle->Hierarchy());SkinPose skin(asset);AnimationPose animation(bundle);
 std::array<resources::Bytes,1> input{texture};SkinRenderer renderer(*glGetCurrentResourcePool(),asset,input,SkinRenderProfile::BowserShock,Drain);
 nlMatrix4 identity;identity.SetIdentity();std::array<double,3> lo,hi;lo.fill(std::numeric_limits<double>::infinity());hi.fill(-std::numeric_limits<double>::infinity());
 // Fit the diagnostic view to actual source-authored positions. The retained
 // hierarchy, bind transforms and animation samples themselves stay unchanged.
 auto sample=[&](float time){AnimationPoseLayer layer{0,time,1,false,false};return skin.Sample(animation.Sample({&layer,1},identity));};
 double first_sum=0,movement=0;
 for(float t:{0.f,.25f,.5f,.75f,1.f})
 {
  auto pose=sample(t);double sum=0;
  for(unsigned p=0;p<asset->Data().packets.size();++p)for(const auto& v:asset->Data().packets[p].vertices)
  {
   const auto& m=pose->packets[p][v.bones[0]].values;double local[3]{};
   for(unsigned r=0;r<3;++r){local[r]=m[r][3];for(unsigned c=0;c<3;++c)local[r]+=m[r][c]*v.position[c];}
   const auto& model=asset->Data().packets[p].matrix;
   for(unsigned c=0;c<3;++c){double x=model[12+c];for(unsigned r=0;r<3;++r)x+=local[r]*model[r*4+c];lo[c]=std::min(lo[c],x);hi[c]=std::max(hi[c],x);sum+=x*(c+1);}
  }
  if(t==0)first_sum=sum;else movement+=std::abs(sum-first_sum);
 }
 Check(movement>1e-3,"Owned sampled skin positions never changed");animation.Reset();skin.Reset();
 const double width=std::max(hi[0]-lo[0],(hi[2]-lo[2])*4/3)*1.25;
 Check(std::isfinite(width)&&width>0&&width<10000,"Invalid owned diagnostic bounds");
 matrices.view.SetIdentity();matrices.view.m22=matrices.view.m33=0;matrices.view.m23=-1;matrices.view.m32=1;
 matrices.view.m41=-(lo[0]+hi[0])/2;matrices.view.m42=-(lo[2]+hi[2])/2;matrices.view.m43=(lo[1]+hi[1])/2-width;
 glMatrixOrthographicCentered(matrices.projection,width,width*.75,0,width*4);
 backend.lighting={};backend.lighting.character.emplace(); // Explicit unlit diagnostic; source material/TEV still executes.
 unsigned visible=0,quiet=0,draws=0,samples=0;const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(25);
 try
 {
  for(unsigned frame=0;frame<180;++frame)
  {
   Check(std::chrono::steady_clock::now()<deadline,"Owned skin GPU run timed out");Acquire(frames);glBeginFrame();GXSetPixelFmt(GX_PF_RGB8_Z24,GX_ZC_LINEAR);GXSetCopyClear({20,24,30,255},GX_MAX_Z24);
   auto pose=sample(float(frame%120)/119);Check(renderer.Submit(view,pose)==4,"Owned Bowser shock packet count changed");backend.read_colours=frame>=12&&quiet>=2;
   glEndFrame();glSendFrame();renderer.FinishFrame();draws+=aurora_get_stats()->drawCallCount;
   const auto pending=mscharged::platform::GetQueuedPipelineCount();quiet=pending?0:quiet+1;
   if(backend.read_colours&&!pending){++samples;for(const auto& c:backend.colours)if(std::abs(int(c[0])-20)>8||std::abs(int(c[1])-24)>8||std::abs(int(c[2])-30)>8)++visible;}
  }
 }
 catch(...){frames.Cancel();renderer.FinishFrame();throw;}
 Check(samples&&visible&&draws>=180*4,"Owned posed skin had no qualified visible samples/draws");
 renderer.Release();skin.Release();animation.Release();std::cout<<"Owned Bowser shock: 180 frames, "<<draws<<" draws, "<<samples<<" readbacks, "<<visible<<" visible samples, movement="<<movement<<"\n";
}
}
int main(int argc,char** argv)
{
 try
 {
  const bool owned=argc==4&&std::string_view(argv[1])=="--owned";Check(owned||argc==1,"Usage: skin_pipeline_tests [--owned DISC OUTPUT_DIRECTORY]");
  const char* base=SDL_GetBasePath();Check(base,"Missing executable directory");const auto path=(owned?std::filesystem::path(argv[3])/"skin-render-data":std::filesystem::path(base)/"skin-render-data").string();std::filesystem::create_directories(path);
  AuroraConfig config{};config.appName="Charged original rigid skin";config.userPath=config.cachePath=path.c_str();config.resourcesPath=base;config.desiredBackend=BACKEND_VULKAN;config.enableBackendValidation=true;
  config.windowWidth=640;config.windowHeight=480;config.windowPosX=config.windowPosY=-1;config.logLevel=LOG_WARNING;config.logCallback=Log;config.vsync=true;config.mem1Size=MEM1_DEFAULT_SIZE;config.mem2Size=64*1024*1024;
  Session session;const auto host=aurora_initialize(argc,argv,&config);session.live=true;Check(host.window&&host.backend==BACKEND_VULKAN,"Skin renderer requires a real Vulkan adapter");ImGui::GetIO().IniFilename=nullptr;ImGui::GetIO().LogFilename=nullptr;
  InitializeStartupOS();nlInitMemory();if(owned){Check(aurora_dvd_open(argv[2]),"Cannot mount owned skin disc");session.disc=true;nlInitFileSystem();}
  const auto a=StandardAllocator.TotalFreeMemory(),b=VirtualAllocator.TotalFreeMemory();
  glInitResourcePools();InitializeOriginalGraphicsMemory();InitializeOriginalGraphicsState();VIInit();VIConfigure(&GXNtsc480IntDf);alignas(32)std::array<unsigned char,65536> fifo{};GXInit(fifo.data(),fifo.size());AuroraSetViewportPolicy(AURORA_VIEWPORT_FIT);SetGraphicsCacheInvalidator(Invalidate);
  MaterialPrograms materials;OriginalViews views(640,480,Drain);ViewMatrices matrices;auto* view=new(32,false)GLView(&matrices,{},GLViewSort_None);gRootView.AddChild(view);
  AuroraFrames backend;OriginalFrames frames(backend);
  if(owned)Owned(frames,backend,*view,matrices);else Generated(frames,backend,*view,matrices);
  glFinish();frames.Release();views.Release();materials.Release();glShutdownMemory();
  Check(StandardAllocator.TotalFreeMemory()==a&&VirtualAllocator.TotalFreeMemory()==b,"Skin renderer leaked game arenas");ResetStartupFiles();if(session.disc){aurora_dvd_close();session.disc=false;}ResetStartupMemory();aurora_shutdown();session.live=false;Check(!errors,"Skin GPU validation errors");
  std::cout<<checks<<" skin GPU checks passed\n";
 }
 catch(const std::exception& e){std::cerr<<"FAILED: "<<e.what()<<'\n';return 1;}
}
