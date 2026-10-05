#include "runtime/frontend_boot_audio.h"
#include "audio_bank_fixture.h"
#include <SDL3/SDL.h>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <new>
#include <source_location>
#include <thread>

thread_local long allocation_budget=-1;
void* operator new(std::size_t n){if(allocation_budget==0)throw std::bad_alloc();if(allocation_budget>0)--allocation_budget;if(auto p=std::malloc(n?n:1))return p;throw std::bad_alloc();}
void* operator new[](std::size_t n){return ::operator new(n);}
void operator delete(void* p)noexcept{std::free(p);}void operator delete[](void* p)noexcept{std::free(p);}
void operator delete(void* p,std::size_t)noexcept{std::free(p);}void operator delete[](void* p,std::size_t)noexcept{std::free(p);}
using namespace mscharged;using namespace mscharged::resources;using namespace audio_bank_fixture;
namespace
{
unsigned checks=0;
void Check(bool v,const char* why){++checks;if(!v)throw std::runtime_error(why);}
template<class F>void Reject(F f,std::source_location at=std::source_location::current())
{++checks;try{f();}catch(const std::exception&){return;}throw std::runtime_error("Invalid boot audio operation accepted at "+std::to_string(at.line()));}
Data Calculation()
{Data section;Append(section,0x23401,Words({2,0xf1000100,0}));Append(section,0x23402,Words({0,0,0,0,0,0,1,0,0,0xf1000100,0,0}));return Wrap(0x80000001,Wrap(0x80023400,section));}
LoadedAudioBank::Handle Loaded(AudioResidentBank::Handle bank)
{return std::make_shared<const LoadedAudioBank>(LoadedAudioBank{25,23,{25,"FE_GEN_Splash"},{23,0,1,false},std::move(bank)});}
Fixture LogoFixture()
{auto f=Make();Put(f.bytes,f.map+20,0xde83984e);Put(f.bytes,f.cues+40,0xde83984e);return f;}
LoadedAudioBank::Handle FixtureBank()
{const auto f=LogoFixture();return Loaded(ReadAudioResidentBank(f.bytes,f.wave));}
void WriteFixture(const char* directory)
{
 const auto root=std::filesystem::path(directory);std::filesystem::create_directories(root);
 Data table;constexpr unsigned base=0xe1000100;
 Append(table,0x23501,Words({1,0xf1000800,24,base,26,0xd1000400}));
 Append(table,0x23502,Words({0x55,0,24,0,0}));Data refs,slots,names;
 for(unsigned i=0;i<24;++i)
 {
  auto ref=Words({base+i*24});refs.insert(refs.end(),ref.begin(),ref.end());
  auto slot=Words({i,0,0,0,0,0});slots.insert(slots.end(),slot.begin(),slot.end());
 }
 Append(table,0x23503,refs);Append(table,0x23504,slots);
 for(unsigned i=0;i<26;++i){auto entry=Words({i,0});names.insert(names.end(),entry.begin(),entry.end());}
 Append(table,0x23505,names);
 for(unsigned i=0;i<26;++i)
 {
  const auto name=i==25?std::string("FE_GEN_Splash"):"unused"+std::to_string(i);
  Data bytes(name.begin(),name.end());bytes.push_back(0);Append(table,0x23506,bytes);
 }
 auto global=Wrap(0x80023500,table);const auto calculation=Calculation();
 global.insert(global.end(),calculation.begin()+8,calculation.end());global=Wrap(0x80000001,global);
 const auto fixture=LogoFixture();
 for(const auto& [name,bytes]:std::initializer_list<std::pair<const char*,const Data*>>{
  {"nlxgs.bun",&global},{"FE_GEN_Splash.resbun",&fixture.bytes},{"FE_GEN_Splash.nlxwb",&fixture.wave}})
 {
  std::ofstream file(root/name,std::ios::binary);file.write(reinterpret_cast<const char*>(bytes->data()),bytes->size());
  if(!file)throw std::runtime_error("Cannot write generated boot audio fixture");
 }
}
unsigned Next(unsigned seed)
{const unsigned a=seed^0x1d872b41U;const unsigned b=a^(a>>5);return b^a^(b<<27);}
unsigned Sample(unsigned seed)
{const auto r=seed%10;return r>=9?3:r>=7?4:r>=4?0:1;}
void Generated()
{
 SDL_SetHint(SDL_HINT_AUDIO_DRIVER,"dummy");auto loaded=FixtureBank();auto calculation=ReadAudioCalculationInitial(Calculation());unsigned seed=0xabcdef12;
 Reject([&]{FrontendBootAudio invalid({},calculation,seed);});Reject([&]{FrontendBootAudio invalid(loaded,{},seed);});
 for(unsigned mode=0;mode<5;++mode)
 {
  auto bad=std::make_shared<LoadedAudioBank>(*loaded);
  if(mode==0)bad->name_index=24;if(mode==1)bad->slot_index=22;if(mode==2)bad->name.name="other";if(mode==3)bad->slot.streaming=true;if(mode==4)bad->bank.reset();
  Reject([&]{FrontendBootAudio invalid(bad,calculation,seed);});
 }
 auto f=Make();Reject([&]{FrontendBootAudio invalid(Loaded(ReadAudioResidentBank(f.bytes,f.wave)),calculation,seed);});
 const auto before=seed;FrontendBootAudio bad_device(loaded,calculation,seed,0x1234567);
 const auto state=bad_device.SelectionState();Reject([&]{bad_device.PlayLogo();});
 Check(seed==before&&bad_device.Status().state==FrontendBootAudioState::Loaded&&!bad_device.Status().sample&&
  bad_device.SelectionState().selected==state.selected&&bad_device.SelectionState().counts==state.counts,
  "Failed SDL admission committed selection or shared RNG");
 Check(!SDL_WasInit(SDL_INIT_AUDIO),"Failed boot admission leaked device reference");bad_device.Unload();
 f=LogoFixture();Put(f.bytes,f.voices+8,Float(1));FrontendBootAudio unsupported(Loaded(ReadAudioResidentBank(f.bytes,f.wave)),calculation,seed);
 Reject([&]{unsupported.PlayLogo();});Check(seed==before&&unsupported.Status().state==FrontendBootAudioState::Loaded,"Unsupported source profile committed boot state");unsupported.Unload();
 unsigned failures=0;bool success=false;
 for(long budget=0;budget<100&&!success;++budget)
 {
  unsigned rng=17;FrontendBootAudio owner(loaded,calculation,rng);const auto prior=owner.SelectionState();
  allocation_budget=budget;bool failed=false;
  try{owner.PlayLogo();}catch(const std::bad_alloc&){failed=true;}allocation_budget=-1;
  if(failed){++failures;Check(rng==17&&owner.Status().state==FrontendBootAudioState::Loaded&&!owner.Status().sample&&owner.SelectionState().selected==prior.selected&&owner.SelectionState().counts==prior.counts,"Allocation failure committed partial boot audio state");Check(!SDL_WasInit(SDL_INIT_AUDIO),"Allocation failure leaked a live output");}
  else {success=true;Check(rng==Next(17)&&owner.Status().sample==Sample(17),"Successful transaction did not preserve original source RNG");}
  owner.Unload();
 }
 Check(success&&failures>=12,"Boot transaction allocation sweep missed preparation boundaries");
 for(unsigned session=0;session<4;++session)
 {
  auto bank=FixtureBank();auto calc=ReadAudioCalculationInitial(Calculation());
  std::weak_ptr<const LoadedAudioBank> weak=bank;std::weak_ptr<const AudioCalculationInitial> weak_calc=calc;
  unsigned rng=23;FrontendBootAudio owner(bank,calc,rng);bank.reset();calc.reset();
  Check(owner.Status().state==FrontendBootAudioState::Loaded&&!owner.Status().sample&&!weak.expired(),"Loaded owner status/retention differs");
  bool rejected=false;std::thread thread([&]{try{owner.PlayLogo();}catch(const std::logic_error&){rejected=true;}});thread.join();Check(rejected&&rng==23,"Foreign thread admitted boot audio");
  if(session!=0)
  {
   owner.PlayLogo();Check(rng==Next(23)&&owner.Status().sample==Sample(23)&&owner.SelectionState().counts[0]==1,"Boot logo source admission differs");
   const auto committed=rng;Reject([&]{owner.PlayLogo();});Reject([&]{owner.Unload(22);});Check(rng==committed&&!weak.expired(),"Rejected overlap/unrelated unload changed ownership");
   if(session==2)
   {
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(3);
    while(owner.Status().state!=FrontendBootAudioState::InputConsumed){if(std::chrono::steady_clock::now()>deadline)throw std::runtime_error("Boot SDL stream did not consume input");std::this_thread::sleep_for(std::chrono::milliseconds(2));}
    Check(!weak.expired()&&!weak_calc.expired(),"Consumed input prematurely released boot bank");Reject([&]{owner.PlayLogo();});
   }
  }
  owner.Unload();owner.Unload();Check(owner.Status().state==FrontendBootAudioState::Unloaded&&weak.expired()&&weak_calc.expired(),"Unload did not discard output and release bank");Reject([&]{owner.PlayLogo();});Reject([&]{owner.SelectionState();});
  Check(!SDL_WasInit(SDL_INIT_AUDIO),"Boot unload leaked SDL lifetime");
 }
 std::weak_ptr<const LoadedAudioBank> destroyed;
 {auto bank=FixtureBank();destroyed=bank;unsigned rng=4;FrontendBootAudio owner(bank,calculation,rng);bank.reset();owner.PlayLogo();}
 Check(destroyed.expired()&&!SDL_WasInit(SDL_INIT_AUDIO),"Destructor did not drain active boot stream");
}
Data Read(const char* path){std::ifstream f(path,std::ios::binary);if(!f)throw std::runtime_error("Cannot read supplied boot audio resource");return {std::istreambuf_iterator<char>(f),{}};}
void Owned(const char* global,const char* metadata,const char* wave)
{
 auto calculation=ReadAudioCalculationInitial(Read(global));auto loaded=Loaded(ReadAudioResidentBank(Read(metadata),Read(wave)));
 for(unsigned original:{0U,300U,600U,900U})
 {
  unsigned seed=original;FrontendBootAudio owner(loaded,calculation,seed);owner.PlayLogo();
  const unsigned r=original%1020,expected=r>=765?3:r>=510?4:r>=255?0:1;
  Check(owner.Status().sample==expected&&seed==Next(original),"Owned logo admission changed ordered selection/RNG");owner.Unload();
  Check(owner.Status().state==FrontendBootAudioState::Unloaded&&!SDL_WasInit(SDL_INIT_AUDIO),"Owned immediate unload left a stream");
 }
 std::cout<<"Owned four logo choices admitted and cancelled on real SDL dummy devices.\n";
}
}
int main(int argc,char** argv)
{try{if(argc==3&&std::string(argv[1])=="--fixture"){WriteFixture(argv[2]);return 0;}Generated();if(argc==4)Owned(argv[1],argv[2],argv[3]);else if(argc!=1)throw std::runtime_error("Usage: frontend_boot_audio_tests [GLOBAL RESBUN WAVE | --fixture DIRECTORY]");std::cout<<checks<<" boot audio checks passed\n";return 0;}catch(const std::exception& e){allocation_budget=-1;std::cerr<<e.what()<<" (check"<<checks<<")\n";return 1;}}
