#include "runtime/weighted_skin_assets.h"
#include "weighted_skin_fixture.h"
#include "Game/SHierarchy.h"
#include "Game/GL/SkinPoseSteps.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <source_location>
namespace f=weighted_skin_fixture;
using namespace mscharged;
namespace
{
unsigned checks=0;
void Check(bool value,const char* message){++checks;if(!value)throw std::runtime_error(message);}
template<class F>void Reject(F fn,std::source_location where=std::source_location::current())
{++checks;try{fn();}catch(const std::exception&){return;}throw std::runtime_error("Expected rejection at line "+std::to_string(where.line()));}
void Bits(float a,float b){Check(std::bit_cast<std::uint32_t>(a)==std::bit_cast<std::uint32_t>(b),"Authored weight bits changed");}
f::Blob Read(const char* path){std::ifstream file(path,std::ios::binary);Check(bool(file),"Cannot open weighted skin fixture");return {std::istreambuf_iterator<char>(file),{}};}
std::uint64_t Fingerprint(const resources::WeightedSkinModel& model)
{
 std::uint64_t hash=14695981039346656037ull;auto word=[&](std::uint32_t x){for(int shift:{24,16,8,0}){hash^=(x>>shift)&255;hash*=1099511628211ull;}};
 auto real=[&](float x){word(std::bit_cast<std::uint32_t>(x));};word(model.hash);
 for(const auto& b:model.binds){word(b.hash);for(float x:b.matrix)real(x);}
 for(const auto& p:model.packets)
 {
  word(p.program);word(p.primitive);word(p.raster);for(auto x:p.stream_slots)word(x);for(float x:p.matrix)real(x);
  for(auto x:p.material.textures){word(x.hash);word(x.flags);}for(float x:{p.material.blend,p.material.alpha,p.material.specular_level,p.material.specular_exponent})real(x);for(float x:p.material.specular_colour)real(x);word(p.material.shadow_level);word(p.material.lighting_enabled);
  for(auto x:p.bone_hashes)word(x);for(auto x:p.indices)word(x);
  for(const auto& v:p.vertices){for(float x:v.position)real(x);for(float x:v.normal)real(x);for(auto uv:v.uv)for(auto x:uv)word(std::uint16_t(x));for(auto x:v.bones)word(x);for(float x:v.weights)real(x);}
 }
 return hash;
}
void Verify(const resources::WeightedSkinModel& raw,const WeightedSkinAsset& asset)
{
 const auto& prepared=asset.Data();Check(raw.hash==prepared.hash&&raw.packets.size()==prepared.packets.size(),"Retained model identity differs");
 bool rigid=true;
 for(unsigned p=0;p<raw.packets.size();++p)
 {
  const auto& source=raw.packets[p];const auto& actual=prepared.packets[p];const auto& gathered=asset.Weights()[p];
  std::vector<std::vector<WeightedSkinPair>> oracle(source.bone_hashes.size());
  for(unsigned v=0;v<source.vertices.size();++v)
  {
   auto w=source.vertices[v].weights;auto b=source.vertices[v].bones;
   // Independent max_element has stable first-tie semantics, unlike the
   // source's four explicit comparisons. No arithmetic touches the weights.
   const auto largest=std::max_element(w.begin(),w.end())-w.begin();std::swap(w[0],w[largest]);std::swap(b[0],b[largest]);
   for(unsigned lane=0;lane<4;++lane){Bits(actual.vertices[v].weights[lane],w[lane]);Check(actual.vertices[v].bones[lane]==b[lane],"Original weight swap changed bone identity");}
   if(w[1]!=0)rigid=false;
   for(unsigned lane=0;lane<4&&w[lane]!=0;++lane)oracle[b[lane]].push_back({v,w[lane]});
  }
  for(unsigned bone=0;bone<oracle.size();++bone)
  {
   Check(gathered[bone].size()==oracle[bone].size(),"Original per-bone pair count changed");
   for(unsigned i=0;i<oracle[bone].size();++i){Check(gathered[bone][i].vertexIndex==oracle[bone][i].vertexIndex,"Original pair traversal order changed");Bits(gathered[bone][i].vertexWeight,oracle[bone][i].vertexWeight);}
   Check(asset.Hierarchy()->Data().GetNodeIndexByID(source.bone_hashes[bone])==int(asset.NodeMaps()[p][bone]),"Original bone/hash association changed");
  }
 }
 Check(rigid==asset.OriginalRigidFlag(),"Original model rigid flag changed");
}
void Synthetic()
{
 auto b=f::Model();const auto original=b;auto h=HierarchyAsset::Decode(f::Rig(4));auto raw=resources::ReadWeightedSkinModel(b);auto asset=WeightedSkinAsset::Decode(b,h);
 Verify(raw,*asset);Check(!asset->OriginalRigidFlag(),"Fractional model was rigidified");Check(b==original,"Decoder mutated authored bytes");
 for(unsigned i=0;i<4;++i){const auto& m=asset->InverseBinds()[i];Bits(m.m11,.5f);Bits(m.m22,.5f);Bits(m.m33,.5f);Check(m.m41==-float(i)&&m.m42==-2.f&&m.m43==3.f,"Independent inverse-bind translation differs");}
 Check(asset->Data().packets[0].vertices[3].weights==std::array{.25f,.25f,.25f,.25f},"Four influences were truncated");
 for(std::size_t n=0;n<b.size();++n)Reject([&]{resources::ReadWeightedSkinModel(resources::Bytes(b.data(),n));});
 auto fail=[&](auto fn,bool owner=false){auto bad=b;fn(bad);Reject([&]{if(owner)WeightedSkinAsset::Decode(bad,h);else resources::ReadWeightedSkinModel(bad);});};
 auto setFloat=[&](f::Blob& bytes,std::size_t at,float x){f::Set(bytes,at,std::bit_cast<unsigned>(x));};
 const auto pa=f::Find(b,0x1b004),st=f::Find(b,0x1b005),v=f::Find(b,0x1b006),ma=f::Find(b,0x1b016),bo=f::Find(b,0x1b00b),bi=f::Find(b,0x1b00a),mo=f::Find(b,0x1b00c);
 const auto bones=v+48+48+16*3,weights=bones+16;
 fail([&](auto& x){f::Set(x,pa+16,0x041c3281);});fail([&](auto& x){x[pa+11]=6;});fail([&](auto& x){f::Set(x,pa+12,8);});fail([&](auto& x){x[st+6]=5;});fail([&](auto& x){x[st+5]=4;});fail([&](auto& x){f::Set(x,st,UINT32_MAX);});
 fail([&](auto& x){f::Set(x,mo,1);});fail([&](auto& x){f::Set(x,mo+8,2);});fail([&](auto& x){f::Set(x,ma+68,2);});fail([&](auto& x){f::Set(x,bo,0xabcdef01);},true);fail([&](auto& x){f::Set(x,bi,0xabcdef01);},true);
 for(float bad:{-1.f,1.001f,NAN,INFINITY})fail([&](auto& x){setFloat(x,weights,bad);});
 fail([&](auto& x){x[bones]=4;});fail([&](auto& x){for(unsigned i=0;i<4;++i)setFloat(x,weights+i*4,0);});
 fail([&](auto& x){setFloat(x,weights,.6f);setFloat(x,weights+4,0);setFloat(x,weights+8,.4f);},true);
 // All four identical bone lanes on all vertices exceed the actual original
 // per-bone scratch capacity; do not silently grow or discard duplicate terms.
 fail([&](auto& x){for(unsigned j=0;j<4;++j)for(unsigned k=0;k<4;++k){x[bones+j*4+k]=0;setFloat(x,weights+j*16+k*4,.25f);}},true);
 for(float bad:{0.f,1e-12f,NAN,INFINITY})fail([&](auto& x){setFloat(x,bi+4,bad);},true);
 Reject([&]{WeightedSkinAsset::Decode(b,{});});Reject([&]{resources::ReadWeightedSkinModel(b,0xbad);});
 // Raw scratch pointers/indices never become host references. Authored stream
 // slot bytes are retained unchanged although this material binds by ordinal.
 auto cache=b;for(unsigned i=24;i<32;++i)cache[ma+i]=0xff;for(unsigned i=0;i<7;++i)cache[st+i*8+4]=255;
 auto changed=WeightedSkinAsset::Decode(cache,h);for(auto slot:changed->Data().packets[0].stream_slots)Check(slot==255,"Authored stream slot byte lost");
 auto retained=asset->Hierarchy();h.reset();b.clear();Check(retained==asset->Hierarchy()&&retained->Data().GetNumNodes()==4,"Skin failed to retain exact hierarchy identity");
 std::cout<<"synthetic_fingerprint="<<Fingerprint(raw)<<'\n';
}
}
int main(int argc,char** argv)
{
 try
 {
  std::cout<<std::unitbuf;
  if(argc==4&&std::string(argv[1])=="--fixture")
  {
   const auto b=f::Model(),h=f::Rig(4);
   std::ofstream model(argv[2],std::ios::binary),hierarchy(argv[3],std::ios::binary);
   model.write(reinterpret_cast<const char*>(b.data()),b.size());
   hierarchy.write(reinterpret_cast<const char*>(h.data()),h.size());
   return !model||!hierarchy;
  }
  if(argc==1)Synthetic();
  else
  {
   Check(argc==3,"Supply RLG and SHier paths");const auto b=Read(argv[1]);auto h=HierarchyAsset::Decode(Read(argv[2]));const auto raw=resources::ReadWeightedSkinModel(b);auto asset=WeightedSkinAsset::Decode(b,h);Verify(raw,*asset);
   std::cout<<"raw_fingerprint="<<Fingerprint(raw)<<" prepared_fingerprint="<<Fingerprint(asset->Data())<<" packets="<<raw.packets.size()<<" nodes="<<h->Data().GetNumNodes()<<" rigid="<<asset->OriginalRigidFlag()<<'\n';
  }
  std::cout<<checks<<" weighted skin checks passed\n";
 }
 catch(const std::exception& e){std::cerr<<"FAILED: "<<e.what()<<" check "<<checks<<'\n';return 1;}
}
