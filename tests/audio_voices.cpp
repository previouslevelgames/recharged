#include "runtime/audio_voices.h"
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

thread_local long allocation_budget=-1;
void* operator new(std::size_t n){if(allocation_budget==0)throw std::bad_alloc();if(allocation_budget>0)--allocation_budget;if(auto p=std::malloc(n?n:1))return p;throw std::bad_alloc();}
void* operator new[](std::size_t n){return ::operator new(n);}
void operator delete(void* p)noexcept{std::free(p);}void operator delete[](void* p)noexcept{std::free(p);}
void operator delete(void* p,std::size_t)noexcept{std::free(p);}void operator delete[](void* p,std::size_t)noexcept{std::free(p);}
using namespace mscharged;using namespace mscharged::resources;using namespace audio_bank_fixture;
namespace
{
unsigned checks=0;
void Check(bool good,const char* message){++checks;if(!good)throw std::runtime_error(message);}
template<class F>void Reject(F f,std::source_location at=std::source_location::current())
{++checks;try{f();}catch(const std::exception&){return;}throw std::runtime_error("Invalid voice operation accepted at "+std::to_string(at.line()));}
Data Calculation()
{Data section;Append(section,0x23401,Words({2,0xf1000100,0}));Append(section,0x23402,Words({0,0,0,0,0,0,1,0,0,0xf1000100,0,0}));return Wrap(0x80000001,Wrap(0x80023400,section));}
AudioCalculationInitial::Handle Calc(){return ReadAudioCalculationInitial(Calculation());}
AudioResidentBank::Handle Bank(){auto f=Make();return ReadAudioResidentBank(f.bytes,f.wave);}
AudioBankSelectionResult Select(AudioResidentBank::Handle bank,unsigned& seed)
{AudioBankSelection selector(bank);return *selector.Select({0x10203040,0,0,0},seed);}
AudioBankSelectionResult Select(AudioResidentBank::Handle bank)
{unsigned seed=17;return Select(std::move(bank),seed);}
void State(const AudioVoices& owner,AudioVoiceHandle handle,int exposed,int internal,bool voice=true)
{const auto s=owner.Status(handle);Check(s.state==exposed&&s.internal_state==internal&&s.has_voice==voice&&!s.failed,"Original source state/voice ownership differs");}
void Transitions()
{
 SDL_SetHint(SDL_HINT_AUDIO_DRIVER,"dummy");auto selected=Select(Bank());auto calc=Calc();
 AudioVoices voices;const auto h=voices.Create(selected,calc);State(voices,h,1,1);Check(voices.LastCreated()==h&&voices.Sources()==std::vector{h},"Create did not initialize then publish in order");
 voices.Stop(h);State(voices,h,1,1);Check(!voices.Prepare(h),"Original Prepare did not return false");State(voices,h,1,3);
 Check(voices.PollState(h)==3,"Original prepared public state differs");State(voices,h,3,3);
 Check(voices.Play(h),"Original Play admission differs");State(voices,h,3,5);voices.Stop(h);State(voices,h,3,5);
 Check(voices.PollState(h)==3,"Pending5 exposed state was not3");voices.ServiceAudio();State(voices,h,3,4);
 Check(voices.PollState(h)==4,"Running source was not exposed4");Reject([&]{voices.Play(h);});Reject([&]{voices.Prepare(h);});
 voices.Stop(h);State(voices,h,4,4);voices.ServiceAudio();State(voices,h,4,6);
 Check(voices.PollState(h)==6,"Stopped source did not expose terminal6");State(voices,h,6,6,false);
 voices.ServiceAudio();State(voices,h,6,1,false);Check(voices.PollState(h)==1,"Original terminal source did not become idle1");
 Reject([&]{voices.Play(h);});voices.Destroy(h);Check(voices.Sources().empty()&&voices.LastCreated()==h,"Destroy changed original last-created identity");Reject([&]{voices.Status(h);});
 // Stop prepared changes BOTH states immediately, but releases only when the
 // game-side state poll sees6. If service runs first it can miss that state.
 for(bool poll_first:{false,true})
 {
  const auto v=voices.Create(selected,calc);voices.Prepare(v);voices.Stop(v);State(voices,v,6,6);
  if(poll_first){Check(voices.PollState(v)==6,"Prepared stop poll differs");State(voices,v,6,6,false);}
  voices.ServiceAudio();Check(voices.PollState(v)==1,"Prepared stop next service differs");State(voices,v,1,1,!poll_first);voices.Destroy(v);
 }
 const auto pending=voices.Create(selected,calc);voices.Play(pending);voices.Stop(pending);State(voices,pending,1,5);
 voices.Destroy(pending);Check(voices.Sources().empty(),"Explicit Destroy did not cancel pending5");
 voices.Release();voices.Release();Reject([&]{voices.Create(selected,calc);});Reject([&]{voices.Sources();});
 Check(!SDL_WasInit(SDL_INIT_AUDIO),"Source teardown retained SDL audio reference");
}
void HandlesAndBudgets()
{
 SDL_SetHint(SDL_HINT_AUDIO_DRIVER,"dummy");auto selected=Select(Bank());auto calc=Calc();
 Reject([&]{AudioVoices bad({0});});Reject([&]{AudioVoices bad({65});});Reject([&]{AudioVoices bad({1,0});});
 AudioVoices one({1});const auto first=one.Create(selected,calc);Reject([&]{one.Create(selected,calc);});
 Check(one.Sources()==std::vector{first}&&one.LastCreated()==first,"Capacity failure changed publication");
 one.Destroy(first);const auto second=one.Create(selected,calc);Check(first.slot==second.slot&&second.generation==first.generation+1,"Slot reuse did not advance generation");
 Reject([&]{one.Prepare(first);});Reject([&]{one.Destroy(first);});
 AudioVoices other({2});Reject([&]{other.Status(second);});Reject([&]{one.Status({});});
 bool rejected=false;std::thread thread([&]{try{one.ServiceAudio();}catch(const std::exception&){rejected=true;}});thread.join();Check(rejected,"Cross-thread voice service accepted");
 const auto bytes=one.Status(second).input_bytes;
 AudioVoices limited({2,bytes});const auto limited_h=limited.Create(selected,calc);Reject([&]{limited.Create(selected,calc);});
 limited.Destroy(limited_h);const auto recreated=limited.Create(selected,calc);Check(limited.Status(recreated).input_bytes==bytes,"Destroyed input budget was not recovered");limited.Release();
 const auto a=other.Create(selected,calc),b=other.Create(selected,calc);other.Destroy(a);const auto c=other.Create(selected,calc);
 Check(other.Sources()==std::vector{b,c},"Reused slot did not append in source order");
 Reject([&]{other.Play(c,0);});Reject([&]{other.Play(c,2);});Reject([&]{other.Play(c,0xffff);});State(other,c,1,1);
 one.Release();other.Release();Check(!SDL_WasInit(SDL_INIT_AUDIO),"Generation/budget test leaked output owner");
}
void OwnershipAndFailure()
{
 SDL_SetHint(SDL_HINT_AUDIO_DRIVER,"dummy");auto bank=Bank();auto calc=Calc();unsigned seed=0xabcdef12;
 const unsigned before=seed,random=before%10;const unsigned expected=random>=9?3:random>=7?4:random>=4?0:1;
 auto selected=Select(bank,seed);const unsigned a=before^0x1d872b41U,b=a^(a>>5),next=b^a^(b<<27);
 Check(seed==next&&selected.events[0].sample==expected,"Independent original selection/RNG oracle differs");
 AudioVoices owner({2});const auto existing=owner.Create(selected,calc);Check(seed==next,"Create consumed caller RNG");
 unsigned failures=0;bool success=false;
 for(long budget=0;budget<80&&!success;++budget)
 {
  const auto last=owner.LastCreated();allocation_budget=budget;bool failed=false;AudioVoiceHandle result{};
  try{result=owner.Create(selected,calc);}catch(const std::bad_alloc&){failed=true;}allocation_budget=-1;
  if(failed){++failures;Check(owner.LastCreated()==last&&owner.Sources()==std::vector{existing},"Failed source init published a partial slot");State(owner,existing,1,1);}
  else{success=true;owner.Destroy(result);}
 }
 Check(success&&failures>=8,"Creation allocation sweep missed prepared output ownership");
 AudioVoices bad_device({1,64*1024*1024,0x1234567});Reject([&]{bad_device.Create(selected,calc);});Check(bad_device.Sources().empty()&&bad_device.LastCreated()==AudioVoiceHandle{},"Failed SDL device published a source");
 auto invalid=selected;invalid.events[0].sample=UINT32_MAX;Reject([&]{owner.Create(invalid,calc);});
 invalid=selected;invalid.events[0].source=UINT32_MAX;Reject([&]{owner.Create(invalid,calc);});
 auto f=Make();Put(f.bytes,f.sounds+4,2);auto repeat=Select(ReadAudioResidentBank(f.bytes,f.wave));Reject([&]{owner.Create(repeat,calc);});
 f=Make();Put(f.bytes,f.voices+8,Float(1));auto pitched=Select(ReadAudioResidentBank(f.bytes,f.wave));Reject([&]{owner.Create(pitched,calc);});
 Check(owner.Sources()==std::vector{existing}&&seed==next,"Invalid create changed unrelated sources/RNG");
 std::weak_ptr<const AudioResidentBank> weak_bank=bank;std::weak_ptr<const AudioCalculationInitial> weak_calc=calc;
 invalid={};selected={};bank.reset();calc.reset();Check(!weak_bank.expired()&&!weak_calc.expired(),"Prepared source lost retained resources");
 owner.Prepare(existing);owner.Stop(existing);owner.PollState(existing);State(owner,existing,6,6,false);
 Check(!weak_bank.expired()&&!weak_calc.expired(),"Voice release prematurely destroyed source metadata");
 owner.Destroy(existing);Check(weak_bank.expired()&&weak_calc.expired(),"Source destroy retained bank/calculation/PCM");owner.Release();bad_device.Release();
 Check(!SDL_WasInit(SDL_INIT_AUDIO),"Failed admission/ownership retained SDL reference");
}
Fixture LongFixture(unsigned blocks)
{
 auto f=Make();f.wave.assign(5*blocks*8,0);
 for(unsigned sample=0;sample<5;++sample)
 {
  const unsigned base=sample*blocks*16;auto record=f.sp+4+sample*28;
  Put(f.bytes,record+4,44100);Put(f.bytes,record+16,base+blocks*16-1);Put(f.bytes,record+20,base+2);
  std::fill_n(f.bytes.begin()+f.sp+4+5*28+sample*46,46,0);
  for(unsigned block=0;block<blocks;++block)
   std::fill_n(f.wave.begin()+sample*blocks*8+block*8+1,7,std::uint8_t((sample+1)*17));
 }
 return f;
}
AudioBankSelectionResult Specific(AudioResidentBank::Handle bank,unsigned sample)
{
 unsigned seed=1;AudioBankSelection selector(bank);
 for(unsigned i=0;i<128;++i){auto selected=*selector.Select({0x10203040,0,0,0},seed);if(selected.events[0].sample==sample)return selected;}
 throw std::runtime_error("Original source choice did not visit test sample");
}
void Consume(AudioVoices& voices,std::span<const AudioVoiceHandle> handles)
{
 const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
 std::vector<bool> done(handles.size());
 for(;;)
 {
  voices.ServiceAudio();
  for(unsigned i=0;i<handles.size();++i)if(!done[i])
  {
   const auto state=voices.PollState(handles[i]);
   if(state==6){Check(!voices.Status(handles[i]).has_voice,"Source6 did not release output");done[i]=true;}
  }
  if(std::all_of(done.begin(),done.end(),[](bool v){return v;}))break;
  if(std::chrono::steady_clock::now()>deadline)throw std::runtime_error("Real audio did not consume concurrent source queues");
  std::this_thread::sleep_for(std::chrono::milliseconds(2));
 }
 voices.ServiceAudio();for(auto h:handles)Check(voices.PollState(h)==1,"Consumed source did not finish original6→1 transition");
}
Data Read(const std::filesystem::path& path){std::ifstream f(path,std::ios::binary);if(!f)throw std::runtime_error("Cannot read "+path.string());return {std::istreambuf_iterator<char>(f),{}};}
void ConcurrentCancel()
{
 SDL_SetHint(SDL_HINT_AUDIO_DRIVER,"dummy");auto f=LongFixture(1024);
 auto bank=ReadAudioResidentBank(f.bytes,f.wave);auto calc=Calc();
 const auto selected=Specific(bank,0);
 {
  AudioVoices voices({2});const auto first=voices.Create(selected,calc),second=voices.Create(selected,calc);
  voices.Play(first);voices.Play(second);voices.ServiceAudio();
  Check(voices.PollState(first)==4&&voices.PollState(second)==4,"Cancellation fixture did not start both voices");
  voices.Destroy(first);Check(voices.Sources()==std::vector{second},"Destroy removed an unrelated running source");
  Check(SDL_WasInit(SDL_INIT_AUDIO)!=0,"Destroy closed another source's SDL lifetime");
  const std::array survivor{second};Consume(voices,survivor);voices.Destroy(second);
 }
 Check(!SDL_WasInit(SDL_INIT_AUDIO),"Surviving source teardown leaked audio device");
 Check(SDL_InitSubSystem(SDL_INIT_AUDIO),"Cannot hold external SDL lifetime");
 {
  AudioVoices voices({2});const auto first=voices.Create(selected,calc),second=voices.Create(selected,calc);
  voices.Play(first);voices.Play(second);voices.ServiceAudio();
  Check(voices.Status(first).internal_state==4&&voices.Status(second).internal_state==4,"Destructor fixture is not active");
 }
 Check(SDL_WasInit(SDL_INIT_AUDIO)!=0,"Owner destruction removed external SDL lifetime");
 SDL_QuitSubSystem(SDL_INIT_AUDIO);Check(!SDL_WasInit(SDL_INIT_AUDIO),"Active owner destruction did not drain output references");
}
void ConcurrentDisk()
{
 auto directory=std::filesystem::temp_directory_path()/("mscharged-voices-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
 Check(std::filesystem::create_directory(directory),"Cannot create concurrent output directory");
 struct Cleanup{std::filesystem::path path;~Cleanup(){std::error_code e;std::filesystem::remove_all(path,e);}}cleanup{directory};const auto path=directory/"mix.raw";
 SDL_SetHint(SDL_HINT_AUDIO_DRIVER,"disk");SDL_SetHint(SDL_HINT_AUDIO_DISK_OUTPUT_FILE,path.string().c_str());SDL_SetHint(SDL_HINT_AUDIO_FORMAT,"F32");SDL_SetHint(SDL_HINT_AUDIO_FREQUENCY,"44100");SDL_SetHint(SDL_HINT_AUDIO_CHANNELS,"2");
 auto f=LongFixture(2048);auto bank=ReadAudioResidentBank(f.bytes,f.wave);auto calc=Calc();AudioVoices voices({2});
 const auto first=voices.Create(Specific(bank,0),calc),second=voices.Create(Specific(bank,1),calc);
 const std::array handles{first,second};for(auto h:handles){Check(!voices.Prepare(h),"Concurrent source Prepare changed return");voices.PollState(h);voices.Play(h);}
 voices.ServiceAudio();for(auto h:handles)Check(voices.PollState(h)==4,"Concurrent outputs did not start");Consume(voices,handles);
 std::this_thread::sleep_for(std::chrono::milliseconds(60));voices.Release();
 const auto bytes=Read(path);Check(bytes.size()%8==0,"SDL disk output is not stereoF32");
 // Both independently encoded coefficient-zero DSP samples are constant1 and2.
 // The original default gains apply separately, then SDL sums float streams.
 const float gain=32767.0f/32768.0f*(23197.0f/32768.0f);
 const float expected=(1.0f/32768.0f*gain)+(2.0f/32768.0f*gain);
 unsigned matching=0;
 for(std::size_t at=0;at<bytes.size();at+=8)
 {
  float left,right;std::memcpy(&left,bytes.data()+at,4);std::memcpy(&right,bytes.data()+at+4,4);
  Check(std::isfinite(left)&&std::isfinite(right),"Concurrent output produced nonfinite samples");
  if(std::abs(left-expected)<1e-10f&&std::abs(right-expected)<1e-10f)++matching;
 }
 Check(matching>=1000,"Real SDL output did not mix both independently expected sources");
 std::cout<<"Concurrent disk output: "<<matching<<" stereo frames match independent sum\n";
 SDL_ResetHint(SDL_HINT_AUDIO_DISK_OUTPUT_FILE);SDL_ResetHint(SDL_HINT_AUDIO_FORMAT);SDL_ResetHint(SDL_HINT_AUDIO_FREQUENCY);SDL_ResetHint(SDL_HINT_AUDIO_CHANNELS);SDL_ResetHint(SDL_HINT_AUDIO_DRIVER);
 Check(!SDL_WasInit(SDL_INIT_AUDIO),"Concurrent device shutdown retained SDL reference");
}
void Owned(const char* global,const char* resbun,const char* wave)
{
 SDL_SetHint(SDL_HINT_AUDIO_DRIVER,"dummy");auto bank=ReadAudioResidentBank(Read(resbun),Read(wave));auto calc=ReadAudioCalculationInitial(Read(global));AudioBankSelection select(bank);unsigned seed=23;
 AudioVoices voices({4});std::array<bool,5> seen{};std::vector<AudioVoiceHandle> handles;
 for(unsigned n=0;n<128&&handles.size()<4;++n)
 {
  auto result=*select.Select({0xde83984e,0,0,0},seed);const auto sample=result.events[0].sample;
  if(seen[sample])continue;seen[sample]=true;const auto h=voices.Create(result,calc);voices.Prepare(h);voices.PollState(h);voices.Play(h);handles.push_back(h);
 }
 Check(handles.size()==4&&seen[0]&&seen[1]&&!seen[2]&&seen[3]&&seen[4],"Owned logo variants are incomplete");
 const auto selected_seed=seed;voices.ServiceAudio();for(auto h:handles)Check(voices.PollState(h)==4,"Owned concurrent logo did not start");
 for(auto h:handles)voices.Stop(h);Consume(voices,handles);voices.Release();Check(seed==selected_seed,"Owned voice lifecycle consumed selection RNG");
 Check(!SDL_WasInit(SDL_INIT_AUDIO),"Owned sources retained audio device refs");std::cout<<"Owned logo: four actual concurrent sources admitted/stopped/released\n";
}
}
int main(int argc,char** argv)
{
 try
 {
  for(unsigned session=0;session<3;++session){Transitions();HandlesAndBudgets();OwnershipAndFailure();}
  ConcurrentCancel();ConcurrentDisk();if(argc==4)Owned(argv[1],argv[2],argv[3]);else Check(argc==1,"Supply optional nlxgs/resbun/nlxwb");
  std::cout<<checks<<" resident voice checks passed\n";
 }
 catch(const std::exception& e){allocation_budget=-1;std::cerr<<"FAILED: "<<e.what()<<" (check "<<checks<<")\n";return 1;}
}
