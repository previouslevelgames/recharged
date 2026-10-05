// Synthetic pixels for the genuine Specular program. Matrix/geometry fixtures
// qualify transport and TEV; they do not qualify original actor pose animation.
#include "runtime/specular_material.h"
#include "runtime/materials.h"
#include "runtime/views.h"
#include "runtime/gpu_readback.h"
#include "runtime/graphics_memory.h"
#include "runtime/graphics_state.h"
#include "runtime/startup.h"
#include "runtime/startup_files.h"
#include "Game/GraphicsMemoryStartup.h"
#include "NL/gl/glMemoryInit.h"
#include "NL/gl/glMemory.h"
#include "NL/gl/glMatrix.h"
#include "NL/gl/glState.h"
#include "NL/gl/glTexture.h"
#include "NL/glx/GXSpecularMaterialProgram.h"
#include "NL/glx/glxTexture.h"
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
unsigned checks=0,total_draws=0;std::atomic_uint errors=0;
void Check(bool ok,const char* message){++checks;if(!ok)throw std::runtime_error(message);}
void Log(AuroraLogLevel level,const char*,const char* message,unsigned n){if(level>=LOG_ERROR)++errors;std::cerr.write(message,n);std::cerr<<'\n';}
void Drain(){AuroraGXSync();}void Invalidate(){GXInvalidateVtxCache();GXInvalidateTexAll();}
struct Session{bool live=false;~Session(){if(live){Drain();glShutdownMemory();ResetStartupFiles();ResetStartupMemory();aurora_shutdown();}}};
struct Fixture
{
 std::array<std::array<float,3>,3> positions{{{-.8f,-.8f,-.3f},{.8f,-.8f,-.3f},{0,.8f,-.3f}}},normals{{{0,0,1},{0,0,1},{0,0,1}}};
 std::array<std::array<short,2>,3> uv{{{128,128},{128,128},{128,128}}};std::array<std::array<u8,4>,3> bones{};
 std::array<std::array<float,4>,3> weights{{{1,0,0,0},{1,0,0,0},{1,0,0,0}}};std::array<u16,3> indices{0,1,2};glModelStream streams[7]{};
 GXSpecularParameters parameters{};glModelPacket packet{};glModel model{123,1,&packet};float matrices[2][3][4]{{{1,0,0,0},{0,1,0,0},{0,0,1,0}},{{1,0,0,1.25f},{0,1,0,0},{0,0,1,0}}};
 Fixture()
 {
  void* pointers[]{positions.data(),normals.data(),uv.data(),uv.data(),uv.data(),bones.data(),weights.data()};const unsigned ids[]{1,2,4,4,4,7,5},strides[]{12,12,4,4,4,4,16};
  for(unsigned s=0;s<7;++s)streams[s]={pointers[s],u8(s),u8(strides[s]),u8(ids[s]),0};packet.indexBuffer=indices.data();packet.numVertices=3;packet.numUniqueVertices=3;packet.numStreams=7;packet.streams=streams;packet.rasterState=0xc0007;
  resources::SpecularSkinMaterial material;for(unsigned i=0;i<3;++i)material.textures[i]={100+i,3};material.blend=material.alpha=1;material.specular_level=0;material.specular_exponent=64;material.specular_colour={1,1,1,1};material.shadow_level=UINT32_MAX;material.lighting_enabled=1;
  InstallSpecularMaterial(packet,material,parameters);parameters.skinMatrices=matrices;parameters.skinMatricesSize=sizeof(matrices);
 }
};
void Pixels(Fixture& f,ViewMatrices& matrices,const GameLighting& lighting,const char* name,std::array<unsigned char,3> expected,unsigned sample=4)
{
 auto submitted=std::make_unique<GLView>(&matrices,GLRenderPair{},GLViewSort_None);gRootView.AddChild(submitted.get());unsigned quiet=0,draws=0;ColourSamples colours{};nlMatrix4 identity;identity.SetIdentity();
 const auto until=std::chrono::steady_clock::now()+std::chrono::seconds(15);
 for(unsigned frame=0;;++frame)
 {
  Check(std::chrono::steady_clock::now()<until,"Specular shader/readback deadline exceeded");for(const auto* e=aurora_update();e->type!=AURORA_NONE;++e)Check(e->type!=AURORA_EXIT,"Specular test window closed");if(!aurora_begin_frame()){SDL_Delay(1);continue;}
  glSetCurrentMatrix(glGetIdentityMatrix());glplatFrameAllocNextFrame();glModelSetMatrix(&f.model,identity);GXSetPixelFmt(GX_PF_RGB8_Z24,GX_ZC_LINEAR);GXSetCopyClear({20,24,30,255},GX_MAX_Z24);submitted->AttachModel(&f.model,0);
  try{RenderOriginalViews(0,lighting);}catch(...){glSetCurrentMatrix(glGetIdentityMatrix());aurora_end_frame();throw;}glSetCurrentMatrix(glGetIdentityMatrix());GXDrawDone();
  const bool read=frame>=12&&quiet>=2;if(read)colours=EndFrameAndReadColours();else aurora_end_frame();draws+=aurora_get_stats()->drawCallCount;
  const auto pending=std::atomic_ref<const std::uint32_t>(aurora_get_stats()->queuedPipelines).load();quiet=pending?0:quiet+1;if(read&&!pending)break;
 }
 const auto& pixel=colours[sample];for(unsigned c=0;c<3;++c)Check(std::abs(int(pixel[c])-expected[c])<=4,"Original Specular pixel oracle differs");
 Check(draws>0||f.parameters.alphaValue==0,"Specular source issued no GX geometry");total_draws+=draws;std::cout<<name<<": RGB "<<unsigned(pixel[0])<<','<<unsigned(pixel[1])<<','<<unsigned(pixel[2])<<", draws "<<draws<<'\n';Drain();
}
}
int main(int argc,char**argv)
{
 try
 {
  Check(argc==1,"Usage: specular_pipeline_tests");const char* base=SDL_GetBasePath();Check(base,"Missing executable directory");const auto path=(std::filesystem::path(base)/"specular-data").string();std::filesystem::create_directories(path);
  AuroraConfig config{};config.appName="Charged original Specular";config.userPath=config.cachePath=path.c_str();config.resourcesPath=base;config.desiredBackend=BACKEND_VULKAN;config.enableBackendValidation=true;config.windowWidth=640;config.windowHeight=480;config.windowPosX=config.windowPosY=-1;config.logLevel=LOG_WARNING;config.logCallback=Log;config.vsync=true;config.mem1Size=MEM1_DEFAULT_SIZE;config.mem2Size=64*1024*1024;
  Session session;const auto host=aurora_initialize(argc,argv,&config);session.live=true;Check(host.window&&host.backend==BACKEND_VULKAN,"Specular requires a real Vulkan adapter");ImGui::GetIO().IniFilename=nullptr;ImGui::GetIO().LogFilename=nullptr;InitializeStartupOS();nlInitMemory();
  const auto free1=StandardAllocator.TotalFreeMemory(),free2=VirtualAllocator.TotalFreeMemory();glInitResourcePools();InitializeOriginalGraphicsMemory();InitializeOriginalGraphicsState();VIInit();VIConfigure(&GXNtsc480IntDf);alignas(32)std::array<u8,65536>fifo{};GXInit(fifo.data(),fifo.size());AuroraSetViewportPolicy(AURORA_VIEWPORT_FIT);SetGraphicsCacheInvalidator(Invalidate);
  MaterialPrograms programs;OriginalViews views(640,480,Drain);auto& pool=*glGetCurrentResourcePool();const auto mark=pool.MarkResource();std::array<std::array<u8,64>,4>pixels{};PlatTexture textures[4];const std::array<std::array<u8,4>,4>colours{{{200,100,50,255},{128,64,192,255},{0,0,0,255},{255,255,255,255}}};
  for(unsigned t=0;t<4;++t){for(unsigned i=0;i<16;++i){const auto c=t==0&&i%4>=2?std::array<u8,4>{20,40,200,255}:colours[t];pixels[t][i*2]=c[3];pixels[t][i*2+1]=c[0];pixels[t][i*2+32]=c[1];pixels[t][i*2+33]=c[2];}textures[t].m_Width=textures[t].m_Height=4;textures[t].m_Levels=textures[t].m_MaxLevel=1;textures[t].m_Format=GXTex_RGBA8;textures[t].m_SwizzledData=pixels[t].data();textures[t].m_NativeDataBytes=64;glRegisterTexture(100+t,textures+t,&pool);}
  Fixture fixture;ViewMatrices matrices;matrices.view.SetIdentity();glMatrixOrthographicCentered(matrices.projection,4,3,0,1);GameLighting lighting;lighting.ramp_texture=103;
  Pixels(fixture,matrices,lighting,"Specular diffuse",{200,100,50});fixture.parameters.blendAmount=0;Pixels(fixture,matrices,lighting,"Specular detail",{100,25,38});fixture.parameters.blendAmount=.5f;Pixels(fixture,matrices,lighting,"Specular authored additive detail blend",{200,75,50});fixture.parameters.blendAmount=1;
  fixture.parameters.alphaValue=.5f;Pixels(fixture,matrices,lighting,"Specular two-pass alpha",{110,62,40});fixture.parameters.alphaValue=0;Pixels(fixture,matrices,lighting,"Specular alpha zero",{20,24,30});fixture.parameters.alphaValue=1;
  for(auto& uv:fixture.uv)uv={896,128};Pixels(fixture,matrices,lighting,"Specular signed16 UV",{20,40,200});for(auto& uv:fixture.uv)uv={128,128};
  for(auto& b:fixture.bones)b[0]=1;Pixels(fixture,matrices,lighting,"Specular palette shift centre",{20,24,30});Pixels(fixture,matrices,lighting,"Specular palette shift right",{200,100,50},5);for(auto& b:fixture.bones)b[0]=0;
  lighting.enabled=true;lighting.ambient={{64,128,192,0}};Pixels(fixture,matrices,lighting,"Specular ambient",{50,50,38});lighting.ambient={{0,0,0,0}};lighting.light_count=1;lighting.lights[0].useWorldPosition=true;lighting.lights[0].worldPosition={0,0,1};lighting.lights[0].intensity=.5f;Pixels(fixture,matrices,lighting,"Specular directional",{100,50,25});for(auto& normal:fixture.normals)normal={0,0,-1};Pixels(fixture,matrices,lighting,"Specular normal direction",{0,0,0});
  Check(total_draws>=100,"Specular native transport did not draw all pixel cases");Drain();pool.ReleaseResource(mark);views.Release();programs.Release();glShutdownMemory();Check(StandardAllocator.TotalFreeMemory()==free1&&VirtualAllocator.TotalFreeMemory()==free2,"Specular GPU session leaked arenas");ResetStartupFiles();ResetStartupMemory();aurora_shutdown();session.live=false;Check(!errors,"Specular Aurora validation errors");std::cout<<checks<<" Specular GPU checks passed\n";
 }catch(const std::exception&e){std::cerr<<"FAILED: "<<e.what()<<'\n';return 1;}
}
