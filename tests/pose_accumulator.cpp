#include "runtime/pose_accumulator.h"
#include "runtime/hierarchy_assets.h"
#include "runtime/graphics_memory.h"
#include "runtime/startup.h"
#include "Game/SHierarchy.h"
#include "NL/nlMath.h"
#include "pose_accumulator_fixture.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <source_location>
#include <thread>

namespace allocation_probe
{
std::atomic<std::size_t> live=0;
thread_local std::size_t budget=SIZE_MAX;
}
void* operator new(std::size_t n)
{
 if(allocation_probe::budget!=SIZE_MAX){if(!allocation_probe::budget)throw std::bad_alloc();--allocation_probe::budget;}
 if(void* p=std::malloc(n?n:1)){++allocation_probe::live;return p;}throw std::bad_alloc();
}
void operator delete(void* p)noexcept{if(p){--allocation_probe::live;std::free(p);}}
void operator delete(void* p,std::size_t)noexcept{::operator delete(p);}
void* operator new[](std::size_t n){return ::operator new(n);}
void operator delete[](void* p)noexcept{::operator delete(p);}
void operator delete[](void* p,std::size_t)noexcept{::operator delete(p);}

using namespace mscharged;
namespace
{
unsigned checks=0;
void Check(bool yes,const char* message){++checks;if(!yes)throw std::runtime_error(message);}
template<class F>void Reject(F action,std::source_location at=std::source_location::current())
{++checks;try{action();}catch(const std::exception&){return;}throw std::runtime_error("Expected rejection at line "+std::to_string(at.line()));}
void Near(double actual,double expected,double tolerance=3e-5)
{++checks;if(!std::isfinite(actual)||std::abs(actual-expected)>tolerance*(1+std::abs(expected)))throw std::runtime_error("Pose oracle differs: actual="+std::to_string(actual)+" expected="+std::to_string(expected));}
using V=std::array<double,3>;using Q=std::array<double,4>;using M=std::array<double,16>;
V Cross(V a,V b){return {a[1]*b[2]-a[2]*b[1],a[2]*b[0]-a[0]*b[2],a[0]*b[1]-a[1]*b[0]};}
double Dot(Q a,Q b){double x=0;for(unsigned i=0;i<4;++i)x+=a[i]*b[i];return x;}
Q Unit(Q q){const auto d=std::sqrt(Dot(q,q));for(auto& x:q)x/=d;return q;}
Q Multiply(Q a,Q b)
{
 // Hamilton product via vector dot/cross, independent of original component code.
 V av{a[0],a[1],a[2]},bv{b[0],b[1],b[2]},c=Cross(av,bv);Q out{};
 for(unsigned i=0;i<3;++i)out[i]=a[3]*bv[i]+b[3]*av[i]+c[i];
 out[3]=a[3]*b[3]-(av[0]*bv[0]+av[1]*bv[1]+av[2]*bv[2]);return out;
}
Q Nlerp(Q a,Q b,double t){if(Dot(a,b)<=0)for(auto& x:b)x=-x;for(unsigned i=0;i<4;++i)a[i]=(1-t)*a[i]+t*b[i];return Unit(a);}
V Rotate(Q q,V v)
{
 // Quaternion vector rotation: v + 2w(q.xyz x v) + 2(q.xyz x(q.xyz x v)).
 V xyz{q[0],q[1],q[2]},u=Cross(xyz,v),w=Cross(xyz,u);
 for(unsigned i=0;i<3;++i)v[i]+=2*(q[3]*u[i]+w[i]);return v;
}
M Matrix(Q q,V scale,V translation)
{
 M out{};out[15]=1;
 for(unsigned axis=0;axis<3;++axis){V unit{};unit[axis]=1;auto v=Rotate(q,unit);for(unsigned c=0;c<3;++c)out[axis*4+c]=v[c]*scale[axis];out[12+axis]=translation[axis];}return out;
}
V Transform(V p,const M& m)
{V out{};for(unsigned c=0;c<3;++c){out[c]=m[12+c];for(unsigned r=0;r<3;++r)out[c]+=p[r]*m[4*r+c];}return out;}
nlMatrix4 Native(const M& m){nlMatrix4 out;for(unsigned i=0;i<16;++i)out.e[i]=float(m[i]);return out;}
M Identity(){return Matrix({0,0,0,1},{1,1,1},{0,0,0});}
void Compare(const nlMatrix4& actual,const M& expected,double tolerance=3e-5)
{for(unsigned i=0;i<16;++i)Near(actual.e[i],expected[i],tolerance);}
void Compare(const std::array<float,4>& actual,Q expected,double tolerance=3e-5)
{for(unsigned i=0;i<4;++i)Near(actual[i],expected[i],tolerance);}
std::array<float,3> F(V v){return {float(v[0]),float(v[1]),float(v[2])};}
std::array<float,4> F(Q q){return {float(q[0]),float(q[1]),float(q[2]),float(q[3])};}
V Translation(const cSHierarchy& h,unsigned i){const auto& v=h.GetTranslationOffset(i);return {v.x,v.y,v.z};}
struct Arenas
{
 void* one=::operator new(8*1024*1024,std::align_val_t(64));void* two=::operator new(8*1024*1024,std::align_val_t(64));
 Arenas(){ResetStartupMemory();StandardAllocator.Initialize(one,8*1024*1024);VirtualAllocator.Initialize(two,8*1024*1024);gMemoryInitialized=1;}
 ~Arenas(){ResetStartupMemory();::operator delete(one,std::align_val_t(64));::operator delete(two,std::align_val_t(64));}
};
void Tree(const std::vector<int>& parents,bool mirror=false)
{
 std::vector<bool> preserve(parents.size());for(unsigned i=0;i<parents.size();++i)preserve[i]=i%3==0;
 const int pelvis=parents.size()>5?2:-1,spine=parents.size()>5?5:-1;
 auto bytes=pose_fixture::Hierarchy(parents,preserve,pelvis,spine);auto hierarchy=HierarchyAsset::Decode(bytes);const auto& h=hierarchy->Data();
 PoseAccumulator pose(hierarchy,true);Check(pose.Nodes()==parents.size()&&!pose.HasPose(),"New pose state differs");
 Reject([&]{pose.Matrix(0);});Reject([&]{pose.PreviousMatrix(0);});
 const Q world_q=Unit({.1,-.2,.3,.9});const V world_scale{1.2,.9,1.1},world_translation{3,-2,5};
 const M world=Matrix(world_q,world_scale,world_translation);
 std::vector<Q> global_q(parents.size());std::vector<V> global_s(parents.size());std::vector<M> matrices(parents.size());
 for(unsigned i=0;i<parents.size();++i)
 {
  Q q=Unit({.03*double(i%3),.04*double(i%5),.05*double(i%2),1});V s{1+.01*(i%2),1-.005*(i%3),1+.002*(i%4)};
  V t=preserve[i]?Translation(h,i):V{.1*double(i+1),-.2*double(i%2),.125*double(i%3)};
  pose.BlendRotation(i,F(q),1,mirror);pose.BlendScale(i,F(s),1,mirror);if(!preserve[i])pose.BlendTranslation(i,F(t),1,mirror);
  if(mirror)
  {
   if(int(i)==pelvis||int(i)==spine)q={-q[3],q[2],q[1],-q[0]};
   else if(int(i)<pelvis)q={-q[0],q[1],-q[2],q[3]};
   else q={-q[0],-q[1],q[2],q[3]};
   if(!preserve[i]){if(int(i)<=pelvis||int(i)==spine)t[1]=-t[1];else t[2]=-t[2];}
  }
  q=Nlerp({0,0,0,1},q,1);
  if(i==0){global_q[i]=Multiply(world_q,q);for(unsigned c=0;c<3;++c)global_s[i][c]=s[c]*1.25*world_scale[c];t=Transform(t,world);}
  else{global_q[i]=Multiply(global_q[parents[i]],q);for(unsigned c=0;c<3;++c)global_s[i][c]=global_s[parents[i]][c]*s[c];t=Transform(t,matrices[parents[i]]);}
  matrices[i]=Matrix(global_q[i],global_s[i],t);
 }
 pose.Build(Native(world),1.25f);Check(pose.HasPose(),"Build did not publish pose");
 for(unsigned i=0;i<parents.size();++i){Compare(pose.Matrix(i),matrices[i],4e-4);Compare(pose.Quaternion(i),global_q[i],4e-4);Compare(pose.PreviousMatrix(i),Identity());}
 hierarchy.reset();bytes.clear();pose.Reset();pose.Build(Native(Identity()));
 for(unsigned i=0;i<parents.size();++i)Compare(pose.PreviousMatrix(i),matrices[i],4e-4);
 const auto retained=pose.Matrix(0);pose.Release();pose.Release();Reject([&]{pose.Matrix(0);});Compare(retained,Matrix({0,0,0,1},{1,1,1},preserve[0]?V{.25,-.5,.125}:V{0,0,0}));
}
void Blends()
{
 auto h=HierarchyAsset::Decode(pose_fixture::Hierarchy({-1,0,0},{false,true,false},1,2));PoseAccumulator p(h,true);
 auto build=[&]{p.Build(Native(Identity()));};
 p.BlendTranslation(0,{8,4,-2},std::nextafter(.001f,0.f));p.BlendScale(0,{3,4,5},0);p.BlendRotation(0,{0,0,0,1},0);build();Compare(p.Matrix(0),Identity());
 p.Reset();p.BlendTranslation(0,{8,4,-2},.001f);build();Compare(p.Matrix(0),Matrix({0,0,0,1},{1,1,1},{8,4,-2}));
 p.Reset();p.BlendTranslation(0,{8,4,-2},.25f);p.BlendTranslation(0,{0,8,2},.75f);p.BlendTranslationIdentity(0,1);
 p.BlendScale(0,{2,4,6},.25f);p.BlendScale(0,{4,2,2},.75f);p.BlendScaleIdentity(0,1);p.MultiplyScale(0,{2,4,6},.5f);
 const Q q1=Unit({.2,.1,.3,1}),q2=Unit({-.1,.3,.1,1});p.BlendRotation(0,F(q1),.25f);p.BlendRotation(0,F(q2),.75f);p.BlendRotationIdentity(0,1);
 const Q expected=Nlerp(Nlerp(Nlerp({0,0,0,1},q1,1),q2,.75),{0,0,0,1},.5);
 build();Compare(p.Matrix(0),Matrix(expected,{3.375,4.375,7},{1,3.5,.5}));Compare(p.Quaternion(0),expected);
 // Preserve-bone initialization is weight1; direct accumulator calls still
 // blend it. Only cSAnim::BlendTrans (outside this milestone) skips these nodes.
 p.Reset();const auto original=Translation(h->Data(),1);p.BlendTranslation(1,{10,20,30},1);build();
 V averaged{};for(unsigned c=0;c<3;++c)averaged[c]=(original[c]+V{10,20,30}[c])*.5;Compare(p.Matrix(1),Matrix({0,0,0,1},{1,1,1},averaged));
 p.Reset();build();Compare(p.Matrix(1),Matrix({0,0,0,1},{1,1,1},averaged));
 p.Reset();p.BlendAngle(0,0x4000,1);build();Compare(p.Matrix(0),Matrix({0,0,std::sqrt(.5),std::sqrt(.5)},{1,1,1},{0,0,0}),1e-4);
 p.Reset();p.BlendAngle(0,0xc000,1);p.BlendAngle(0,0x4000,1);build();
 // Signed16 shortest delta at the exact half-turn chooses -32768, yielding0x8000.
 Compare(p.Matrix(0),Matrix({0,0,1,0},{1,1,1},{0,0,0}),1e-4);
 PoseAccumulator no_previous(h,false);no_previous.Build(Native(Identity()));Reject([&]{no_previous.PreviousMatrix(0);});
}
void InvalidAndOwnership()
{
 auto hierarchy=HierarchyAsset::Decode(pose_fixture::Hierarchy({-1,0,1}));PoseAccumulator p(hierarchy,true);p.Build(Native(Identity()));const auto saved=p.Matrix(0);
 Reject([&]{p.BlendTranslation(3,{0,0,0},1);});Reject([&]{p.BlendRotation(0,{0,0,0,0},1);});Reject([&]{p.BlendScale(0,{1,NAN,1},1);});
 Reject([&]{p.BlendTranslation(0,{0,0,0},-1);});Reject([&]{p.BlendAngle(0,0,INFINITY);});Reject([&]{p.Build(Native(Identity()),NAN);});
 auto bad=Identity();bad[0]=0;Reject([&]{p.Build(Native(bad));});bad=Identity();bad[1]=.5;Reject([&]{p.Build(Native(bad));});bad=Identity();bad[0]=-1;Reject([&]{p.Build(Native(bad));});
 for(unsigned i=0;i<16;++i)Near(p.Matrix(0).e[i],saved.e[i]);Reject([&]{p.Matrix(3);});
 std::thread other([&]{Reject([&]{p.Reset();});Reject([&]{p.Release();});});other.join();
}
void BranchesAndHistory()
{
 auto h=HierarchyAsset::Decode(pose_fixture::Hierarchy({-1,0,0},{},-1,-1,{0xabcd,0xabcd,0xbcde}));PoseAccumulator p(h,true);
 const auto q=Unit({.2,-.3,.1,1});const Q z{0,0,std::sqrt(.5),std::sqrt(.5)};
 p.BlendRotationIdentity(0,.25f);p.BlendRotation(0,F(q),.75f);p.BlendAngle(0,0x4000,1);
 p.BlendTranslationIdentity(0,.25f);p.BlendTranslation(0,{4,8,12},.75f);
 p.BlendScaleIdentity(0,.25f);p.BlendScale(0,{2,3,4},.75f);
 p.BlendTranslation(1,{10,0,0},1);p.BlendTranslation(2,{0,20,0},1);
 Q expected=Nlerp({0,0,0,1},q,.75);M previous=Identity();
 for(unsigned repeat=0;repeat<3;++repeat)
 {
  // Build itself mixes the stored quaternion with the weighted Z channel.
  // Repeated Build without Reset therefore intentionally continues that mix.
  expected=Nlerp(expected,z,.5);const auto matrix=Matrix(expected,{1.75,2.5,3.25},{3,6,9});
  p.Build(Native(Identity()));Compare(p.Quaternion(0),expected);Compare(p.Matrix(0),matrix);Compare(p.PreviousMatrix(0),previous);
  Compare(p.MatrixByHash(0xabcd),matrix);Compare(p.MatrixByHash(0xbcde),Matrix(expected,{1.75,2.5,3.25},Transform({0,20,0},matrix)));
  previous=matrix;
 }
 Reject([&]{p.MatrixByHash(0xfeed);});p.Reset();Check(!p.HasPose(),"Reset published stale matrices");Reject([&]{p.MatrixByHash(0xabcd);});
 p.Build(Native(Identity()));Compare(p.Matrix(0),Identity());Compare(p.PreviousMatrix(0),previous);
 // Root scalar and squared world-axis gates are distinct source predicates.
 for(float scale:{.9998f,.99995f,1.f,1.00005f,1.0002f})
 {
  p.Reset();p.Build(Native(Identity()),scale);const double expected_scale=std::abs(scale-1.f)<.0001f?1:scale;
  Compare(p.Matrix(0),Matrix({0,0,0,1},{expected_scale,expected_scale,expected_scale},{0,0,0}),1e-7);
  auto world=Identity();world[0]=scale;p.Reset();p.Build(Native(world));const float squared=scale*scale;
  const double axis_scale=(squared<.9999f||squared>1.0001f)?scale:1;
  Compare(p.Matrix(0),Matrix({0,0,0,1},{axis_scale,1,1},{0,0,0}),2e-7);
 }
}
void Allocations()
{
 auto hierarchy=HierarchyAsset::Decode(pose_fixture::Hierarchy(std::vector<int>{-1,0,1,2,3,4,5,6,7,8,9,10,11,12,13,14}));
 const auto before1=StandardAllocator.TotalFreeMemory(),before2=VirtualAllocator.TotalFreeMemory();
 auto* selected=CurrentAllocator;
 {
  PoseAccumulator p(hierarchy,true);
  Check(StandardAllocator.TotalFreeMemory()==before1&&VirtualAllocator.TotalFreeMemory()<before2,"Pose arrays did not use their owning MEM2 arena");
  Check(CurrentAllocator==selected,"Pose construction changed the ambient allocator");
 }
 unsigned failures=0,successes=0;
 for(unsigned bytes:{64,256,512,1024,2048,2560,3072,3584,3840,4096,5120})
 {
  alignas(64)std::array<unsigned char,8192> memory{};MemoryAllocator tiny;tiny.Initialize(memory.data(),bytes);const auto tiny_free=tiny.TotalFreeMemory();auto saved=VirtualAllocator;VirtualAllocator=tiny;
  bool failed=false;try{PoseAccumulator p(hierarchy,true);}catch(const std::bad_alloc&){failed=true;}catch(...){VirtualAllocator=saved;throw;}
  const bool recovered=VirtualAllocator.TotalFreeMemory()==tiny_free;VirtualAllocator=saved;
  if(failed)++failures;else ++successes;
  Check(bytes>3072||failed,"Small native arena did not reach a constructor failure");Check(recovered,"Partial native pose allocation leaked");
  Check(CurrentAllocator==selected,"Failed pose construction changed the ambient allocator");
 }
 Check(failures>=7&&successes>=1,"Native arena sweep did not cover failed and complete construction");
 for(unsigned budget=0;budget<3;++budget)
 {
  auto live=allocation_probe::live.load();allocation_probe::budget=budget;
  try{PoseAccumulator p(hierarchy,true);}catch(const std::bad_alloc&){}catch(...){allocation_probe::budget=SIZE_MAX;throw;}
  allocation_probe::budget=SIZE_MAX;Check(allocation_probe::live==live,"Host pose allocation failure leaked storage");
 }
 Check(StandardAllocator.TotalFreeMemory()==before1&&VirtualAllocator.TotalFreeMemory()==before2,"Pose construction failed arena rollback");
}
void Owned(const std::filesystem::path& file)
{
 std::ifstream in(file,std::ios::binary);Check(bool(in),"Missing private hierarchy");pose_fixture::Blob bytes{std::istreambuf_iterator<char>(in),{}};
 auto hierarchy=HierarchyAsset::Decode(bytes);const auto& h=hierarchy->Data();PoseAccumulator p(hierarchy,true);
 std::vector<V> translations(h.GetNumNodes());
 for(unsigned pass=0;pass<2;++pass)
 {
  p.Reset();for(int i=0;i<h.GetNumNodes();++i)
  {
   V local=(pass||h.PreserveBoneLength(i))?Translation(h,i):V{};
   if(pass&&!h.PreserveBoneLength(i))p.BlendTranslation(i,F(local),1);
   translations[i]=local;if(i)for(unsigned c=0;c<3;++c)translations[i][c]+=translations[h.GetParent(i)][c];
  }
  p.Build(Native(Identity()));for(int i=0;i<h.GetNumNodes();++i)Compare(p.Matrix(i),Matrix({0,0,0,1},{1,1,1},translations[i]),1e-4);
 }
 std::cout<<"Owned hierarchy "<<file.filename().string()<<" nodes="<<h.GetNumNodes()<<" depth="<<hierarchy->MaximumDepth()<<'\n';
}
}
int main(int argc,char**argv)
{
 try
 {
  Arenas arenas;const auto free1=StandardAllocator.TotalFreeMemory(),free2=VirtualAllocator.TotalFreeMemory();
  if(argc>2&&std::string_view(argv[1])=="--owned")for(int i=2;i<argc;++i)Owned(argv[i]);
  else
  {
   Check(argc==1,"Use --owned FILE...");
   for(unsigned repeat=0;repeat<3;++repeat){Tree({-1});Tree({-1,0,1,0,3,3,0});Tree({-1,0,1,0,3,3,0},true);std::vector<int> deep{-1};for(int i=0;i<31;++i)deep.push_back(i);Tree(deep);Blends();BranchesAndHistory();InvalidAndOwnership();Allocations();}
  }
  Check(StandardAllocator.TotalFreeMemory()==free1&&VirtualAllocator.TotalFreeMemory()==free2,"Pose owner did not recover both arenas");
  std::cout<<checks<<" pose accumulator checks passed\n";
 }
 catch(const std::exception& e){std::cerr<<"FAILED: "<<e.what()<<" (check "<<checks<<")\n";return 1;}
}
