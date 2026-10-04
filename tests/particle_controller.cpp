#include "runtime/particle_controller.h"
#include "runtime/graphics_memory.h"
#include "runtime/startup.h"
#include "Game/Effects/ParticleSystem.h"
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <source_location>
#include <thread>
using namespace mscharged;
namespace
{
unsigned checks=0;
void Check(bool b,const char* message){++checks;if(!b)throw std::runtime_error(message);}
template<class F>void Reject(F action,std::source_location at=std::source_location::current())
{++checks;try{action();}catch(const std::exception&){return;}throw std::runtime_error("Expected rejection at line "+std::to_string(at.line()));}
void Near(float a,float b){Check(std::isfinite(a)&&std::abs(a-b)<.00002f,"Controller numerical oracle differs");}
struct Arenas
{
 void* one=::operator new(8*1024*1024,std::align_val_t(64));void* two=::operator new(8*1024*1024,std::align_val_t(64));
 Arenas(){ResetStartupMemory();StandardAllocator.Initialize(one,8*1024*1024);VirtualAllocator.Initialize(two,8*1024*1024);gMemoryInitialized=1;}
 ~Arenas(){ResetStartupMemory();::operator delete(one,std::align_val_t(64));::operator delete(two,std::align_val_t(64));}
};
std::vector<std::uint8_t> Read(const std::filesystem::path& p){std::ifstream f(p,std::ios::binary);if(!f)throw std::runtime_error("Missing fixture");return {std::istreambuf_iterator<char>(f),{}};}
EffectsRegistry::Handle Load(const std::filesystem::path& folder,const char* name,bool owned=false)
{
 auto files=std::make_shared<ParticleFiles>();std::array<std::string,4> names=owned?std::array<std::string,4>{"Art/effects/effects.bun","Art/effects/effectsnonres.bun","Art/objects/effectsgeometry.bun","Art/objects/effectsgeometrytextures.rlt"}:std::array<std::string,4>{std::string(name)+".bun","nonresident.bun","geometry.bun","textures.rlt"};
 for(unsigned i=0;i<4;++i){files->data[i]=Read(folder/names[i]);files->source_sizes[i]=files->data[i].size();}return EffectsRegistry::FromFiles(files);
}
std::uint32_t Random(std::uint32_t seed,unsigned count)
{for(unsigned i=0;i<count;++i){const auto a=seed^0x1d872b41U;const auto b=a^(a>>5);seed=b^a^(b<<27);}return seed;}
void Basic(const std::filesystem::path& folder)
{
 auto registry=Load(folder,"multi");auto before1=StandardAllocator.TotalFreeMemory(),before2=VirtualAllocator.TotalFreeMemory();const auto saved=uSeed;
 {
  ParticleControllers c({32,8,123});auto a=c.Start(registry,0x81f2a311);auto b=c.Start(registry,0x81f2a311,{false,71});
  auto initial=c.Snapshot();Check(initial.size()==2&&initial[0].token==b&&initial[0].id==71&&initial[1].id==1,"Original prepend/ID order differs");
  Reject([&]{ParticleSimulation other(registry,0x81f2a311,0);});Reject([&]{ParticleControllers other;});
  ParticleEmitterFrame frame;frame.position={10,20,30};frame.direction={1,0,0};frame.velocity={0,0,0};c.SetFrame(a,frame);
  const auto prior=VirtualAllocator.TotalFreeMemory();Reject([&]{c.Start(Load(folder,"bad"),0x81f2a311);});
  Check(c.Snapshot().size()==2&&c.Seed()==123&&VirtualAllocator.TotalFreeMemory()==prior,"Partial group construction did not roll back");
  auto bad=frame;bad.direction={0,0,0};Reject([&]{c.SetFrame(a,bad);});bad=frame;bad.time_scale=INFINITY;Reject([&]{c.SetFrame(a,bad);});
  Reject([&]{c.Advance(-1);});Reject([&]{c.Advance(NAN);});
  std::thread thread([&]{Reject([&]{c.Stop(a);});Reject([&]{c.Release();});});thread.join();
  std::vector<ParticleControllers::Token> calls;
  c.SetCallbacks(a,[&](auto&){calls.push_back(a);Reject([&]{c.Clear(a);});},{});
  c.SetCallbacks(b,[&](auto&){calls.push_back(b);},{});
  c.Advance(0);Check(calls==std::vector<ParticleControllers::Token>{b,a}&&c.Seed()==123,"Zero-step callback/order or no-emission semantics differ");calls.clear();
  Check(c.Advance(.25f),"Controllers ended too soon");
  // Four emitters: each Evaluate(rate) consumes 2 RNG calls, then two particles
  // each consume 25 calls. This oracle is independent unsigned recurrence.
  Check(c.Seed()==Random(123,4*52),"Shared multi-controller RNG order/count differs");
  Check(calls==std::vector<ParticleControllers::Token>{b,a},"Controller callback order differs");
  for(auto token:{a,b})for(unsigned spec=0;spec<2;++spec)Check(c.Particles(token,spec).size()==2,"Complete group failed to emit both specs");
  const auto p=c.Particles(a,0).front();Near(p.initial_position[0],10);Near(p.initial_position[1],20);Near(p.initial_position[2],30);
  Near(p.direction[0],1);Near(p.position[0],10.5f);Near(p.position[2],30.0625f);
  frame.disabled=true;c.SetFrame(a,frame);const auto age=c.Snapshot()[1].age;c.Advance(.25f);Near(c.Snapshot()[1].age,age);
  frame.disabled=false;frame.time_scale=.5f;c.SetFrame(a,frame);c.Advance(.25f);Near(c.Snapshot()[1].age,age+.125f);
  c.Clear(a);Check(c.Particles(a,0).empty()&&c.Particles(a,1).empty(),"Clear did not return all particles");
  std::vector<int> finished;c.SetCallbacks(a,{},[&](auto,int reason){finished.push_back(reason);});c.Stop(a);c.Stop(a);Check(finished==std::vector<int>{0},"Die callback was duplicated");
  for(unsigned i=0;i<16&&!c.Snapshot().empty();++i)c.Advance(.25f);
  Check(c.Snapshot().empty()&&finished==std::vector<int>({0,1,2}),"Natural/destructor callback order differs");Reject([&]{c.Stop(a);});
  c.Reset(321);auto fresh=c.Start(registry,0x81f2a311);Check(fresh!=a&&fresh!=b&&c.Snapshot()[0].id==1,"Reset reused token or retained ID sequence");c.Advance(.25f);Check(c.Seed()==Random(321,104),"Reset seed differs");
  c.Destroy(fresh);Check(c.Snapshot().empty(),"Destroy did not remove controller");c.Release();c.Release();Check(!c.Active(),"Release stayed active");
 }
 Check(uSeed==saved&&ParticleSystem::m_NumInstances==0,"Shared atlas/RNG ownership leaked");
 Check(StandardAllocator.TotalFreeMemory()==before1&&VirtualAllocator.TotalFreeMemory()==before2,"Controller pools leaked arenas");
 {
  ParticleControllers c({3,4,19});auto first=c.Start(registry,0x81f2a311);auto second=c.Start(registry,0x81f2a311,{false,0});c.Advance(.25f);
  Check(c.Particles(second,0).size()==2&&c.Particles(second,1).size()==1&&c.Particles(first,0).empty(),"Original shared pool capacity/order differs");
  // 4 rate evaluations (8 calls), three initializations (75 calls).
  Check(c.Seed()==Random(19,83),"Pool exhaustion changed original RNG consumption");
 }
}
void Failures(const std::filesystem::path& folder)
{
 auto registry=Load(folder,"multi");const auto before=VirtualAllocator.TotalFreeMemory();
 {
  ParticleControllers c({32,2,1});auto token=c.Start(registry,0x81f2a311);c.SetCallbacks(token,[](auto&){throw std::runtime_error("update");},{});
  Reject([&]{c.Advance(.25f);});Check(c.Failed(),"Callback failure did not poison controller domain");Reject([&]{c.Advance(0);});c.Reset(7);Check(!c.Failed()&&c.Snapshot().empty(),"Reset did not recover callback failure");
  token=c.Start(registry,0x81f2a311);unsigned destroyed=0;
  c.SetCallbacks(token,{},[&](auto,int reason){if(reason==2){++destroyed;throw std::runtime_error("finish");}});
  Reject([&]{c.Reset(9);});Check(destroyed==1&&c.Snapshot().empty()&&c.Seed()==9,"Throwing reset callback prevented cleanup");
  token=c.Start(registry,0x81f2a311);c.SetCallbacks(token,{},[](auto,int){throw std::runtime_error("destructor");});
 }
 Check(VirtualAllocator.TotalFreeMemory()==before,"Throwing callbacks leaked resources");
 for(unsigned bytes:{64,256,1024,4096})
 {
  alignas(64)unsigned char storage[8192];MemoryAllocator small{};small.Initialize(storage,bytes);auto saved=VirtualAllocator;VirtualAllocator=small;
  Reject([&]{ParticleControllers c({4096,2,1});});Check(VirtualAllocator.TotalFreeMemory()==small.TotalFreeMemory(),"Failed domain constructor leaked resources");VirtualAllocator=saved;
 }
 {
  ParticleControllers c({8,16,1});auto lingering=Load(folder,"linger");for(unsigned i=0;i<12;++i)c.Start(lingering,0x81f2a311);
  Reject([&]{c.Start(lingering,0x81f2a311);});Check(c.Snapshot().size()==12,"Rejected unqualified lingering eviction changed owner");
 }
}
void Owned(const std::filesystem::path& folder)
{
 auto registry=Load(folder,"",true);const auto before=VirtualAllocator.TotalFreeMemory();unsigned peak=0,total=0,drain=0;std::uint32_t seed=0;
 {
  ParticleControllers c({512,4,0x9184eb0c});auto a=c.Start(registry,0xe6650c7c);auto b=c.Start(registry,0xe6650c7c);registry.reset();
  Check(c.Snapshot()[0].systems==3&&c.Snapshot()[1].systems==3,"Owned full group did not retain all six systems");
  ParticleEmitterFrame f;f.position={30,0,0};c.SetFrame(b,f);
  for(unsigned i=0;i<120;++i){c.Advance(1.f/60);unsigned n=0;for(const auto& s:c.Snapshot())n+=s.particles;peak=std::max(peak,n);total+=n;}
  c.Stop(a);c.Stop(b);while(c.Advance(1.f/60)&&++drain<1200){}
  Check(c.Snapshot().empty()&&peak>0&&total>0,"Owned controllers failed to emit/drain");seed=c.Seed();
 }
 Check(VirtualAllocator.TotalFreeMemory()==before,"Owned controllers leaked arena");std::cout<<"Owned complete group=e6650c7c controllers=2 specs=6 peak="<<peak<<" particle_samples="<<total<<" drain="<<drain<<" seed="<<seed<<'\n';
}
}
int main(int argc,char** argv)
{
 try{Check(argc==2||argc==3,"Supply fixture folder [--owned]");Arenas arenas;if(argc==3){Check(std::string_view(argv[2])=="--owned","Unknown option");Owned(argv[1]);}else{for(unsigned i=0;i<3;++i){Basic(argv[1]);Failures(argv[1]);}}std::cout<<checks<<" particle controller checks passed\n";}
 catch(const std::exception& e){std::cerr<<"FAILED: "<<e.what()<<" (check "<<checks<<")\n";return 1;}
}
