#include "skin_render_fixture.h"
#include "runtime/skin_material.h"
#include "runtime/frames.h"
#include "runtime/views.h"
#include "runtime/materials.h"
#include "runtime/material_environment.h"
#include "runtime/lighting_state.h"
#include "runtime/graphics_memory.h"
#include "runtime/graphics_state.h"
#include "runtime/startup.h"
#include "Game/GameObjectLighting.h"
#include "Game/Render/LightingLookup.h"
#include "Game/GL/GLInventory.h"
#include "Game/GL/GLTextureAnim.h"
#include "NL/gl/GLFontAtlas.h"
#include "NL/gl/font_data.h"
#include "NL/gl/gl.h"
#include "NL/gl/glMemory.h"
#include "NL/gl/glMemoryInit.h"
#include "NL/gl/glTexture.h"
#include "NL/gl/glTextureManager.h"
#include "NL/glx/GXCharacterSkinCustomMaterialProgram.h"
#include "NL/glx/glxTexture.h"
#include <cmath>
#include <climits>
#include <iostream>
#include <source_location>
#include <thread>
using namespace mscharged;
namespace f=skin_render_fixture;
namespace
{
unsigned checks=0,drains=0;bool fail_drain=false;
void Check(bool yes,const char* message){++checks;if(!yes)throw std::runtime_error(message);}
template<class F>void Reject(F fn,std::source_location at=std::source_location::current())
{++checks;try{fn();}catch(const std::exception&){return;}throw std::runtime_error("Invalid skin render accepted at line "+std::to_string(at.line()));}
void Drain(){++drains;if(fail_drain)throw std::runtime_error("Injected skin drain failure");}
const glModelPacket* observed=nullptr;
void Inspect(GLView*,unsigned long,const glModelPacket* packet){if(packet)observed=packet;}
struct Backend:FrameBackend
{GLView* view=nullptr;bool Acquire()override{return true;}void Render()override{if(view)view->Iterate(Inspect);}void Finish(bool)override{}void Drain()override{}void WaitIdle()override{}void Cancel()noexcept override{}};
void Begin(OriginalFrames& f){Check(f.Acquire(),"Frame acquisition failed");glBeginFrame();}
void Atlas()
{
 const auto* texture=glx_GetTex(0xbea58af6);Check(texture&&texture->m_Width==128&&texture->m_Height==256&&texture->m_Format==GXTex_RGB5A3,"Original missing texture is absent");
 const auto* data=static_cast<const std::uint8_t*>(texture->m_SwizzledData);unsigned set=0;
 for(unsigned y=0;y<256;++y)for(unsigned x=0;x<128;++x)
 {
  // Independent linear glyph-grid oracle reads original ushort row masks,
  // then addresses the decoded GX tile. No production placement/swizzle call.
  const unsigned glyph=(y/16)*12+x/10,dx=x%10,dy=y%16;
  const bool white=glyph<94&&x/10<12&&dx<9&&dy<15&&((sMediumFontData[glyph*15+dy]>>(15-dx))&1);
  const unsigned at=((y/4)*32+x/4)*32+((y%4)*4+x%4)*2;
  Check((unsigned(data[at])<<8|data[at+1])==(white?65535:0),"Original medium font atlas bits differ");set+=white;
 }
 Check(set>1000,"Original fallback font is an empty image");
 std::vector<unsigned short> pixels(128*256);Reject([&]{glBuildFixedFontImage(3,pixels.data());});Reject([&]{glBuildFixedFontImage(1,nullptr);});
 Reject([&]{glFontBlitCharacter(INT_MAX,0,'A',pixels.data(),128,1);});Reject([&]{glFontBlitCharacter(0,INT_MAX,'A',pixels.data(),128,1);});Reject([&]{glFontBlitCharacter(0,0,31,pixels.data(),128,1);});
}
void Session()
{
 const GLMemoryRequirement req[]={{GLM_Header,65536},{GLM_VertexData,65536},{GLM_TextureData,65536}};const GLMemoryConfig cfg{65536,65536,req,3,4};
 glInitResourcePools();glInitMemory(&cfg);InitializeOriginalGraphicsState();SetGraphicsCacheInvalidator(Drain);
 MaterialPrograms programs;OriginalViews views(640,480,Drain);ViewMatrices matrices;auto* view=new(8,false)GLView(&matrices,{},GLViewSort_None);gRootView.AddChild(view);
 Backend backend;backend.view=view;OriginalFrames frames(backend);auto& pool=*glGetCurrentResourcePool();const auto free=pool.GetFreeMemory();const auto slots=glGetTextureManager()->mFreeIndices->mCount;
 const auto bytes=f::Rlt();std::array<resources::Bytes,1> files{bytes};auto asset=f::Asset(1);auto pose=f::Frame(asset,.75f);
 Reject([&]{SkinRenderer bad(pool,asset,files,SkinRenderProfile::Authored,nullptr);});
 {auto missing=f::Rlt(true);std::array<resources::Bytes,1> m{missing};Reject([&]{SkinRenderer bad(pool,asset,m,SkinRenderProfile::Authored,Drain);});}
 {
  SkinRenderer renderer(pool,asset,files,SkinRenderProfile::Authored,Drain);Check(renderer.Active()&&renderer.TextureIndex(0x12345678)!=0xffff,"Genuine texture registration failed");
  Reject([&]{SkinRenderer duplicate(pool,asset,files,SkinRenderProfile::Authored,Drain);});
  Reject([&]{renderer.Submit(*view,pose);});renderer.FinishFrame();
  std::thread wrong([&]{Reject([&]{renderer.Active();});Reject([&]{renderer.Release();});});wrong.join();
  Begin(frames);Reject([&]{renderer.Submit(*view,{});});auto malformed=std::make_shared<SkinPoseFrame>(*pose);malformed->packets[0].clear();Reject([&]{renderer.Submit(*view,malformed);});
  Check(renderer.Submit(*view,pose)==1,"Skin submission lost its source packet");view->Iterate(Inspect);Check(observed&&observed->numStreams==6,"Native packet stream count differs");ValidateNativeSkinPacket(*observed);
  const auto& p=*observed;const auto& material=*static_cast<const GXCharacterSkinCustomParameters*>(p.materialParameters);
  Check(material.skinMatricesSize==96&&material.diffuseTexture.texture==0x12345678&&material.detailTexture.texture==0x12345679,"Native material offsets or binding identities differ");
  Check(material.skinMatrices[1][0][3]==.75f,"GX matrix slot translation differs");
  const auto* uv=static_cast<const short*>(p.streams[3].address);Check(uv[0]==896&&uv[1]==128,"Signed16 fixed UVs changed");
  auto saved=*observed;auto& edit=*const_cast<glModelPacket*>(observed);edit.numStreams=5;Reject([&]{ValidateNativeSkinPacket(edit);});edit=saved;
  auto& params=*static_cast<GXCharacterSkinCustomParameters*>(edit.materialParameters);params.skinMatricesSize=11*48;Reject([&]{ValidateNativeSkinPacket(edit);});params.skinMatricesSize=96;
  Reject([&]{renderer.Submit(*view,pose);});Reject([&]{renderer.Release();});Reject([&]{renderer.FinishFrame();});
  glEndFrame();glSendFrame();fail_drain=true;Reject([&]{renderer.FinishFrame();});fail_drain=false;Reject([&]{renderer.Release();});renderer.FinishFrame();
  Begin(frames);renderer.Submit(*view,pose);frames.Cancel();renderer.FinishFrame();
  const auto mark=pool.MarkResource();Reject([&]{renderer.Release();});pool.ReleaseResource(mark);
  fail_drain=true;Reject([&]{renderer.Release();});fail_drain=false;Check(renderer.Active(),"Failed drain retired live registration");renderer.Release();renderer.Release();
 }
 Check(pool.GetFreeMemory()==free&&glGetTextureManager()->mFreeIndices->mCount==slots,"Skin release leaked pool or slots");
 // A same-hash animation alias is a collision even without a static texture.
 {
  const auto mark=pool.MarkResource();PlatTexture texture;glRegisterTexture(7,&texture,&pool);
  GLTextureAnim alias{};GLAnimTex frame{texture.m_TextureIndex,1};alias.m_textureIndex=0xffff;
  alias.m_nNumTextures=alias.m_NativeFrameCount=1;alias.m_pAnimTex=&frame;alias.m_ePlayMode=GLAnimMode_Loop;alias.m_nPlayDir=1;
  pool.m_inventory->AddTextureAnim(0x12345678,&alias);glGetTextureManager()->RegisterTextureAnim(&alias);
  Reject([&]{SkinRenderer bad(pool,asset,files,SkinRenderProfile::Authored,Drain);});pool.ReleaseResource(mark);
 }
 {
  const auto mark=pool.MarkResource();PlatTexture held[3];for(unsigned i=0;i<3;++i)glRegisterTexture(100+i,held+i,&pool);const auto before=pool.GetFreeMemory();
  Reject([&]{SkinRenderer bad(pool,asset,files,SkinRenderProfile::Authored,Drain);});Check(pool.GetFreeMemory()==before&&glGetTextureIndex(0x12345678)==0xffff,"Partial texture registration failed rollback");pool.ReleaseResource(mark);
 }
 {
  SkinRenderer renderer(pool,asset,files,SkinRenderProfile::Authored,Drain);Begin(frames);glFrameAlloc(65536-64,GLM_Header);Reject([&]{renderer.Submit(*view,pose);});frames.Cancel();renderer.FinishFrame();
  Begin(frames);renderer.Submit(*view,pose);glEndFrame();glSendFrame();renderer.FinishFrame();renderer.Release();
 }
 {
  auto shock=f::Asset(0,1,1,true,true);auto rlt=f::Rlt(true);std::array<resources::Bytes,1> input{rlt};SkinRenderer renderer(pool,shock,input,SkinRenderProfile::BowserShock,Drain);Atlas();
  Begin(frames);renderer.Submit(*view,f::Frame(shock));view->Iterate(Inspect);const auto& material=*static_cast<const GXCharacterSkinCustomParameters*>(observed->materialParameters);
  Check(material.diffuseTexture.texture==0xe4457de5&&material.detailTexture.texture==0xbea58af6,"Original shock/fallback binding branch differs");frames.Cancel();renderer.FinishFrame();renderer.Release();
 }
 nlMatrix4 identity;identity.SetIdentity();GameLighting lighting;
 // Independent diagonal inverse-transpose oracle and actual unsafe domains.
 float normal_input[3][4]{{2,0,0,100},{0,4,0,-200},{0,0,.5f,300}},normal_output[3][4]{};
 SkinNormalMatrix(normal_input,normal_output);
 for(unsigned r=0;r<3;++r)for(unsigned c=0;c<4;++c)Check(normal_output[r][c]==(r==c?(r==0?.5f:r==1?.25f:2.f):0),"Original normal inverse-transpose differs");
 normal_input[0][0]=0;Reject([&]{SkinNormalMatrix(normal_input,normal_output);});
 for(unsigned i=0;i<3;++i)normal_input[i][i]=1e15f;Reject([&]{SkinNormalMatrix(normal_input,normal_output);});
 normal_input[0][0]=NAN;Reject([&]{SkinNormalMatrix(normal_input,normal_output);});
 {MaterialPreviewScope scope(identity,0,lighting);Reject([&]{GetGameObjectLightCount(true,false);});SetGameObjectShadowViewMatrix(nullptr);Check(!ActiveGameLighting().shadow_inverse_view,"Inactive source shadow setter changed state");}
 lighting.character.emplace();lighting.character->light_count=7;Reject([&]{ValidateGameLighting(lighting);});lighting.character->light_count=1;lighting.character->lights[0].useColour=2;Reject([&]{ValidateGameLighting(lighting);});lighting.character->lights[0].useColour=0;
 {MaterialPreviewScope scope(identity,0,lighting);Check(GetGameObjectLightCount(true,false)==1&&GetGameObjectLight(0,true)==&ActiveGameLighting().inputs.character->lights[0],"Explicit character input was not selected");}
 {
  const auto mark=pool.MarkResource();std::array<u8,32> pixels{};alignas(u16)std::array<u8,2> palette{255,255};PlatTexture texture;
  texture.m_Width=8;texture.m_Height=4;texture.m_Levels=texture.m_MaxLevel=1;texture.m_Format=GXTex_CI8;texture.m_nPaletteEntries=1;
  texture.m_SwizzledData=pixels.data();texture.m_PaletteData=reinterpret_cast<u16*>(palette.data());texture.m_NativeDataBytes=32;texture.m_NativePaletteBytes=2;glRegisterTexture(7,&texture,&pool);
  LightingLookup lookup;lookup.LoadTexture(7);lighting.shadow.lookup=&lookup;lighting.shadow.texture=7;
  {MaterialPreviewScope scope(identity,0,lighting);nlMatrix4 view=identity;view.m11=2;view.m41=4;SetGameObjectShadowViewMatrix(&view);
   const auto& inverse=*ActiveGameLighting().shadow_inverse_view;Check(inverse.m11==.5f&&inverse.m41==-2,"Original shadow inverse-view setter differs");
   Reject([&]{SetGameObjectShadowViewMatrix(nullptr);});view.m11=0;Reject([&]{SetGameObjectShadowViewMatrix(&view);});view.m11=NAN;Reject([&]{SetGameObjectShadowViewMatrix(&view);});
   Check(ActiveGameLighting().shadow_inverse_view->m41==-2,"Failed shadow inverse changed publication");Reject([&]{ApplyGameObjectShadowLighting(1,0);});}
  lighting.shadow={};pool.ReleaseResource(mark);
 }
 frames.Release();views.Release();programs.Release();glShutdownMemory();
}
}
int main()
{
 try{std::vector<std::uint64_t>a(1024*1024),b(1024*1024);ResetStartupMemory();StandardAllocator.Initialize(a.data(),a.size()*8);VirtualAllocator.Initialize(b.data(),b.size()*8);gMemoryInitialized=1;
 const auto m1=StandardAllocator.TotalFreeMemory(),m2=VirtualAllocator.TotalFreeMemory();for(unsigned i=0;i<3;++i){Session();Check(StandardAllocator.TotalFreeMemory()==m1&&VirtualAllocator.TotalFreeMemory()==m2,"Repeated skin renderer leaked arenas");}ResetStartupMemory();std::cout<<checks<<" skin renderer checks passed\n";
 }catch(const std::exception& e){fail_drain=false;std::cerr<<"FAILED: "<<e.what()<<" (check "<<checks<<")\n";return 1;}
}
