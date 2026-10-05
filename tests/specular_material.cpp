#include "specular_gx_record.h"
#include "resources/specular_material.h"
#include "runtime/specular_material.h"
#include "runtime/materials.h"
#include "runtime/material_environment.h"
#include "runtime/graphics_memory.h"
#include "runtime/graphics_state.h"
#include "runtime/startup.h"
#include "NL/gl/glMatrix.h"
#include "NL/gl/glMaterialProgram.h"
#include "NL/gl/glMemory.h"
#include "NL/gl/glMemoryInit.h"
#include "NL/gl/glState.h"
#include "NL/gl/glTexture.h"
#include "NL/gl/glTextureManager.h"
#include "NL/glx/GXSpecularMaterialProgram.h"
#include "NL/glx/glxSkinMatrix.h"
#include "NL/glx/glxTexture.h"
#include "Game/Render/LightingLookup.h"
#include <fstream>
#include <iostream>
#include <iterator>
#include <source_location>
#include <limits>
using namespace mscharged;
namespace rec=specular_gx_record;
extern "C" void __real__Z31RestoreGameObjectShadowLightingv();
extern "C" void __wrap__Z31RestoreGameObjectShadowLightingv()
{
 rec::Record("RestoreGameObjectShadowLighting");
 __real__Z31RestoreGameObjectShadowLightingv();
}
namespace
{
unsigned checks=0;
void Check(bool condition,const char* message){++checks;if(!condition)throw std::runtime_error(message);}
template<class F>void Reject(F fn,std::source_location line=std::source_location::current())
{++checks;try{fn();}catch(const std::exception&){return;}throw std::runtime_error("Invalid Specular input accepted at line "+std::to_string(line.line()));}
void Drain(){}
void U32(std::array<u8,72>& b,unsigned o,u32 value){for(unsigned i=0;i<4;++i)b[o+i]=u8(value>>(24-i*8));}
void Float(std::array<u8,72>& b,unsigned o,float value){U32(b,o,std::bit_cast<u32>(value));}
std::array<u8,72> Record(unsigned flags=0,float alpha=1,float exponent=64)
{
 std::array<u8,72> b{};
 for(unsigned i=0;i<3;++i){U32(b,i*8,0xabc100+i);b[i*8+4]=0x42;b[i*8+5]=0x99;b[i*8+6]=(flags>>(i*2))&3;}
 U32(b,24,0xf1234567);U32(b,28,0xffffffff);Float(b,32,.25f);Float(b,36,alpha);Float(b,40,.75f);Float(b,44,exponent);
 for(unsigned i=0;i<4;++i)Float(b,48+i*4,float(i+1)/4);U32(b,64,UINT32_MAX);U32(b,68,1);return b;
}
void Decoder()
{
 for(unsigned flags=0;flags<64;++flags)for(float alpha:{0.f,.5f,1.f})for(float exponent:{0.f,1.f,64.f,10000.f})
 {
  const auto raw=Record(flags,alpha,exponent);const auto got=resources::ReadSpecularSkinMaterial(raw);
  for(unsigned i=0;i<3;++i)Check(got.textures[i].hash==0xabc100+i&&got.textures[i].flags==((flags>>(i*2))&3),"Authored Specular binding decoded differently");
  Check(got.blend==.25f&&got.alpha==alpha&&got.specular_level==.75f&&got.specular_exponent==exponent,"Authored Specular scalars differ");
  for(unsigned i=0;i<4;++i)Check(got.specular_colour[i]==float(i+1)/4,"Authored Specular colour differs");
  Check(got.shadow_level==UINT32_MAX&&got.lighting_enabled==1,"Authored Specular switches differ");
 }
 auto b=Record();for(unsigned n=0;n<72;++n)Reject([&]{resources::ReadSpecularSkinMaterial(resources::Bytes(b).first(n));});
 std::array<u8,73> extra{};std::copy(b.begin(),b.end(),extra.begin());Reject([&]{resources::ReadSpecularSkinMaterial(extra);});
 for(unsigned t=0;t<3;++t){b=Record();b[t*8+6]=4;Reject([&]{resources::ReadSpecularSkinMaterial(b);});b=Record();b[t*8+7]=1;Reject([&]{resources::ReadSpecularSkinMaterial(b);});}
 for(unsigned o:{32u,36u,40u,44u,48u,52u,56u,60u})for(float v:{NAN,INFINITY,-INFINITY,-.1f}){b=Record();Float(b,o,v);Reject([&]{resources::ReadSpecularSkinMaterial(b);});}
 for(unsigned o:{32u,36u,40u,48u,52u,56u,60u}){b=Record();Float(b,o,1.01f);Reject([&]{resources::ReadSpecularSkinMaterial(b);});}
 b=Record();Float(b,44,10001);Reject([&]{resources::ReadSpecularSkinMaterial(b);});b=Record();U32(b,68,2);Reject([&]{resources::ReadSpecularSkinMaterial(b);});
}
bool Event(const char* name,std::initializer_list<std::uint64_t> args)
{for(const auto& e:rec::events)if(e.name==name&&e.args==std::vector<std::uint64_t>(args))return true;return false;}
unsigned Count(const char* name){unsigned n=0;for(const auto& e:rec::events)n+=e.name==name;return n;}
struct Packet
{
 std::array<std::array<float,3>,3> position{{{-1,-1,0},{1,-1,0},{0,1,0}}},normal{{{0,0,1},{0,0,1},{0,0,1}}};
 std::array<std::array<short,2>,3> uv0{{{0,0},{1024,0},{512,1024}}},uv1=uv0,uv2=uv0;
 std::array<std::array<u8,4>,3> bone{{{0,0,0,0},{1,0,0,0},{0,0,0,0}}};
 std::array<std::array<float,4>,3> weight{{{1,0,0,0},{1,0,0,0},{1,0,0,0}}};
 std::array<u16,3> indices{2,0,1};glModelStream streams[7]{};GXSpecularParameters params{};glModelPacket packet{};
 float matrices[2][3][4]{{{1,0,0,0},{0,1,0,0},{0,0,1,0}},{{1,0,0,.25f},{0,1,0,0},{0,0,1,0}}};
 Packet()
 {
  void* pointers[]{position.data(),normal.data(),uv0.data(),uv1.data(),uv2.data(),bone.data(),weight.data()};
  const unsigned ids[]{1,2,4,4,4,7,5},strides[]{12,12,4,4,4,4,16};for(unsigned s=0;s<7;++s)streams[s]={pointers[s],u8(42+s),u8(strides[s]),u8(ids[s]),0};
  packet.indexBuffer=indices.data();packet.numVertices=3;packet.numUniqueVertices=3;packet.primType=0;packet.numStreams=7;packet.streams=streams;
  packet.matrix=glGetIdentityMatrix();packet.rasterState=glGetCurrentRasterState();
  InstallSpecularMaterial(packet,resources::ReadSpecularSkinMaterial(Record()),params);
 }
 void Pose(){params.skinMatrices=matrices;params.skinMatricesSize=sizeof(matrices);}
};
void Trace(const Packet& p,unsigned passes,bool lit)
{
 Check(Count("GXBegin")==passes&&Count("GXEnd")==passes,"Original Specular pass count differs");
 Check(Count("RestoreGameObjectShadowLighting")==1,"Original Specular Draw shadow restore count differs");
 unsigned final_end=0,restore=0;for(unsigned i=0;i<rec::events.size();++i){if(rec::events[i].name=="GXEnd")final_end=i;if(rec::events[i].name=="RestoreGameObjectShadowLighting")restore=i;}Check(restore>final_end,"Specular shadow restored before original Draw finished");
 Check(Count("GXSetArray")==5,"Specular did not bind five retained native GX arrays");
 const GXAttr attributes[]{GX_VA_POS,GX_VA_NRM,GX_VA_TEX0,GX_VA_TEX1,GX_VA_TEX2};
 for(unsigned s=0;s<5;++s)Check(Event("GXSetArray",{attributes[s],rec::Value(SpecularVertexArray(p.packet,s)),std::uint64_t(3*p.streams[s].stride),p.streams[s].stride,1}),"Specular GX array extent/stride/endian differs");
 const unsigned orders[5][3]{{1,1,255},{0,0,255},{255,255,lit?4u:255u},{2,2,255},{255,255,5}};
 for(unsigned s=0;s<5;++s)Check(Event("GXSetTevOrder",{s,orders[s][0],orders[s][1],orders[s][2]}),"Original Specular TEV order differs");
 Check(Event("GXSetNumTevStages",{5})&&Event("GXSetNumTexGens",{3})&&Event("GXSetNumChans",{2}),"Specular channel/stage/generator counts differ");
 const unsigned inputs[5][4]{{15,12,8,14},{15,0,8,15},{15,0,lit?10u:12u,15},{15,14,8,15},{15,0,10,2}};
 for(unsigned s=0;s<5;++s)Check(Event("GXSetTevColorIn",{s,inputs[s][0],inputs[s][1],inputs[s][2],inputs[s][3]}),"Original Specular TEV colour equation differs");
 Check(Count("GXLoadTexObj")==4,"Diffuse/detail/gloss/light-ramp binding count differs");
 std::vector<rec::Event> fifo;for(const auto& e:rec::events)if(e.name=="GXParam1u8"||e.name=="GXPosition1x16"||e.name=="GXNormal1x16"||e.name=="GXTexCoord1x16")fifo.push_back(e);
 Check(fifo.size()==passes*3*6,"Specular FIFO stitch/five-index count differs");unsigned at=0;
 for(unsigned pass=0;pass<passes;++pass)for(auto index:p.indices)
 {
  Check(fifo[at].name=="GXParam1u8"&&fifo[at++].args==std::vector<std::uint64_t>{glx_SkinMatrixSlots[p.bone[index][0]]},"Original stitch matrix slot differs");
  for(const char* name:{"GXPosition1x16","GXNormal1x16","GXTexCoord1x16","GXTexCoord1x16","GXTexCoord1x16"})Check(fifo[at].name==name&&fifo[at++].args==std::vector<std::uint64_t>{index},"Original five stream-index order differs");
 }
}
void Session()
{
 const GLMemoryRequirement req[]{{GLM_Header,65536},{GLM_VertexData,65536},{GLM_TextureData,65536}};const GLMemoryConfig cfg{65536,65536,req,3,16};
 glInitResourcePools();glInitMemory(&cfg);InitializeOriginalGraphicsState();SetGraphicsCacheInvalidator(Drain);auto& pool=*glGetCurrentResourcePool();
 MaterialPrograms programs;Reject([]{MaterialPrograms duplicate;});auto* program=static_cast<GLMaterialProgram*>(glGetMaterialProgram(0x22cadb20));
 Check(program==GXSpecularMaterialProgram::Instance&&program->parameterDataSize==sizeof(GXSpecularParameters)&&program->parameterCount==11,"Genuine Specular registration/native layout is absent");
 const unsigned offsets[]{offsetof(GXSpecularParameters,diffuseTexture),offsetof(GXSpecularParameters,detailTexture),offsetof(GXSpecularParameters,glossTexture),offsetof(GXSpecularParameters,skinMatrices),offsetof(GXSpecularParameters,blendAmount),offsetof(GXSpecularParameters,alphaValue),offsetof(GXSpecularParameters,specularLevel),offsetof(GXSpecularParameters,specularExponent),offsetof(GXSpecularParameters,specularColour),offsetof(GXSpecularParameters,shadowLevel),offsetof(GXSpecularParameters,lightingEnabled)};
 const u32 ids[]{0x69f44dc5,0xebaf55d2,0x93014de7,0xfb3b01ec,0x0658bb38,0xb46c81a2,0x29d1d576,0xccbcf02f,0x4fbdbbf2,0x46ccf41d,0x8e10b600};
 for(unsigned i=0;i<11;++i)Check(program->GetParameters()[i].hash==ids[i]&&program->GetParameters()[i].offset==offsets[i],"Original parameter identity/native offset differs");
 Check(sizeof(GXSpecularParameters)==80&&offsets[4]==36&&offsets[9]==68&&sizeof(GXSpecularParameters::shadowLevel)==4,"Host pointer and authored scalar widths are not independent");
 std::array<u8,64> pixels{};PlatTexture textures[4];const auto mark=pool.MarkResource();
 for(unsigned i=0;i<4;++i){textures[i].m_Width=textures[i].m_Height=4;textures[i].m_Format=GXTex_RGBA8;textures[i].m_Levels=textures[i].m_MaxLevel=1;textures[i].m_SwizzledData=pixels.data();textures[i].m_NativeDataBytes=pixels.size();glRegisterTexture(0xabc100+i,textures+i,&pool);}
 Packet p;Check(!p.params.skinMatrices&&!p.params.skinMatricesSize,"Registration manufactured a pose");for(auto* b:{&p.params.diffuseTexture,&p.params.detailTexture,&p.params.glossTexture})Check(b->textureIndex==0xffff,"Registration retained a serialized texture-slot cache");
 Reject([&]{ValidateNativeSpecularPacket(p.packet);});p.Pose();ValidateNativeSpecularPacket(p.packet);
 nlMatrix4 view;view.SetIdentity();GameLighting light;light.enabled=true;light.light_count=1;light.ramp_texture=0xabc103;
 light.lights[0].intensity=.5f;light.lights[0].useWorldPosition=true;light.lights[0].worldPosition={1,0,0};
 {MaterialPreviewScope context(view,0,light);program->Activate(nullptr);rec::Reset();program->Deactivate();Check(!Count("RestoreGameObjectShadowLighting"),"Native Deactivate added an original game-side shadow request");Check(Count("GXSetCurrentMtx")==1&&Event("GXSetCurrentMtx",{0}),"Original forced matrix reset was not forwarded once");}
 {
  MaterialPreviewScope context(view,0,light);rec::Reset();DrawMaterial(p.packet);Trace(p,1,true);
  bool atten=false;for(const auto& e:rec::events)if(e.name=="GXInitLightAttn"&&e.args.size()==7)atten|=e.args[1]==rec::Value(0.f)&&e.args[2]==rec::Value(0.f)&&e.args[3]==rec::Value(1.f)&&e.args[4]==rec::Value(32.f)&&e.args[5]==rec::Value(0.f)&&e.args[6]==rec::Value(-31.f);Check(atten,"Original exponent attenuation equation differs");
  p.params.alphaValue=.5f;rec::Reset();DrawMaterial(p.packet);Trace(p,2,true);Check(Event("GXSetBlendMode",{GX_BM_BLEND,GX_BL_SRCALPHA,GX_BL_INVSRCALPHA,GX_LO_CLEAR}),"Original alpha colour pass lost its blend mode");Check(Event("GXSetZMode",{1,GX_EQUAL,1})&&Event("GXSetColorUpdate",{0})&&Event("GXSetColorUpdate",{1}),"Original depth prepass/restoration differs");
  for(unsigned fail:{1u,2u}){rec::Reset();rec::fail_begin=fail;Reject([&]{DrawMaterial(p.packet);});rec::fail_begin=0;rec::Reset();DrawMaterial(p.packet);Trace(p,2,true);}
  p.params.alphaValue=0;rec::Reset();DrawMaterial(p.packet);Check(!Count("GXBegin")&&!Count("GXSetArray"),"Original alpha-zero branch issued geometry");p.params.alphaValue=1;
 }
 {
  light.enabled=false;MaterialPreviewScope context(view,0,light);rec::Reset();DrawMaterial(p.packet);Trace(p,1,true);
  p.params.lightingEnabled=0;rec::Reset();DrawMaterial(p.packet);Trace(p,1,false);p.params.lightingEnabled=1;
 }
 for(float exponent:{0.f,0.f,64.f}){light.enabled=true;MaterialPreviewScope context(view,0,light);p.params.specularExponent=exponent;rec::Reset();DrawMaterial(p.packet);Check(Count("GXInitSpecularDir")==unsigned(exponent!=0),"Original zero-exponent cache sentinel behavior differs");}
 {
  light.double_intensity=true;MaterialPreviewScope context(view,0,light);rec::Reset();DrawMaterial(p.packet);Check(Event("GXSetTevColorOp",{2,GX_TEV_ADD,GX_TB_ZERO,GX_CS_SCALE_2,1,GX_TEVREG0}),"Original double-light TEV scaling differs");light.double_intensity=false;
 }
 const auto saved=p.packet;for(unsigned s=0;s<7;++s){auto stream=p.streams[s];p.streams[s].stride=1;Reject([&]{ValidateNativeSpecularPacket(p.packet);});p.streams[s]=stream;p.streams[s].unknown07=1;Reject([&]{ValidateNativeSpecularPacket(p.packet);});p.streams[s]=stream;}
 p.packet.numStreams=6;Reject([&]{ValidateNativeSpecularPacket(p.packet);});p.packet=saved;p.indices[0]=3;Reject([&]{ValidateNativeSpecularPacket(p.packet);});p.indices[0]=2;p.params.skinMatricesSize=95;Reject([&]{ValidateNativeSpecularPacket(p.packet);});p.params.skinMatricesSize=96;
 p.weight[0]={.5f,.5f,0,0};p.bone[0][1]=1;Reject([&]{ValidateNativeSpecularPacket(p.packet);});
 {
  SpecularSoftwareSkin arrays{p.position,p.normal};SpecularSoftwareSkinScope posed(p.packet,arrays);ValidateNativeSpecularPacket(p.packet);Reject([&]{SpecularSoftwareSkinScope duplicate(p.packet,arrays);});auto other=p.packet;Reject([&]{SpecularUsesSoftwareSkin(other);});
  MaterialPreviewScope context(view,0,light);rec::Reset();DrawMaterial(p.packet);Trace(p,1,true);Check(Count("GXLoadPosMtxImm")==10&&Count("GXLoadNrmMtxImm")==9,"Genuine software arrays did not select original default matrices");
 }
 Reject([&]{ValidateNativeSpecularPacket(p.packet);});p.weight[0]={1,0,0,0};p.bone[0][1]=0;
 {
  const auto shadow_mark=pool.MarkResource();PlatTexture shadow;std::array<u8,32>data{};alignas(u16)std::array<u8,2>palette{255,255};
  shadow.m_Width=8;shadow.m_Height=4;shadow.m_Levels=shadow.m_MaxLevel=1;shadow.m_Format=GXTex_CI8;shadow.m_nPaletteEntries=1;shadow.m_SwizzledData=data.data();shadow.m_PaletteData=reinterpret_cast<u16*>(palette.data());shadow.m_NativeDataBytes=32;shadow.m_NativePaletteBytes=2;glRegisterTexture(0xabc104,&shadow,&pool);
  LightingLookup lookup;lookup.LoadTexture(0xabc104);auto with_shadow=light;with_shadow.shadow.lookup=&lookup;with_shadow.shadow.texture=0xabc104;
  {MaterialPreviewScope context(view,0,with_shadow);Reject([&]{DrawMaterial(p.packet);});}
  {MaterialPreviewScope context(view,0,light);rec::Reset();DrawMaterial(p.packet);Trace(p,1,true);}
  Drain();pool.ReleaseResource(shadow_mark);
 }
 {auto absent=light;absent.ramp_texture=UINT32_MAX;MaterialPreviewScope context(view,0,absent);Reject([&]{DrawMaterial(p.packet);});}
 Reject([&]{DrawMaterial(p.packet);});pool.ReleaseResource(mark);programs.Release();Check(!glGetMaterialProgram(0x22cadb20),"Material registry retained a freed Specular instance");Reject([&]{GXSpecularParameters storage;InstallSpecularMaterial(p.packet,resources::ReadSpecularSkinMaterial(Record()),storage);});glShutdownMemory();
}
void Owned(const char* name)
{
 std::ifstream input(name,std::ios::binary);std::vector<u8>b{std::istreambuf_iterator<char>(input),{}};Check(b.size()==144,"Owned Specular extraction must contain both exact72 records");
 for(unsigned p=0;p<2;++p){const auto m=resources::ReadSpecularSkinMaterial(resources::Bytes(b).subspan(p*72,72));Check(m.blend==1&&m.alpha==1&&m.specular_level==1&&m.specular_exponent==64&&m.shadow_level==UINT32_MAX&&m.lighting_enabled==1,"Owned ChainChomp authored scalar domain differs");Check(m.textures[0].hash==0x0b222782&&m.textures[1].hash==0x0b222782&&m.textures[2].hash==0x2f0cb969,"Owned ChainChomp binding identities differ");for(float value:m.specular_colour)Check(value==1,"Owned ChainChomp source specular colour differs");}
}
}
int main(int argc,char**argv)
{
 try{Decoder();std::vector<std::uint64_t>a(1024*1024),b(1024*1024);ResetStartupMemory();StandardAllocator.Initialize(a.data(),a.size()*8);VirtualAllocator.Initialize(b.data(),b.size()*8);gMemoryInitialized=1;const auto free1=StandardAllocator.TotalFreeMemory(),free2=VirtualAllocator.TotalFreeMemory();for(unsigned i=0;i<2;++i){Session();Check(StandardAllocator.TotalFreeMemory()==free1&&VirtualAllocator.TotalFreeMemory()==free2,"Specular session failed to recover both arenas");}ResetStartupMemory();if(argc==2)Owned(argv[1]);std::cout<<checks<<" original Specular material/source GX checks passed\n";}
 catch(const std::exception&e){std::cerr<<"FAILED: "<<e.what()<<" (check "<<checks<<")\n";return 1;}
}
