#include "runtime/skin_pose.h"
#include "runtime/graphics_memory.h"
#include "runtime/startup.h"
#include "runtime/startup_files.h"
#include "Game/SHierarchy.h"
#include "NL/nlFile.h"
#include "NL/nlFileGC.h"
#include "NL/nlMemory.h"
#include "skin_pose_fixture.h"
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
namespace f=skin_fixture;
namespace
{
unsigned checks=0;
void Check(bool okay,const char* message){++checks;if(!okay)throw std::runtime_error(message);}
template<class F>void Reject(F fn,std::source_location at=std::source_location::current())
{++checks;try{fn();}catch(const std::exception&){return;}throw std::runtime_error("Expected rejection at line "+std::to_string(at.line()));}
void Near(double a,double b,double tolerance=3e-4)
{++checks;if(!std::isfinite(a)||std::abs(a-b)>tolerance*(1+std::abs(b)))throw std::runtime_error("Skin oracle differs: "+std::to_string(a)+" != "+std::to_string(b));}
using M=std::array<double,16>;using V=std::array<double,3>;
M Matrix(const nlMatrix4& source){M out;std::copy(std::begin(source.e),std::end(source.e),out.begin());return out;}
M Matrix(const std::array<float,16>& source){M out;std::copy(source.begin(),source.end(),out.begin());return out;}
M Identity(){M out{};out[0]=out[5]=out[10]=out[15]=1;return out;}
nlMatrix4 Native(const M& source){nlMatrix4 out;std::copy(source.begin(),source.end(),std::begin(out.e));return out;}
M Product(const M& a,const M& b)
{M out{};for(unsigned r=0;r<4;++r)for(unsigned c=0;c<4;++c)for(unsigned k=0;k<4;++k)out[r*4+c]+=a[r*4+k]*b[k*4+c];return out;}
M Inverse(M input)
{
 // Independent affine double cofactor oracle; the selected original SDK
 // routine instead performs float Gauss-Jordan elimination with pivoting.
 M inverse=Identity();
 auto cofactor=[&](unsigned row,unsigned col){std::array<unsigned,2> r{},c{};for(unsigned i=0,j=0;i<3;++i)if(i!=row)r[j++]=i;for(unsigned i=0,j=0;i<3;++i)if(i!=col)c[j++]=i;const auto minor=input[r[0]*4+c[0]]*input[r[1]*4+c[1]]-input[r[0]*4+c[1]]*input[r[1]*4+c[0]];return (row+col)%2?-minor:minor;};
 const auto determinant=input[0]*cofactor(0,0)+input[1]*cofactor(0,1)+input[2]*cofactor(0,2);
 Check(std::abs(determinant)>1e-12,"Oracle input is singular");
 for(unsigned r=0;r<3;++r)for(unsigned c=0;c<3;++c)inverse[c*4+r]=cofactor(r,c)/determinant;
 for(unsigned c=0;c<3;++c){inverse[12+c]=0;for(unsigned r=0;r<3;++r)inverse[12+c]-=input[12+r]*inverse[r*4+c];}
 return inverse;
}
V Transform(V p,const M& m)
{V out{};for(unsigned c=0;c<3;++c){out[c]=m[12+c];for(unsigned r=0;r<3;++r)out[c]+=p[r]*m[r*4+c];}return out;}
void Compare(const nlMatrix4& a,const M& b){for(unsigned i=0;i<16;++i)Near(a.e[i],b[i]);}
std::uint64_t Fingerprint(const resources::RigidSkinModel& model)
{
 std::uint64_t hash=14695981039346656037ull;
 auto word=[&](std::uint32_t value){for(int shift:{24,16,8,0}){hash^=(value>>shift)&255;hash*=1099511628211ull;}};
 auto real=[&](float value){word(std::bit_cast<std::uint32_t>(value));};
 word(model.hash);
 for(const auto& bind:model.binds){word(bind.hash);for(float x:bind.matrix)real(x);}
 for(const auto& p:model.packets)
 {
  word(p.primitive);word(p.program);word(p.raster);for(float x:p.matrix)real(x);
  for(auto t:p.material.textures){word(t.hash);word(t.flags);}real(p.material.blend);real(p.material.alpha);word(p.material.shadow_level);word(p.material.lighting_enabled);
  for(auto hash:p.bone_hashes)word(hash);for(auto index:p.indices)word(index);
  for(const auto& v:p.vertices){for(float x:v.position)real(x);for(float x:v.normal)real(x);for(auto uv:v.uv)for(auto x:uv)word(std::uint16_t(x));for(auto x:v.bones)word(x);for(auto x:v.weights)real(x);}
 }
 return hash;
}
std::vector<M> BindOracle(const RigidSkinAsset& asset)
{
 const auto& h=asset.Hierarchy()->Data();std::vector<M> out(h.GetNumNodes(),Identity());
 for(const auto& bind:asset.Data().binds){auto inverse=Inverse(Matrix(bind.matrix));int node=h.GetNodeIndexByID(bind.hash);if(node>=0)out[node]=inverse;}
 return out;
}
double Verify(const SkinPoseFrame& frame)
{
 const auto inverse=BindOracle(*frame.asset);const auto& model=frame.asset->Data();double sum=0;
 for(unsigned n=0;n<inverse.size();++n){Compare(frame.asset->InverseBinds()[n],inverse[n]);Compare(frame.matrices[n],Product(inverse[n],Matrix(frame.pose->matrices[n])));}
 for(unsigned p=0;p<model.packets.size();++p)
 {
  const auto& packet=model.packets[p];const auto& map=frame.asset->NodeMaps()[p];
  for(unsigned b=0;b<map.size();++b)for(unsigned r=0;r<3;++r)for(unsigned c=0;c<4;++c)
   Near(frame.packets[p][b].values[r][c],frame.matrices[map[b]].e[c*4+r],0);
  for(const auto& vertex:packet.vertices)
  {
   const unsigned slot=vertex.bones[0],node=map.at(slot);const auto expected=Product(inverse[node],Matrix(frame.pose->matrices[node]));
   const V original{vertex.position[0],vertex.position[1],vertex.position[2]};const auto point=Transform(original,expected);
   V actual{};for(unsigned r=0;r<3;++r){actual[r]=frame.packets[p][slot].values[r][3];for(unsigned c=0;c<3;++c)actual[r]+=original[c]*frame.packets[p][slot].values[r][c];Near(actual[r],point[r]);sum+=actual[r]*(r+1);}
   // Packet-local model transform remains separate until rendering. Assert a
   // reference composition that also exercises authored packet translation.
   const auto final=Transform(actual,Matrix(packet.matrix));const auto reference=Transform(original,Product(expected,Matrix(packet.matrix)));
   for(unsigned c=0;c<3;++c)Near(final[c],reference[c]);
  }
 }
 return sum;
}
void SetFloat(f::Blob& b,std::size_t at,float x){f::Set(b,at,std::bit_cast<unsigned>(x));}
void Formats()
{
 for(unsigned lane=0;lane<4;++lane)
 {
  auto bytes=f::Rlg(lane);const auto before=bytes;auto model=resources::ReadRigidSkinModel(bytes,0x99f34aae);
  Check(model.hash==0x99f34aae&&model.packets.size()==1&&model.binds.size()==2,"Model/bind metadata differs");
  const auto& p=model.packets[0];Check(p.program==0x041c3281&&p.raster==0x11002233&&p.vertices.size()==3&&p.indices==std::vector<std::uint16_t>{0,1,2},"Packet metadata differs");
  for(unsigned v=0;v<3;++v){Check(p.vertices[v].bones[lane]==v%2&&p.vertices[v].weights[lane]==1,"Reader changed authored influences");Check(p.vertices[v].uv[0][1]==-512+int(v)*1024,"Signed UV bits changed");}
  auto hierarchy=HierarchyAsset::Decode(f::Rig(2));auto asset=RigidSkinAsset::Decode(bytes,hierarchy);
  for(const auto& v:asset->Data().packets[0].vertices)Check(v.weights[0]==1&&v.bones[0]<2,"Original largest-weight-first was not selected");
  Check(bytes==before,"Skin decoder mutated source storage");
 }
 auto valid=f::Rlg();
 for(std::size_t n=0;n<valid.size();++n)Reject([&]{resources::ReadRigidSkinModel(resources::Bytes(valid.data(),n));});
 const auto packet=f::Find(valid,0x1b004),streams=f::Find(valid,0x1b005),vertices=f::Find(valid,0x1b006),indices=f::Find(valid,0x1b007),mat=f::Find(valid,0x1b016),bones=f::Find(valid,0x1b00b),morph=f::Find(valid,0x1b00c),binds=f::Find(valid,0x1b00a);
 auto fail=[&](auto mutate){auto bad=valid;mutate(bad);Reject([&]{resources::ReadRigidSkinModel(bad);});};
 fail([&](auto& b){f::Set(b,4,UINT32_MAX);});fail([&](auto& b){f::Set(b,packet+4,UINT32_MAX);});
 fail([&](auto& b){f::Set(b,packet,1);});fail([&](auto& b){b[packet+9]=0;});fail([&](auto& b){b[packet+10]=255;});
 fail([&](auto& b){b[packet+11]=5;});fail([&](auto& b){f::Set(b,packet+12,8);});fail([&](auto& b){f::Set(b,packet+16,0x1ace1d01);});
 fail([&](auto& b){f::Set(b,packet+24,1);});fail([&](auto& b){f::Set(b,packet+32,1);});
 fail([&](auto& b){b[indices+1]=3;});fail([&](auto& b){b[streams+5]=8;});fail([&](auto& b){b[streams+6]=2;});fail([&](auto& b){b[streams+7]=1;});
 fail([&](auto& b){f::Set(b,streams,UINT32_MAX);});fail([&](auto& b){b[mat+6]=4;});fail([&](auto& b){b[mat+7]=1;});
 for(float x:{-.1f,1.1f,INFINITY,NAN})for(unsigned off:{24u,28u})fail([&](auto& b){SetFloat(b,mat+off,x);});
 fail([&](auto& b){f::Set(b,mat+36,2);});fail([&](auto& b){f::Set(b,morph,1);});fail([&](auto& b){f::Set(b,morph+4,0);});fail([&](auto& b){f::Set(b,morph+8,2);});
 for(float x:{INFINITY,NAN,1e8f})fail([&](auto& b){SetFloat(b,vertices,x);});
 const auto bone_data=vertices+96,weight_data=vertices+108;
 fail([&](auto& b){b[bone_data]=2;});for(float x:{0.f,.5f,-1.f,INFINITY,NAN})fail([&](auto& b){SetFloat(b,weight_data,x);});
 fail([&](auto& b){b[bone_data+1]=0;SetFloat(b,weight_data+4,1);});
 auto hierarchy=HierarchyAsset::Decode(f::Rig(2));
 for(unsigned offset:{bones,binds}){auto bad=valid;f::Set(bad,offset,0xbad);Reject([&]{RigidSkinAsset::Decode(bad,hierarchy);});}
 for(float x:{0.f,1e-10f,NAN,INFINITY}){auto bad=valid;SetFloat(bad,binds+4,x);Reject([&]{RigidSkinAsset::Decode(bad,hierarchy);});}
 {auto bad=valid;SetFloat(bad,f::Find(bad,0x1b002),0);Reject([&]{RigidSkinAsset::Decode(bad,hierarchy);});}
 Reject([&]{RigidSkinAsset::Decode(valid,{});});Reject([&]{resources::ReadRigidSkinModel(valid,0x123);});
 // Ignored source cache values are never interpreted as stream slots/pointers.
 for(unsigned value:{0u,255u}){auto b=valid;for(unsigned i=0;i<6;++i)b[streams+i*8+4]=value;for(unsigned i=16;i<24;++i)b[mat+i]=value;Check(resources::ReadRigidSkinModel(b).packets[0].vertices.size()==3,"Serialized cache values became native references");}
 // Original bind records permit repeated IDs and later records win. Reference
 // only root in this case, so the second unreferenced node retains identity.
 auto duplicate=valid;f::Set(duplicate,binds+68,0x123000);f::Set(duplicate,bones+4,0x123000);
 auto asset=RigidSkinAsset::Decode(duplicate,hierarchy);Compare(asset->InverseBinds()[0],Inverse(Matrix(f::Matrix(1,0,3,0))));Compare(asset->InverseBinds()[1],Identity());
 // A well-formed additional collection entry cannot silently choose an asset.
 f::Blob collection;f::Append(collection,valid);f::Append(collection,valid);f::Blob root;f::Chunk(root,0x8001b100,collection);
 Reject([&]{resources::ReadRigidSkinModel(root);});Reject([&]{resources::ReadRigidSkinModel(root,0x99f34aae);});
}
AnimationBundle::Handle Bundle()
{
 f::Node a,b;a.properties=b.properties=0x10;
 a.rotation={{{0,0,0,.99996948f}},{{0,0,.5f,.75f}}};b.rotation={{{0,.25f,0,.875f}},{{.25f,0,0,.875f}}};
 a.translation={{{4,2,-6}},{{8,3,-2}}};b.translation={{{0,3,0}},{{1,4,2}}};a.scale={{{2,2,2}},{{1,1.5f,1}}};b.scale={{{1,1,1}},{{.75f,1,1.5f}}};
 auto bytes=f::Rig(2,false,true);f::Append(bytes,f::Animation(2,{a,b}));return AnimationBundle::Decode(f::World(bytes),f::World({}),f::Hash("rig"));
}
void Poses()
{
 auto bundle=Bundle();auto bytes=f::Rlg(3);auto asset=RigidSkinAsset::Decode(bytes,bundle->Hierarchy());SkinPose skin(asset);AnimationPose animation(bundle);Check(!skin.Current(),"New skin pose is already published");
 for(bool mirror:{false,true})for(float time:{0.f,std::nextafter(0.f,1.f),.125f,.5f,std::nextafter(1.f,0.f),1.f})
 {
  AnimationPoseLayer layer{0,time,1,mirror,false};auto world=Identity();world[12]=7;world[13]=-2;world[14]=3;
  Verify(*skin.Sample(animation.Sample({&layer,1},Native(world))));
 }
 auto last=skin.Current();auto source=last->pose;
 // Affine source input with translation larger than basis components makes
 // original Gauss-Jordan pivot across rows. Its computed fourth-column
 // roundoff must survive until the original GX3x4 conversion discards it.
 {auto rotated=bytes;const auto bind=f::Find(rotated,0x1b00a)+4;M matrix{.8,.6,0,0,-.6,.8,0,0,0,0,1,0,7,-11,5,1};for(unsigned i=0;i<16;++i)SetFloat(rotated,bind+i*4,float(matrix[i]));auto rig=RigidSkinAsset::Decode(rotated,bundle->Hierarchy());SkinPose roundoff(rig);Verify(*roundoff.Sample(source));}
 Reject([&]{skin.Sample({});});Check(skin.Current()==last,"Null pose changed published state");
 for(unsigned kind=0;kind<6;++kind)
 {
  auto bad=std::make_shared<AnimationPoseFrame>(*source);
  if(kind==0)bad->hierarchy=HierarchyAsset::Decode(f::Rig(2));
  if(kind==1)bad->matrices.clear();if(kind==2)bad->matrices[0].m11=INFINITY;
  if(kind==3)bad->matrices[0].m11=NAN;if(kind==4)bad->matrices[0].m14=1;
  if(kind==5)bad->matrices[0]=nlMatrix4{};
  Reject([&]{skin.Sample(bad);});Check(skin.Current()==last,"Malformed pose changed publication");
 }
 unsigned failures=0,successes=0;
 for(unsigned budget=0;budget<64;++budget)
 {
  allocation_probe::budget=budget;
  try{skin.Sample(source);allocation_probe::budget=SIZE_MAX;++successes;last=skin.Current();}
  catch(const std::bad_alloc&){allocation_probe::budget=SIZE_MAX;++failures;Check(skin.Current()==last,"Allocation failure changed publication");}
 }
 Check(failures>=4&&successes>0,"Publication allocation sweep missed partial construction");
 failures=successes=0;
 for(unsigned budget=0;budget<100;++budget)
 {
  allocation_probe::budget=budget;
  try{auto candidate=RigidSkinAsset::Decode(bytes,bundle->Hierarchy());allocation_probe::budget=SIZE_MAX;++successes;}
  catch(const std::bad_alloc&){allocation_probe::budget=SIZE_MAX;++failures;}
 }
 Check(failures>=10&&successes>0,"Asset allocation sweep missed partial construction");
 std::atomic<unsigned> rejected=0;std::thread worker([&]{for(unsigned op=0;op<4;++op)try{switch(op){case 0:skin.Current();break;case 1:skin.Sample(source);break;case 2:skin.Reset();break;default:skin.Release();}}catch(const std::logic_error&){++rejected;}});worker.join();Check(rejected==4,"Skin owner accepted wrong-thread mutation");
 auto retained=skin.Current();skin.Reset();Check(!skin.Current(),"Reset retained owner publication");skin.Sample(source);skin.Release();skin.Release();
 Reject([&]{skin.Current();});Reject([&]{skin.Sample(source);});Reject([&]{skin.Reset();});
 asset.reset();bundle.reset();Verify(*retained);
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
f::Blob ReadDisc(const char* path)
{
 unsigned long size=0;void* data=nlLoadEntireFile(path,&size,32,AllocateStart,nullptr,0,nullptr);
 std::unique_ptr<void,void(*)(void*)> owner(data,nlFree);Check(data&&size,"Owned skin file read failed");return f::Blob(static_cast<std::uint8_t*>(data),static_cast<std::uint8_t*>(data)+size);
}
void Owned(int argc,char** argv)
{
 const auto directory=(std::filesystem::path(argv[3])/"skin-pose-data").string();std::filesystem::create_directories(directory);
 AuroraConfig config{};config.appName="Charged retained rigid skin";config.userPath=config.cachePath=directory.c_str();config.resourcesPath=SDL_GetBasePath();config.desiredBackend=BACKEND_NULL;
 config.windowWidth=320;config.windowHeight=240;config.windowPosX=config.windowPosY=-1;config.logLevel=LOG_WARNING;config.mem1Size=MEM1_DEFAULT_SIZE;config.mem2Size=64*1024*1024;
 Session session;auto host=aurora_initialize(argc,argv,&config);session.live=true;Check(host.window,"Aurora initialization failed");InitializeStartupOS();nlInitMemory();
 Check(aurora_dvd_open(argv[2]),"Cannot open owned skin disc");session.disc=true;nlInitFileSystem();
 const auto a=StandardAllocator.TotalFreeMemory(),b=VirtualAllocator.TotalFreeMemory();
 {
  AnimationBundleLoad load;load.BeginCharacter(1);
  const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(20);
  while(load.State()==AnimationBundleState::Loading){load.Service();if(std::chrono::steady_clock::now()>deadline)throw std::runtime_error("Owned animation timeout");std::this_thread::yield();}
  auto bundle=load.Result();AnimationPose animation(bundle);
  Check(CharacterAnimation(1).name=="bowser","Owned character association changed");
  for(const char* path:{"Art/characters/bowser/bowser_shock.rlg","Art/characters/bowser/bowser_shadow.rlg"})
  {
   auto data=ReadDisc(path);auto asset=RigidSkinAsset::Decode(data,bundle->Hierarchy());SkinPose skin(asset);unsigned samples=0,vertices=0;for(auto& p:asset->Data().packets)vertices+=p.vertices.size();double movement=0,first=0;
   for(unsigned track=0;track<bundle->Size();++track)for(bool mirror:{false,true})for(float time:{0.f,.125f,.5f,std::nextafter(1.f,0.f),1.f})
   {
    AnimationPoseLayer layer{track,time,1,mirror,false};auto frame=skin.Sample(animation.Sample({&layer,1},Native(Identity())));auto sum=Verify(*frame);if(samples++)movement+=std::abs(sum-first);else first=sum;
   }
   Check(samples&&vertices&&movement>0,"Owned authored mesh never moved");
   std::cout<<"Owned "<<path<<" hash="<<std::hex<<asset->Data().hash<<" fingerprint="<<Fingerprint(asset->Data())<<std::dec<<" packets="<<asset->Data().packets.size()<<" vertices="<<vertices<<" binds="<<asset->Data().binds.size()<<" samples="<<samples<<" movement="<<movement<<'\n';
  }
  auto body=ReadDisc("Art/characters/bowser/bowser.rlg");Reject([&]{RigidSkinAsset::Decode(body,bundle->Hierarchy());});
 }
 Check(StandardAllocator.TotalFreeMemory()==a&&VirtualAllocator.TotalFreeMemory()==b,"Owned skin teardown failed arena recovery");
}
}
int main(int argc,char** argv)
{
 try
 {
  if(argc==4&&std::string_view(argv[1])=="--owned")Owned(argc,argv);
  else{Check(argc==1,"Use --owned DISC OUTPUT_DIRECTORY");Arenas arenas;const auto a=StandardAllocator.TotalFreeMemory(),b=VirtualAllocator.TotalFreeMemory();Formats();Poses();Check(StandardAllocator.TotalFreeMemory()==a&&VirtualAllocator.TotalFreeMemory()==b,"Skin tests leaked arenas");}
  std::cout<<checks<<" skin pose checks passed\n";
 }
 catch(const std::exception& e){allocation_probe::budget=SIZE_MAX;std::cerr<<"FAILED: "<<e.what()<<" (check "<<checks<<")\n";return 1;}
}
