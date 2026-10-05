#include "runtime/frontend_audio.h"
#include "audio_bank_fixture.h"
#include <SDL3/SDL.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <new>
#include <source_location>
#include <thread>
using namespace mscharged;using namespace mscharged::resources;using namespace audio_bank_fixture;
thread_local long allocation_budget=-1;
void* operator new(std::size_t n){if(allocation_budget==0)throw std::bad_alloc();if(allocation_budget>0)--allocation_budget;if(auto p=std::malloc(n?n:1))return p;throw std::bad_alloc();}
void* operator new[](std::size_t n){return ::operator new(n);}
void operator delete(void*p)noexcept{std::free(p);}void operator delete[](void*p)noexcept{std::free(p);}
void operator delete(void*p,std::size_t)noexcept{std::free(p);}void operator delete[](void*p,std::size_t)noexcept{std::free(p);}
namespace
{
unsigned checks=0;constexpr unsigned Key=0x10203040;
void Check(bool value,const char* message){++checks;if(!value)throw std::runtime_error(message);}
template<class F>void Reject(F action,std::source_location at=std::source_location::current())
{++checks;try{action();}catch(const std::exception&){return;}throw std::runtime_error("Invalid frontend audio accepted at "+std::to_string(at.line()));}
Data Read(const std::filesystem::path&path){std::ifstream f(path,std::ios::binary);Check(bool(f),"Cannot open audio evidence");return{std::istreambuf_iterator<char>(f),{}};}
Data Calculation(){Data s;Append(s,0x23401,Words({2,0xf1000100,0}));Append(s,0x23402,Words({0,0,0,0,0,0,1,0,0,0xf1000100,0,0}));return Wrap(0x80000001,Wrap(0x80023400,s));}
auto Calc(){return ReadAudioCalculationInitial(Calculation());}
auto Loaded(const Fixture& f)
{return std::make_shared<LoadedAudioBank>(LoadedAudioBank{23,21,{23,"FE_GEN_Sfx"},{21,0x1234,1,false},ReadAudioResidentBank(f.bytes,f.wave)});}
unsigned Next(unsigned seed){const auto a=seed^0x1d872b41u,b=a^(a>>5);return b^a^(b<<27);}
unsigned Sample(unsigned seed){const auto draw=seed%10;return draw>=9?3:draw>=7?4:draw>=4?0:1;}
void State(FrontendAudio& audio,FrontendAudioHandle h,int cue,int instance,int event,int source)
{const auto s=audio.Status(h);Check(s.state==cue&&s.instance_state==instance&&s.event_state==event&&s.source_state==source,"Independent cue/instance/event/source state oracle differs");}
void Pump(FrontendAudio& owner,const std::vector<FrontendAudioHandle>& handles)
{
 const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(6);
 for(;;)
 {
  owner.ServiceAudio();owner.Update(1.0f/60);
  if(std::all_of(handles.begin(),handles.end(),[&](auto h){return owner.IsFinished(h);}))break;
  Check(std::chrono::steady_clock::now()<deadline,"Real frontend sound did not finish");std::this_thread::sleep_for(std::chrono::milliseconds(2));
 }
}
void Lifecycle()
{
 SDL_SetHint(SDL_HINT_AUDIO_DRIVER,"dummy");auto bank=Loaded(Make());auto calc=Calc();unsigned seed=0xabcdef12,before=seed;
 FrontendAudio audio(bank,calc);auto h=*audio.Play(Key,seed,false);
 Check(seed==Next(before)&&audio.Status(h).sample==Sample(before),"Original ordered selection/RNG differs");
 Check(audio.ActiveCount(Key)==1&&!audio.IsFinished(h),"Cue not counted/preparing");State(audio,h,2,2,2,1);
 audio.Update(0);State(audio,h,4,4,4,3);audio.ServiceAudio();audio.Update(0);State(audio,h,4,4,4,4);
 audio.Stop(h,false);State(audio,h,7,7,7,4);Check(audio.IsFinished(h),"Original FE finished predicate changed stopping7");
 audio.ServiceAudio();audio.Update(0);State(audio,h,7,7,7,6);Check(audio.ActiveCount(Key)==1,"Public source6 prematurely decremented cue count");
 audio.ServiceAudio();audio.Update(0);State(audio,h,8,8,0,0);Check(audio.ActiveCount(Key)==1,"Stopped retained cue was destroyed without release");
 audio.Release(h);Check(audio.Status(h).state==9&&audio.ActiveCount(Key)==1,"Release did not defer original9 removal");audio.Update(0);
 Check(audio.IsFinished(h)&&audio.ActiveCount(Key)==0&&audio.Handles().empty(),"Release did not decrement count/remove source");Reject([&]{audio.Status(h);});
 const auto next=*audio.Play(Key,seed);Check(next.slot==h.slot&&next.generation==h.generation+1,"Cue generation was not advanced");Reject([&]{audio.Stop(h);});
 audio.Cancel(next);Check(audio.ActiveCount(Key)==0&&audio.IsFinished(next),"Explicit cancellation retained cue/source");
 auto pending=*audio.Play(Key,seed);audio.Update(0);audio.Stop(pending,false);audio.ServiceAudio();audio.Update(0);
 State(audio,pending,7,7,7,4); // Source pending5 Stop is no-op; actual playback still starts.
 audio.Cancel(pending);
 for(bool service_first:{false,true})
 {
  auto prepared=*audio.Play(Key,seed);audio.Stop(prepared);State(audio,prepared,7,7,7,6);
  if(service_first)audio.ServiceAudio();else {audio.Update(0);State(audio,prepared,7,7,7,6);audio.ServiceAudio();}
  audio.Update(0);Check(audio.IsFinished(prepared)&&audio.ActiveCount(Key)==0,"Prepared Stop ordering did not finish and autorelease");
 }
 audio.Enable(false);before=seed;Check(!audio.Play(Key,seed)&&seed==before,"Disabled FE consumed selection");audio.Enable(true);
 Check(!audio.Play(UINT32_MAX,seed)&&!audio.Play(0xeeeeeeee,seed)&&seed==before,"Sentinel/missing FE cue consumed selection");
 Reject([&]{audio.Update(-1);});Reject([&]{audio.Update(INFINITY);});Reject([&]{audio.Update(NAN);});Reject([&]{audio.Update(1.001f);});
 auto other=FrontendAudio(bank,calc);auto foreign=*other.Play(Key,seed);Reject([&]{audio.Status(foreign);});Reject([&]{audio.IsFinished({});});other.Unload();
 bool rejected=false;std::thread t([&]{try{audio.Update(0);}catch(const std::logic_error&){rejected=true;}});t.join();Check(rejected,"Foreign frontend audio thread accepted");
 Reject([&]{audio.Unload(22);});Check(audio.Loaded(),"Unrelated slot unload changed bank");audio.Unload();audio.Unload();
 Check(!audio.Loaded()&&!SDL_WasInit(SDL_INIT_AUDIO),"Unload retained SDL/bank owner");Reject([&]{audio.Play(Key,seed);});
}
void LimitsAndRollback()
{
 SDL_SetHint(SDL_HINT_AUDIO_DRIVER,"dummy");auto f=Make();Put(f.bytes,f.cues+40+32,1);auto bank=Loaded(f);auto calc=Calc();unsigned seed=17;
 FrontendAudio limited(bank,calc,{4});auto first=*limited.Play(Key,seed);const auto selected=limited.SelectionState(Key);const auto saved_seed=seed;
 auto rejected=*limited.Play(Key,seed);Check(limited.Status(rejected).limited&&limited.Status(rejected).state==6&&limited.ActiveCount(Key)==2,"Original maximum-count rejected handle was not retained/count incremented");
 Check(seed==saved_seed&&limited.SelectionState(Key).counts==selected.counts,"Maximum-count rejection consumed RNG/selection");limited.Update(0);
 Check(limited.Status(rejected).state==8&&limited.ActiveCount(Key)==2,"Rejected cue did not pass6→8 with callback disabled");
 limited.Stop(rejected);limited.Update(0);Check(limited.ActiveCount(Key)==1,"Rejected handle release did not decrement active count");limited.Cancel(first);limited.Unload();
 auto zero=Make();Put(zero.bytes,zero.cues+40+32,0);FrontendAudio disabled(Loaded(zero),calc);const auto z=*disabled.Play(Key,seed);Check(disabled.Status(z).state==6&&seed==saved_seed,"Maximum0 should never select");disabled.Cancel(z);disabled.Unload();
 FrontendAudio owner(Loaded(Make()),calc,{3});const auto existing=*owner.Play(Key,seed,false);const auto state=owner.SelectionState(Key);const auto original_seed=seed;
 unsigned failures=0;bool success=false;
 for(long budget=0;budget<100&&!success;++budget)
 {
  allocation_budget=budget;std::optional<FrontendAudioHandle> result;bool failed=false;
  try{result=owner.Play(Key,seed);}catch(const std::bad_alloc&){failed=true;}allocation_budget=-1;
  if(failed){++failures;Check(seed==original_seed&&owner.ActiveCount(Key)==1&&owner.Handles()==std::vector{existing}&&owner.SelectionState(Key).counts==state.counts,"Failed cue publication changed selection/RNG/count/live source");}
  else {success=true;owner.Cancel(*result);}
 }
 Check(success&&failures>=12,"Cue allocation sweep did not reach output preparation");owner.Cancel(existing);owner.Unload();
 FrontendAudio bad_device(Loaded(Make()),calc,{2,64*1024*1024,0x1234567});const auto before=seed;Reject([&]{bad_device.Play(Key,seed);});Check(seed==before&&bad_device.ActiveCount(Key)==0&&bad_device.Handles().empty(),"Failed real SDL admission published a cue");bad_device.Unload();
 FrontendAudio capacity(Loaded(Make()),calc,{1});auto cap=*capacity.Play(Key,seed);const auto cap_seed=seed;Reject([&]{capacity.Play(Key,seed);});Check(seed==cap_seed&&capacity.ActiveCount(Key)==1,"Capacity failure changed cue count");capacity.Cancel(cap);capacity.Unload();
 for(unsigned mode=0;mode<5;++mode)
 {
  auto bad=Make();if(mode==0)Put(bad.bytes,bad.sounds+4,2);if(mode==1)Put(bad.bytes,bad.voices+8,Float(1));if(mode==2)bad=Make(1,3,true);if(mode==3)bad=Make(1,3,false,true);if(mode==4)bad.bytes.at(bad.sounds+20)=1;
  FrontendAudio unsupported(Loaded(bad),calc);auto rng=seed;Reject([&]{unsupported.Play(Key,rng);});Check(rng==seed&&unsupported.ActiveCount(Key)==0,"Unsupported profile changed selection/count");
 }
 Check(!SDL_WasInit(SDL_INIT_AUDIO),"Admission errors leaked SDL references");
 auto retained=Loaded(Make());std::weak_ptr<const AudioResidentBank> weak=retained->bank;std::weak_ptr<const AudioCalculationInitial> weak_calc=calc;
 auto owner_ptr=std::make_unique<FrontendAudio>(retained,calc);retained->name.name="mutated";retained->bank.reset();retained.reset();calc.reset();auto live=*owner_ptr->Play(Key,seed);owner_ptr->Update(0);owner_ptr->ServiceAudio();
 Check(!weak.expired()&&!weak_calc.expired(),"Live cue lost immutable bank/calculation");owner_ptr->Unload();Check(weak.expired()&&weak_calc.expired(),"Unload retained bank/calculation owner");
 // Every already-unloaded owner must release both retained asset handles.
 owner_ptr.reset();(void)live;
}
Fixture LongFixture(unsigned blocks)
{
 auto f=Make();f.wave.assign(5*blocks*8,0);for(unsigned sample=0;sample<5;++sample)
 {
  const unsigned base=sample*blocks*16;auto at=f.sp+4+sample*28;Put(f.bytes,at+4,44100);Put(f.bytes,at+16,base+blocks*16-1);Put(f.bytes,at+20,base+2);
  std::fill_n(f.bytes.begin()+f.sp+4+5*28+sample*46,46,0);for(unsigned block=0;block<blocks;++block)std::fill_n(f.wave.begin()+sample*blocks*8+block*8+1,7,std::uint8_t((sample+1)*17));
 }return f;
}
void Concurrent()
{
 auto path=std::filesystem::temp_directory_path()/("charged-fe-audio-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())+".raw");
 struct Cleanup{std::filesystem::path path;~Cleanup(){std::error_code e;std::filesystem::remove(path,e);}}cleanup{path};
 SDL_SetHint(SDL_HINT_AUDIO_DRIVER,"disk");SDL_SetHint(SDL_HINT_AUDIO_DISK_OUTPUT_FILE,path.string().c_str());SDL_SetHint(SDL_HINT_AUDIO_FORMAT,"F32");SDL_SetHint(SDL_HINT_AUDIO_FREQUENCY,"44100");SDL_SetHint(SDL_HINT_AUDIO_CHANNELS,"2");
 FrontendAudio audio(Loaded(LongFixture(2048)),Calc(),{2});unsigned a=4,b=0; // old seed%10 yields sample0 andsample1 independently.
 auto first=*audio.Play(Key,a),second=*audio.Play(Key,b);Check(audio.Status(first).sample==0&&audio.Status(second).sample==1,"Concurrent source selection oracle differs");
 audio.Update(0);audio.ServiceAudio();Pump(audio,{first,second});Check(audio.ActiveCount(Key)==0&&audio.Handles().empty(),"Autoreleased concurrent handles retained active counts");
 std::this_thread::sleep_for(std::chrono::milliseconds(60));audio.Unload();auto bytes=Read(path);Check(bytes.size()%8==0,"Disk stereo output alignment differs");
 const float gain=32767.0f/32768.0f*(23197.0f/32768.0f),expected=1.0f/32768.0f*gain+2.0f/32768.0f*gain;unsigned matched=0;
 for(std::size_t at=0;at<bytes.size();at+=8){float l,r;std::memcpy(&l,bytes.data()+at,4);std::memcpy(&r,bytes.data()+at+4,4);Check(std::isfinite(l)&&std::isfinite(r),"Frontend output nonfinite");if(std::abs(l-expected)<1e-10f&&std::abs(r-expected)<1e-10f)++matched;}
 Check(matched>=1000,"Concurrent frontend cues were not genuinely mixed");std::cout<<matched<<" frames match independent FE stereo sum\n";
 SDL_ResetHint(SDL_HINT_AUDIO_DISK_OUTPUT_FILE);SDL_ResetHint(SDL_HINT_AUDIO_FORMAT);SDL_ResetHint(SDL_HINT_AUDIO_FREQUENCY);SDL_ResetHint(SDL_HINT_AUDIO_CHANNELS);SDL_ResetHint(SDL_HINT_AUDIO_DRIVER);
 SDL_SetHint(SDL_HINT_AUDIO_DRIVER,"dummy");auto bank=Loaded(LongFixture(1024));
 Check(SDL_InitSubSystem(SDL_INIT_AUDIO),"External audio reference failed");
 {FrontendAudio owner(bank,Calc(),{2});unsigned seed=23;auto x=*owner.Play(Key,seed),y=*owner.Play(Key,seed);owner.Update(0);owner.ServiceAudio();owner.Cancel(x);Check(owner.ActiveCount(Key)==1&&!owner.IsFinished(y),"Cancel removed unrelated cue");Pump(owner,{y});}
 Check(SDL_WasInit(SDL_INIT_AUDIO),"Frontend owner closed external audio reference");SDL_QuitSubSystem(SDL_INIT_AUDIO);Check(!SDL_WasInit(SDL_INIT_AUDIO),"Frontend concurrent lifecycle leaked audio");
}
void Owned(const char* folder)
{
 SDL_SetHint(SDL_HINT_AUDIO_DRIVER,"dummy");const auto root=std::filesystem::path(folder);auto catalog=ReadAudioBankCatalog(Read(root/"nlxgs.bun"));auto calc=ReadAudioCalculationInitial(Read(root/"nlxgs.bun"));auto bank=ReadAudioResidentBank(Read(root/"FE_GEN_Sfx.resbun"),Read(root/"FE_GEN_Sfx.nlxwb"));
 auto loaded=std::make_shared<const LoadedAudioBank>(LoadedAudioBank{23,21,catalog->names.at(23),catalog->slots.at(21),bank});FrontendAudio owner(loaded,calc,{16});unsigned seed=23;std::vector<FrontendAudioHandle> handles;
 const std::array keys{0x6b0689d4u,0x0a93e9a0u,0xf6eb899eu,0xf0afd586u,0x304fdd1eu,0x4430b152u};
 const std::array samples{1u,30u,49u,16u,130u,5u};
 for(unsigned i=0;i<keys.size();++i){auto h=*owner.Play(keys[i],seed);Check(owner.Status(h).sample==samples[i],"Owned Main/Options sample oracle differs");handles.push_back(h);}
 owner.Update(0);owner.ServiceAudio();Pump(owner,handles);for(auto key:keys)Check(owner.ActiveCount(key)==0,"Owned completed cue remained active");
 for(auto key:{0x1c4c829eu,0x71d9cd2fu,0x89b1fc93u,0xf9995a87u}){const auto before=seed;Reject([&]{owner.Play(key,seed);});Check(seed==before&&owner.ActiveCount(key)==0,"Unsupported owned profile committed state");}
 owner.Unload();Check(!SDL_WasInit(SDL_INIT_AUDIO),"Owned frontend audio retained SDL refs");std::cout<<"Owned Main/Options: six real simultaneous cue outputs completed and released\n";
}
}
int main(int argc,char**argv)
{
 try
 {
  if(argc==3&&std::string_view(argv[1])=="--fixture")
  {std::filesystem::path p=argv[2];std::filesystem::create_directories(p);auto f=Make();for(const auto&[name,bytes]:{std::pair{"FE_GEN_Sfx.resbun",f.bytes},{"FE_GEN_Sfx.nlxwb",f.wave},{"calculation.bun",Calculation()}}){std::ofstream out(p/name,std::ios::binary);out.write(reinterpret_cast<const char*>(bytes.data()),bytes.size());Check(bool(out),"Fixture write failed");}return 0;}
  for(unsigned i=0;i<3;++i){Lifecycle();LimitsAndRollback();}Concurrent();if(argc==2)Owned(argv[1]);else Check(argc==1,"Supply optional owned audio folder");
  std::cout<<checks<<" frontend cue checks passed\n";
 }
 catch(std::exception&e){allocation_budget=-1;std::cerr<<"FAILED: "<<e.what()<<" (check "<<checks<<")\n";return 1;}
}
