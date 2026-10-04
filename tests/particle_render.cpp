#include "runtime/particle_render.h"
#include "runtime/frames.h"
#include "runtime/views.h"
#include "runtime/materials.h"
#include "runtime/graphics_state.h"
#include "runtime/graphics_memory.h"
#include "runtime/startup.h"
#include "Game/Effects/ParticleBillboard.h"
#include "NL/gl/gl.h"
#include "NL/gl/glMemoryInit.h"
#include "NL/gl/glMemory.h"
#include "NL/gl/glTexture.h"
#include "NL/gl/glTextureManager.h"
#include "NL/glx/glxTexture.h"
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <source_location>
#include <thread>
using namespace mscharged;
namespace
{
unsigned checks=0,drains=0,packets=0;
void Check(bool yes,const char* message){++checks;if(!yes)throw std::runtime_error(message);}
template<class F>void Reject(F f,std::source_location at=std::source_location::current())
{++checks;try{f();}catch(const std::exception&){return;}throw std::runtime_error("Invalid particle render accepted at line "+std::to_string(at.line()));}
void Near(float a,float b){Check(std::isfinite(a)&&std::abs(a-b)<.00001f,"Particle stream differs");}
void Drain(){++drains;}
std::vector<std::uint8_t> Read(const std::filesystem::path& path)
{std::ifstream f(path,std::ios::binary);if(!f)throw std::runtime_error("Cannot open render fixture");return {std::istreambuf_iterator<char>(f),{}};}
EffectsRegistry::Handle Load(const std::filesystem::path& folder,const std::string& mode)
{
 auto files=std::make_shared<ParticleFiles>();const std::array<std::string,4> names{mode+".bun","nonresident.bun","geometry.bun","textures.rlt"};
 for(unsigned i=0;i<4;++i){files->data[i]=Read(folder/names[i]);files->source_sizes[i]=files->data[i].size();}
 return EffectsRegistry::FromFiles(files);
}
const glModelPacket* observed=nullptr;
void Inspect(GLView*,unsigned long,const glModelPacket* p){if(p){observed=p;++packets;}}
struct GuardedMatrices:ViewMatrices
{
 ParticleRenderer* renderer=nullptr;GLView* view=nullptr;
 void GetViewMatrix(nlMatrix4& output)const override
 {
  Reject([&]{renderer->Release();});Reject([&]{renderer->FinishFrame();});Reject([&]{renderer->Submit(*view);});
  output=this->view_matrix();
 }
 const nlMatrix4& view_matrix()const{return ViewMatrices::view;}
};
struct Backend:FrameBackend
{
 GLView* view=nullptr;
 bool Acquire()override{return true;}void Render()override{if(view)view->Iterate(Inspect);}
 void Finish(bool)override{}void Drain()override{}void WaitIdle()override{}void Cancel()noexcept override{}
};
void Frame(OriginalFrames& frames){Check(frames.Acquire(),"CPU frame acquisition failed");glBeginFrame();}
void Send(ParticleRenderer& renderer){glEndFrame();glSendFrame();renderer.FinishFrame();}
void Session(const std::filesystem::path& folder)
{
 const GLMemoryRequirement req[]={{GLM_Header,65536},{GLM_VertexData,65536},{GLM_TextureData,65536}};
 const GLMemoryConfig config{65536,65536,req,3,4};
 glInitResourcePools();glInitMemory(&config);InitializeOriginalGraphicsState();SetGraphicsCacheInvalidator(Drain);
 MaterialPrograms materials;OriginalViews views(640,480,Drain);ViewMatrices matrices;
 auto* view=new(8,false)GLView(&matrices,{},GLViewSort_None);gRootView.AddChild(view);
 Backend backend;backend.view=view;OriginalFrames frames(backend);auto& pool=*glGetCurrentResourcePool();
 auto registry=Load(folder,"basic");ParticleSimulation simulation(registry,0x81f2a311,0);
 const auto free=pool.GetFreeMemory();const auto slots=glGetTextureManager()->mFreeIndices->mCount;
 Reject([&]{ParticleRenderer bad(pool,simulation,nullptr);});
 {
  ParticleRenderer renderer(pool,simulation,Drain);const auto index=renderer.TextureIndex();
  Check(index!=0xffff&&glGetTextureIndex(0x12345678)==index&&glGetTextureManager()->mFreeIndices->mCount==slots-1,"Actual texture registration failed");
  Reject([&]{ParticleRenderer duplicate(pool,simulation,Drain);});
  Reject([&]{renderer.Submit(*view);});renderer.FinishFrame();
  std::thread wrong([&]{Reject([&]{renderer.TextureIndex();});Reject([&]{renderer.Release();});});wrong.join();
  Frame(frames);Check(renderer.Submit(*view)==0,"Empty emitter generated a mesh");frames.Cancel();renderer.FinishFrame();
  simulation.Advance(.25f);const auto expected=simulation.Sample();
  Frame(frames);Check(renderer.Submit(*view,false)==0,"Invisible emitter submitted geometry");
  for(float aspect:{0.f,-1.f,17.f,INFINITY})Reject([&]{renderer.Submit(*view,true,aspect);});
  Check(renderer.Submit(*view,true,2)==2,"Particle count was lost at submission");
  Reject([&]{renderer.Submit(*view);});Reject([&]{renderer.Release();});Reject([&]{renderer.FinishFrame();});
  // Inspect before frame storage advances. The view owns source-order packets,
  // while the original writer retains signed16 UVs and exact RGBA bytes.
  view->Iterate(Inspect);Check(observed&&observed->numUniqueVertices==8&&observed->primType==GLP_QuadList&&!observed->indexBuffer,"Original procedural packet changed");
  const auto& p=*observed;Check(p.streams[0].stride==12&&p.streams[1].stride==4&&p.streams[2].stride==4,"Original stream widths changed");
  const auto* pos=static_cast<const float*>(p.streams[0].address);const auto* uv=static_cast<const short*>(p.streams[1].address);const auto* colour=static_cast<const unsigned char*>(p.streams[2].address);
  for(unsigned i=0;i<8;++i){Near(pos[i*3],expected[i/4].position[i%4][0]*2);Near(pos[i*3+1],expected[i/4].position[i%4][1]);
   Check(uv[i*2]==short(expected[i/4].uv[i%4][0]*1024)&&uv[i*2+1]==short(expected[i/4].uv[i%4][1]*1024),"Source UV quantization differs");
   for(unsigned c=0;c<4;++c)Check(colour[i*4+c]==expected[i/4].colour[c],"Native width/endianness corrupted colour bytes");}
  auto* binding=static_cast<glTextureBinding*>(p.materialParameters);Check(binding->texture==0x12345678&&binding->textureIndex==index&&!binding->flags,"Particle binding lost real hash/index/wrap");
  Check(glGetRasterState(p.rasterState,GLS_DepthWrite)==0&&glGetRasterState(p.rasterState,GLS_DepthTest)==1&&glGetRasterState(p.rasterState,GLS_AlphaBlend)==1&&glGetRasterState(p.rasterState,GLS_Culling)==1,"Source particle raster differs");
  Send(renderer);
  Frame(frames);GuardedMatrices guarded;guarded.renderer=&renderer;guarded.view=view;view->m_Interface=&guarded;
  renderer.Submit(*view);view->m_Interface=&matrices;frames.Cancel();renderer.FinishFrame();
  const auto nested=pool.MarkResource();Reject([&]{renderer.Release();});pool.ReleaseResource(nested);
  renderer.Release();renderer.Release();Check(!renderer.Active(),"Renderer stayed active after release");
 }
 Check(pool.GetFreeMemory()==free&&glGetTextureManager()->mFreeIndices->mCount==slots&&glGetTextureIndex(0x12345678)==0xffff,"Particle texture rollback leaked pool or index");
 // Exhaust real manager slots through registration, then test rollback.
 {
  const auto full_mark=pool.MarkResource();PlatTexture held[4];
  for(unsigned i=0;i<4;++i)glRegisterTexture(100+i,held+i,&pool);
  Check(glGetTextureManager()->mFreeIndices->mCount==0,"Fixture did not exhaust real texture slots");
  const auto before_failure=pool.GetFreeMemory();
  Reject([&]{ParticleRenderer renderer(pool,simulation,Drain);});
  Check(pool.GetFreeMemory()==before_failure&&glGetTextureIndex(0x12345678)==0xffff,"Failed registration left a texture behind");
  pool.ReleaseResource(full_mark);
 }
 Check(pool.GetFreeMemory()==free&&glGetTextureManager()->mFreeIndices->mCount==slots,"Exhausted manager cleanup leaked slots");
 {
  ParticleRenderer renderer(pool,simulation,Drain);
  Frame(frames);glFrameAlloc(65536-128,GLM_VertexData);
  Reject([&]{renderer.Submit(*view);});Reject([&]{renderer.Release();});
  frames.Cancel();renderer.FinishFrame();
  Frame(frames);Check(renderer.Submit(*view)==2,"Renderer did not recover after original writer OOM");Send(renderer);
  renderer.Release();
 }
 // Original writer streams qualify signed16 truncation and both source orders.
 {
  Frame(frames);fxSetParticleRasterState(true,false,false,false,0);
  GLTexturedColourMeshWriter writer;Reject([&]{writer.Begin(0,GLP_QuadList,nullptr);});
  Reject([&]{writer.Begin(65536,GLP_QuadList,nullptr);});
  Reject([&]{writer.Begin(4,GLP_QuadList,&pool);});
  writer.Begin(1,GLP_TriList,nullptr);Reject([&]{writer.End();});
  Reject([&]{writer.Texcoord(nlVector2{32,0});});Reject([&]{writer.Vertex(INFINITY,0,0);});
  writer.Texcoord(nlVector2{1.f/3,-1.f/3});writer.Vertex(1,2,3);writer.Colour(nlColour{{12,34,56,78}});writer.End();
  const auto* p=writer.GetModel()->packets;const auto* uv=static_cast<const short*>(p->streams[1].address);
  Check(uv[0]==341&&uv[1]==-341,"Signed16 UV quantization differs");
  Reject([&]{writer.Colour(nlColour{});});Reject([&]{writer.End();});
  frames.Cancel();Reject([&]{writer.GetModel();});
  Frame(frames);writer.Begin(6,GLP_TriList,nullptr);ParticleReturn quad{};
  for(unsigned i=0;i<4;++i){quad.position[i]={float(i),0,0};quad.texcoord[i]={0,0};}quad.c={{1,2,3,4}};
  fxWriteParticleQuad(writer,quad,false);writer.End();
  const auto* positions=static_cast<const float*>(writer.GetModel()->packets->streams[0].address);
  const unsigned order[]={0,1,2,0,2,3};for(unsigned i=0;i<6;++i)Near(positions[i*3],float(order[i]));
  frames.Cancel();
 }
 simulation.Release();frames.Release();views.Release();materials.Release();glShutdownMemory();
}
}
int main(int argc,char**argv)
{
 try{
  Check(argc==2,"Supply fixture directory");std::vector<std::uint64_t> a(1024*1024),b(1024*1024);ResetStartupMemory();
  StandardAllocator.Initialize(a.data(),a.size()*8);VirtualAllocator.Initialize(b.data(),b.size()*8);gMemoryInitialized=1;
  const auto free1=StandardAllocator.TotalFreeMemory(),free2=VirtualAllocator.TotalFreeMemory();
  for(unsigned n=0;n<3;++n){Session(argv[1]);Check(StandardAllocator.TotalFreeMemory()==free1&&VirtualAllocator.TotalFreeMemory()==free2,"Repeated render session leaked arenas");}
  ResetStartupMemory();std::cout<<checks<<" particle render checks passed\n";
 }catch(const std::exception&e){std::cerr<<"FAILED: "<<e.what()<<" (check "<<checks<<")\n";return 1;}
}
