#include "runtime/animation_pose.h"
#include "runtime/pose_accumulator.h"
#include "runtime/graphics_memory.h"
#include "runtime/startup.h"
#include "runtime/startup_files.h"
#include "Game/SAnim.h"
#include "Game/SAnim/AnimRetargeter.h"
#include "Game/SHierarchy.h"
#include "Game/SAnimPoseSteps.h"
#include "NL/nlFile.h"
#include "NL/nlFileGC.h"
#include "animation_pose_fixture.h"
#include <aurora/aurora.h>
#include <aurora/dvd.h>
#include <SDL3/SDL.h>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <limits>
#include <source_location>
#include <thread>

namespace allocation_probe { thread_local std::size_t budget=SIZE_MAX; }
void* operator new(std::size_t n)
{
 if(allocation_probe::budget!=SIZE_MAX){if(!allocation_probe::budget)throw std::bad_alloc();--allocation_probe::budget;}
 if(void* p=std::malloc(n?n:1))return p;throw std::bad_alloc();
}
void operator delete(void* p)noexcept{std::free(p);}
void operator delete(void* p,std::size_t)noexcept{std::free(p);}
void* operator new[](std::size_t n){return ::operator new(n);}
void operator delete[](void* p)noexcept{::operator delete(p);}
void operator delete[](void* p,std::size_t)noexcept{::operator delete(p);}
using namespace mscharged;
namespace f=animation_pose_fixture;
namespace
{
unsigned checks=0;
void Check(bool okay,const char* message){++checks;if(!okay)throw std::runtime_error(message);}
template<class F>void Reject(F fn,std::source_location at=std::source_location::current())
{++checks;try{fn();}catch(const std::exception&){return;}throw std::runtime_error("Expected rejection at "+std::to_string(at.line()));}
void Near(double a,double b,double tolerance=7e-4)
{++checks;if(!std::isfinite(a)||std::abs(a-b)>tolerance*(1+std::abs(b)))throw std::runtime_error("Animation pose oracle differs: "+std::to_string(a)+" != "+std::to_string(b));}
using V=std::array<double,3>;using Q=std::array<double,4>;using M=std::array<double,16>;
Q Unit(Q q){double n=0;for(double x:q)n+=x*x;n=std::sqrt(n);for(auto& x:q)x/=n;return q;}
Q Mix(Q a,Q b,double t)
{double dot=0;for(unsigned i=0;i<4;++i)dot+=a[i]*b[i];for(unsigned i=0;i<4;++i)a[i]=(1-t)*a[i]+t*(dot<=0?-b[i]:b[i]);return Unit(a);}
V Cross(V a,V b){return {a[1]*b[2]-a[2]*b[1],a[2]*b[0]-a[0]*b[2],a[0]*b[1]-a[1]*b[0]};}
Q Product(Q a,Q b)
{V v=Cross({a[0],a[1],a[2]},{b[0],b[1],b[2]});Q q{};for(unsigned i=0;i<3;++i)q[i]=a[3]*b[i]+b[3]*a[i]+v[i];q[3]=a[3]*b[3]-a[0]*b[0]-a[1]*b[1]-a[2]*b[2];return q;}
M Matrix(Q q,V scale,V t)
{
 M m{};m[15]=1;V xyz{q[0],q[1],q[2]};
 for(unsigned row=0;row<3;++row){V axis{};axis[row]=1;auto a=Cross(xyz,axis),b=Cross(xyz,a);for(unsigned col=0;col<3;++col)m[row*4+col]=(axis[col]+2*(q[3]*a[col]+b[col]))*scale[row];m[12+row]=t[row];}return m;
}
V Transform(V p,const M& m){V t{};for(unsigned c=0;c<3;++c){t[c]=m[12+c];for(unsigned r=0;r<3;++r)t[c]+=p[r]*m[4*r+c];}return t;}
nlMatrix4 Identity(){nlMatrix4 m{};m.m11=m.m22=m.m33=m.m44=1;return m;}
void Compare(const nlMatrix4& actual,const M& expected){for(unsigned i=0;i<16;++i)Near(actual.e[i],expected[i]);}
void Same(const nlMatrix4& a,const nlMatrix4& b){for(unsigned i=0;i<16;++i)Check(std::bit_cast<unsigned>(a.e[i])==std::bit_cast<unsigned>(b.e[i]),"Published matrix bits changed");}
struct Accum
{
 Q q{0,0,0,1};V s{1,1,1},t{};double qw=0,sw=0,tw=0,aw=0;std::uint16_t angle=0;bool translation=false;
};
Q Decode(const cSAnim& a,unsigned node,unsigned key)
{
 const auto props=a.m_pNodeProperties[node];const unsigned width=props&16?8:props&32?6:4;
 const auto* bytes=static_cast<const std::uint8_t*>(a.m_pRotKeys[node])+key*width;Q out{};
 for(unsigned i=0;i<4;++i)
 {
  unsigned bits=width==8?(unsigned(bytes[2*i])<<8|bytes[2*i+1]):width==4?bytes[i]:i%2?(unsigned(bytes[i/2*3+2])<<4)|(bytes[i/2*3+1]&15u):(unsigned(bytes[i/2*3])<<4)|(bytes[i/2*3+1]>>4);
  const unsigned sign=width==8?32768:width==6?2048:128;const int value=bits&sign?int(bits)-int(sign*2):int(bits);out[i]=double(value)/sign;
 }
 return out;
}
std::vector<std::pair<unsigned,float>> Keys(unsigned frames,bool constant,float time,float weight)
{
 // Independent interval oracle: locate enclosing segment using floor, then
 // collapse the terminal/singleton interval. Preserve each float key weight.
 const float position=time*float(frames-1);const unsigned low=unsigned(std::floor(position));
 if(constant)return {{0,weight}};
 if(frames==1||low==frames-1)return {{frames-1,weight}};
 const float right=weight*(position-float(low));return {{low,weight-right},{low+1,right}};
}
std::vector<M> Oracle(const AnimationBundle& bundle,std::span<const AnimationPoseLayer> layers)
{
 const auto& h=bundle.Hierarchy()->Data();std::vector<Accum> accum(h.GetNumNodes());
 for(unsigned n=0;n<accum.size();++n)if(h.PreserveBoneLength(n)){const auto& t=h.GetTranslationOffset(n);accum[n].t={t.x,t.y,t.z};accum[n].tw=1;accum[n].translation=true;}
 auto vectorBlend=[](V& old,double& sum,V value,float weight){if(weight<.001f)return;sum+=weight;const double ratio=weight/sum;for(unsigned c=0;c<3;++c)old[c]=(1-ratio)*old[c]+ratio*value[c];};
 for(const auto& layer:layers)
 {
  const auto& a=bundle.At(layer.animation)->Data();const auto* map=bundle.Retarget(layer.animation);
  for(unsigned n=0;n<accum.size();++n)
  {
   auto& acc=accum[n];const unsigned mirrored=layer.mirror?h.m_pMirrorTable[n]:n;const int source=map?map->m_pMap[mirrored]:int(mirrored);
   if(source<0)
   {
    if(layer.scale_only)continue;
    if(layer.weight>=.001f){acc.qw+=layer.weight;acc.q=Mix(acc.q,{0,0,0,1},layer.weight/acc.qw);vectorBlend(acc.s,acc.sw,{1,1,1},layer.weight);}
    const auto& offset=h.GetTranslationOffset(n);acc.t={offset.x,offset.y,offset.z};continue;
   }
   const unsigned props=a.m_pNodeProperties[source];
   if(layer.scale_only)
   {
    if(!a.m_pScaleKeys[source]||layer.weight<.001f)continue;V interpolated{};
    for(auto [key,w]:Keys(a.m_nNumKeys,props&8,layer.time,1))
    {const auto& v=a.m_pScaleKeys[source][key];const V value{v.x/2048.,v.y/2048.,v.z/2048.};for(unsigned c=0;c<3;++c)interpolated[c]+=w*value[c];}
    for(unsigned c=0;c<3;++c)acc.s[c]*=(1-layer.weight)+layer.weight*interpolated[c];continue;
   }
   if(!a.m_pRotKeys[source])
   {if(layer.weight>=.001f){acc.qw+=layer.weight;acc.q=Mix(acc.q,{0,0,0,1},layer.weight/acc.qw);}}
   else for(auto [key,w]:Keys(a.m_nNumKeys,props&2,layer.time,layer.weight))
   {
    if(w<.001f)continue;
    if(props&1)
    {
     const auto angle=static_cast<const std::uint16_t*>(a.m_pRotKeys[source])[key];acc.aw+=w;
     int delta=(int(angle)-acc.angle+65536)%65536;if(delta>=32768)delta-=65536;
     const float ratio=float(w/acc.aw);acc.angle=std::uint16_t(int(acc.angle)+int(std::trunc(ratio*delta)));
    }
    else
    {
     Q q=Decode(a,source,key);
     if(layer.mirror)
     {
      if(int(n)==h.m_nSpineNodeIndex||int(n)==h.m_nPelvisNodeIndex)q={-q[3],q[2],q[1],-q[0]};
      else if(int(n)<h.m_nPelvisNodeIndex)q={-q[0],q[1],-q[2],q[3]};else q={-q[0],-q[1],q[2],q[3]};
     }
     acc.qw+=w;acc.q=Mix(acc.q,q,w/acc.qw);
    }
   }
   if(!a.m_pScaleKeys[source])vectorBlend(acc.s,acc.sw,{1,1,1},layer.weight);
   else for(auto [key,w]:Keys(a.m_nNumKeys,props&8,layer.time,layer.weight))
   {const auto& v=a.m_pScaleKeys[source][key];vectorBlend(acc.s,acc.sw,{v.x/2048.,v.y/2048.,v.z/2048.},w);}
   if(h.PreserveBoneLength(n))continue;
   if(!a.m_pTransKeys[source]){vectorBlend(acc.t,acc.tw,{0,0,0},layer.weight);}
   else for(auto [key,w]:Keys(a.m_nNumKeys,props&4,layer.time,layer.weight))
   {
    const auto& v=a.m_pTransKeys[source][key];V value{v.x,v.y,v.z};
    if(layer.mirror)value[int(n)<=h.m_nPelvisNodeIndex||int(n)==h.m_nSpineNodeIndex?1:2]*=-1;
    vectorBlend(acc.t,acc.tw,value,w);if(w>=.001f)acc.translation=true;
   }
  }
 }
 std::vector<M> matrices(accum.size());std::vector<Q> rotations(accum.size());std::vector<V> scales(accum.size());
 for(unsigned n=0;n<accum.size();++n)
 {
  auto& a=accum[n];const double radians=a.angle*(2*std::acos(-1.)/65536.);Q z{0,0,std::sin(radians/2),std::cos(radians/2)};
  Q q=a.qw?Mix(a.q,z,a.aw/(a.qw+a.aw)):a.aw?z:Q{0,0,0,1};V t=a.translation?a.t:V{};V scale=a.s;
  if(n){const unsigned p=h.GetParent(n);q=Product(rotations[p],q);t=Transform(t,matrices[p]);for(unsigned c=0;c<3;++c)scale[c]*=scales[p][c];}
  rotations[n]=q;scales[n]=scale;matrices[n]=Matrix(q,scale,t);
 }
 return matrices;
}
AnimationBundle::Handle Bundle(unsigned frames,std::vector<f::Node> nodes,bool preserve=false,bool mirror=false)
{auto data=f::Rig(nodes.size(),preserve,mirror);f::Append(data,f::Animation(frames,nodes));return AnimationBundle::Decode(f::World(data),f::World({}),f::Hash("rig"));}
void Verify(AnimationPose& pose,const AnimationBundle& bundle,std::span<const AnimationPoseLayer> layers)
{const auto oracle=Oracle(bundle,layers);const auto current=pose.Sample(layers,Identity());Check(current->matrices.size()==oracle.size(),"Published hierarchy size differs");for(unsigned i=0;i<oracle.size();++i)Compare(current->matrices[i],oracle[i]);}
void Channels()
{
 for(unsigned format:{0u,16u,32u,1u})for(unsigned frames:{1u,2u,3u,17u})for(bool constant:{false,true})
 {
  f::Node node;node.properties=format|(constant?14:0);
  const unsigned count=constant?1:frames;
  for(unsigned key=0;key<count;++key)
  {
   const float t=count==1?0:float(key)/float(count-1);
   if(format==1)node.angles.push_back(std::uint16_t(62000+unsigned(t*12000)));
   else node.rotation.push_back({0,0,.25f+t*.5f,.875f-t*.125f});
   node.scale.push_back({1+t,2-t*.5f,1});node.translation.push_back({t*4,2+t,-3+t});
  }
  auto bundle=Bundle(frames,{node});AnimationPose pose(bundle);
  for(float time:{0.f,std::nextafter(0.f,1.f),.0005f,.125f,.5f,.9995f,std::nextafter(1.f,0.f),1.f})
   for(float weight:{0.f,std::nextafter(.001f,0.f),.001f,.125f,1.f})for(bool mirror:{false,true})
   {const AnimationPoseLayer layer{0,time,weight,mirror,false};Verify(pose,*bundle,{&layer,1});}
  std::array layers{AnimationPoseLayer{0,.375f,.25f,false,false},AnimationPoseLayer{0,.625f,.75f,true,false},AnimationPoseLayer{0,.5f,.3f,false,true}};
  Verify(pose,*bundle,layers);
 }
 // Missing channel identity paths, zero-quaternion at an unused zero-weight key,
 // and unsigned scale decode beyond the previous manual sixteen-unit limit.
 f::Node empty,unused;unused.rotation={{{0,0,0,.9921875f}},{{0,0,0,0}}};
 auto sparse=Bundle(2,{unused,empty},true,true);AnimationPose p(sparse);
 AnimationPoseLayer layer{0,0,1,false,false};Verify(p,*sparse,{&layer,1});
 layer.time=1;auto old=p.Current();Reject([&]{p.Sample({&layer,1},Identity());});Check(p.Current()==old,"Invalid consumed quaternion replaced pose");
 layer.time=0;layer.mirror=true;Verify(p,*sparse,{&layer,1});
 f::Node large;large.properties=8;large.scale={{{65535.f/2048,16,1}}};auto max=Bundle(1,{large});AnimationPose max_pose(max);layer={};Verify(max_pose,*max,{&layer,1});
 PoseAccumulator manual(max->Hierarchy());for(float scale:{16.f,std::nextafter(16.f,32.f),32.f}){manual.Reset();manual.BlendScale(0,{scale,1,1},1);manual.Build(Identity());Near(manual.Matrix(0).m11,scale);manual.Reset();manual.MultiplyScale(0,{scale,1,1},1);manual.Build(Identity());Near(manual.Matrix(0).m11,scale);}
 Reject([&]{manual.BlendScale(0,{std::nextafter(32.f,INFINITY),1,1},1);});Reject([&]{manual.MultiplyScale(0,{std::nextafter(32.f,INFINITY),1,1},1);});
}
void Retarget()
{
 f::Node a,b,c;a.properties=b.properties=c.properties=14;a.translation={{{10,20,30}}};b.translation={{{40,50,60}}};c.translation={{{70,80,90}}};
 for(bool preserve:{false,true})
 {
  auto animations=f::Animation(2,{a,b,c},0x1111);f::Append(animations,f::Animation(2,{a,b,c},0x2222));
  const auto bundle=AnimationBundle::DecodeCharacter(f::Rig(2,preserve,true,"mario"),animations,f::Retarget({{0x1111,{2,-1}},{0x2222,{1,0}}}),0);AnimationPose pose(bundle);
  for(bool mirror:{false,true})for(float weight:{0.f,.25f,1.f})
  {
   std::array layers{AnimationPoseLayer{0,.5f,weight,mirror,false},AnimationPoseLayer{1,.2f,.75f,false,false}};
   Verify(pose,*bundle,layers);std::swap(layers[0],layers[1]);Verify(pose,*bundle,layers);
   layers[1].scale_only=true;Verify(pose,*bundle,layers);
  }
  AnimationPoseLayer layer{};auto value=pose.Sample({&layer,1},Identity());
  // Root maps to source2; child -1 keeps identity when unpreserved, or bind
  // translation when preserved. Direct writes do not change its weight/flag.
  Near(value->matrices[1].m41,preserve?72:70);
 }
}
void Failures()
{
 f::Node node;node.properties=14;node.translation={{{4,5,6}}};auto bundle=Bundle(1,{node});
 AnimationPoseFrame::Handle retained;const auto before1=StandardAllocator.TotalFreeMemory(),before2=VirtualAllocator.TotalFreeMemory();
 {
  AnimationPose pose(bundle);Check(!pose.Current(),"New sampler has stale publication");AnimationPoseLayer layer{};
  auto first=pose.Sample({&layer,1},Identity());Same(first->previous[0],Identity());
  auto world=Identity();world.m41=10;auto second=pose.Sample({&layer,1},world);Same(second->previous[0],first->matrices[0]);
  for(float value:{-.01f,std::nextafter(1.f,2.f),INFINITY,NAN})
  {layer.time=value;Reject([&]{pose.Sample({&layer,1},Identity());});Check(pose.Current()==second,"Invalid time changed publication");}
  layer={};for(float value:{-.01f,std::nextafter(1.f,2.f),INFINITY,NAN})
  {layer.weight=value;Reject([&]{pose.Sample({&layer,1},Identity());});Check(pose.Current()==second,"Invalid weight changed publication");}
  layer={};layer.animation=1;Reject([&]{pose.Sample({&layer,1},Identity());});layer={};
  world.m11=0;Reject([&]{pose.Sample({&layer,1},world);});Check(pose.Current()==second,"Invalid world changed publication");
  std::array<AnimationPoseLayer,257> too_many{};Reject([&]{pose.Sample(too_many,Identity());});
  std::atomic<bool> rejected=false;std::thread wrong([&]{try{pose.Current();}catch(const std::logic_error&){rejected=true;}});wrong.join();Check(rejected,"Cross-thread sampler access accepted");
  const auto scratch_free=VirtualAllocator.TotalFreeMemory();unsigned failed=0,succeeded=0;
  for(unsigned budget=0;budget<12;++budget)
  {
   const auto old=pose.Current();allocation_probe::budget=budget;
   bool failure=false;try{pose.Sample({&layer,1},Identity());}catch(const std::bad_alloc&){failure=true;}catch(...){allocation_probe::budget=SIZE_MAX;throw;}
   allocation_probe::budget=SIZE_MAX;
   if(failure){++failed;Check(pose.Current()==old,"Allocation failure published a partial pose");}else ++succeeded;
   Check(VirtualAllocator.TotalFreeMemory()==scratch_free,"Scratch sample leaked game storage");
   pose.Reset();Check(!pose.Current(),"Reset failed to hide pose");auto reset=pose.Sample({&layer,1},Identity());Same(reset->previous[0],Identity());
  }
  Check(failed>=4&&succeeded>0,"Publication allocation sweep lacks failing and successful cases");
  retained=pose.Current();bundle.reset();pose.Release();pose.Release();Reject([&]{pose.Current();});Reject([&]{pose.Reset();});Reject([&]{pose.Sample({},Identity());});Near(retained->matrices[0].m41,4);
 }
 Check(StandardAllocator.TotalFreeMemory()==before1&&VirtualAllocator.TotalFreeMemory()==before2,"Released sampler leaked either arena");
 bundle=Bundle(1,{node});unsigned failures=0,successes=0;
 for(unsigned bytes:{64,128,256,384,512,768,1024,1280,1536,2048,3072,4096})
 {
  alignas(64)std::array<unsigned char,8192> buffer{};MemoryAllocator tiny;tiny.Initialize(buffer.data(),bytes);auto saved=VirtualAllocator;VirtualAllocator=tiny;const auto before=VirtualAllocator.TotalFreeMemory();auto* ambient=CurrentAllocator;
  try{AnimationPose pose(bundle);++successes;}catch(const std::bad_alloc&){++failures;}catch(...){VirtualAllocator=saved;throw;}
  const bool recovered=VirtualAllocator.TotalFreeMemory()==before;VirtualAllocator=saved;Check(recovered&&CurrentAllocator==ambient,"Two-owner constructor failed rollback");
 }
 Check(failures>=4&&successes>0,"Two-owner arena sweep did not cover partial construction");
 auto data=f::Rig(1);f::Append(data,f::Animation(1,{node},0xabcd,1,true));auto morph=AnimationBundle::Decode(f::World(data),f::World({}),f::Hash("rig"));AnimationPose m(morph);AnimationPoseLayer layer{};
 Reject([&]{m.Sample({&layer,1},Identity());});Check(!m.Current(),"Unsupported morph published success");layer.scale_only=true;Verify(m,*morph,{&layer,1});
 // Both quaternions lie on the same rotation but opposite signs: the shortest
 // hemisphere is chosen by original NLerp, including near-zero key weights.
 node.properties=0;node.rotation={{{0,0,.5f,.75f}},{{0,0,-.5f,-.75f}}};node.translation.clear();auto opposite=Bundle(2,{node});AnimationPose signs(opposite);
 for(float time:{0.f,.0005f,.5f,.9995f,1.f}){layer={0,time,1,false,false};Verify(signs,*opposite,{&layer,1});}
}
struct Arenas
{
 void* a=::operator new(8*1024*1024,std::align_val_t(64));void* b=::operator new(8*1024*1024,std::align_val_t(64));
 Arenas(){ResetStartupMemory();StandardAllocator.Initialize(a,8*1024*1024);VirtualAllocator.Initialize(b,8*1024*1024);gMemoryInitialized=1;}
 ~Arenas(){ResetStartupMemory();::operator delete(a,std::align_val_t(64));::operator delete(b,std::align_val_t(64));}
};
struct Session
{
 bool live=false,disc=false;
 ~Session(){if(live)ResetStartupFiles();if(disc)aurora_dvd_close();if(live){ResetStartupMemory();aurora_shutdown();}}
};
void Pump(AnimationBundleLoad& load)
{
 const auto end=std::chrono::steady_clock::now()+std::chrono::seconds(20);
 while(load.State()==AnimationBundleState::Loading){load.Service();if(std::chrono::steady_clock::now()>end)throw std::runtime_error("Animation bundle read timeout");std::this_thread::yield();}
 load.Result();
}
void OwnedBundle(AnimationBundle::Handle bundle,const char* profile,bool require_moving_pose)
{
 AnimationPose pose(bundle);unsigned samples=0,morphs=0,scale_samples=0;double movement=0;
 for(unsigned animation=0;animation<bundle->Size();++animation)
 {
  const auto& a=bundle->At(animation)->Data();
  if(a.m_nNumMorphChannels)
  {
   ++morphs;AnimationPoseLayer layer{animation,0,1,false,false};auto old=pose.Current();
   Reject([&]{pose.Sample({&layer,1},Identity());});Check(pose.Current()==old,"Owned morph rejection changed publication");
   for(float time:{0.f,.5f,1.f}){layer={animation,time,1,false,true};Verify(pose,*bundle,{&layer,1});++scale_samples;}
   continue;
  }
  AnimationPoseFrame::Handle start;
  for(bool mirror:{false,true})for(float time:{0.f,.125f,.33333334f,.5f,.875f,std::nextafter(1.f,0.f),1.f})
  {
   AnimationPoseLayer layer{animation,time,1,mirror,false};Verify(pose,*bundle,{&layer,1});++samples;
   const auto current=pose.Current();if(!start)start=current;else for(unsigned n=0;n<current->matrices.size();++n)for(unsigned i=0;i<16;++i)movement+=std::abs(current->matrices[n].e[i]-start->matrices[n].e[i]);
  }
 }
 if(require_moving_pose){Check(samples>0,"Owned FE rig contains no qualified bone animation");Check(movement>0,"Owned FE animations never changed a pose");}
 else Check(samples>0||morphs==bundle->Size(),"Owned character boundary was not classified");
 std::cout<<"Owned "<<profile<<" hierarchy="<<std::hex<<bundle->Hierarchy()->Data().GetHashID()<<std::dec<<" nodes="<<bundle->Hierarchy()->Data().GetNumNodes()<<" tracks="<<bundle->Size()<<" samples="<<samples<<" rejected_morph_tracks="<<morphs<<" scale_only_samples="<<scale_samples<<" movement="<<movement<<'\n';
}
void Owned(int argc,char** argv)
{
 const auto directory=(std::filesystem::path(argv[3])/"animation-pose-data").string();std::filesystem::create_directories(directory);
 AuroraConfig config{};config.appName="Charged animation poses";config.userPath=config.cachePath=directory.c_str();config.resourcesPath=SDL_GetBasePath();config.desiredBackend=BACKEND_NULL;
 config.windowWidth=320;config.windowHeight=240;config.windowPosX=config.windowPosY=-1;config.logLevel=LOG_WARNING;config.mem1Size=MEM1_DEFAULT_SIZE;config.mem2Size=64*1024*1024;
 Session session;auto host=aurora_initialize(argc,argv,&config);session.live=true;Check(host.window,"Aurora initialization failed");InitializeStartupOS();nlInitMemory();
 Check(aurora_dvd_open(argv[2]),"Cannot open owned animation disc");session.disc=true;nlInitFileSystem();
 const auto a=StandardAllocator.TotalFreeMemory(),b=VirtualAllocator.TotalFreeMemory();
 {
  AnimationBundleLoad load;
  for(auto hash:{0x04fb2f26u,0xd625cf79u})
  {load.Begin({"Art/fe/environments/main/gameworld.res.zlib","Art/fe/environments/main/gameworld.tmp.zlib",hash});Pump(load);OwnedBundle(load.Result(),"frontend",true);}
  for(unsigned character:{0u,1u}){load.BeginCharacter(character);Pump(load);OwnedBundle(load.Result(),CharacterAnimation(character).name.data(),false);}
 }
 Check(StandardAllocator.TotalFreeMemory()==a&&VirtualAllocator.TotalFreeMemory()==b,"Owned pose/read teardown failed arena recovery");
}
}
int main(int argc,char** argv)
{
 try
 {
  if(argc==4&&std::string_view(argv[1])=="--owned")Owned(argc,argv);
  else
  {
   Check(argc==1,"Use --owned DISC OUTPUT_DIRECTORY");Arenas arenas;const auto a=StandardAllocator.TotalFreeMemory(),b=VirtualAllocator.TotalFreeMemory();
   Channels();Retarget();Failures();Check(StandardAllocator.TotalFreeMemory()==a&&VirtualAllocator.TotalFreeMemory()==b,"Generated tests leaked game arenas");
  }
  std::cout<<checks<<" animation pose checks passed\n";
 }
 catch(const std::exception& e){allocation_probe::budget=SIZE_MAX;std::cerr<<"FAILED: "<<e.what()<<" (check "<<checks<<")\n";return 1;}
}
