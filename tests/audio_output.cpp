#include "runtime/audio_output.h"
#include "audio_bank_fixture.h"
#include <SDL3/SDL.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <filesystem>
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
{++checks;try{f();}catch(const std::exception&){return;}throw std::runtime_error("Invalid audio output accepted at "+std::to_string(at.line()));}
Data Calculation(std::span<const float> values,std::span<const int> parents)
{
 constexpr unsigned base=0xf1000100;Data section,records(values.size()*24);
 Append(section,0x23401,Words({unsigned(values.size()),base,0x12345678}));
 for(unsigned i=0;i<values.size();++i){Put(records,i*24,i);Put(records,i*24+4,0xcafe0000+i);Put(records,i*24+8,Float(values[i]));if(parents[i]>=0)Put(records,i*24+12,base+parents[i]*24);}
 Append(section,0x23402,records);return Wrap(0x80000001,Wrap(0x80023400,section));
}
Data Calculation(){const float v[]={0,0};const int p[]={-1,0};return Calculation(v,p);}
AudioBankSelectionResult Selected(AudioResidentBank::Handle bank,unsigned seed=1)
{AudioBankSelection select(bank);return *select.Select({0x10203040,0,0,0},seed);}
void Calculations()
{
 const float values[]={2,-3,100,-100,0};const int parents[]={-1,0,3,1,2};
 const auto bytes=Calculation(values,parents);auto result=ReadAudioCalculationInitial(bytes);
 // Stored-order Update(0): forward reference sees initialized value0; backward
 // references see the target already reached, not the dependency's GetValue.
 const float expected[]={3,-1,6,-96,6};
 Check(result->Size()==5,"Calculation count differs");for(unsigned i=0;i<5;++i)Check(result->Value(i)==expected[i],"Initial calculation ordering/clamp differs");
 Reject([&]{result->Value(5);});Reject([&]{ReadAudioCalculationInitial({});});
 for(unsigned n=0;n<bytes.size();++n)Reject([&]{ReadAudioCalculationInitial(Bytes(bytes).first(n));});
 auto bad=bytes;Put(bad,24,4097);Reject([&]{ReadAudioCalculationInitial(bad);});
 bad=bytes;Put(bad,28,0xfffffff8);Reject([&]{ReadAudioCalculationInitial(bad);});
 bad=bytes;Put(bad,44,5);Reject([&]{ReadAudioCalculationInitial(bad);});
 bad=bytes;Put(bad,52,0x7fc00000);Reject([&]{ReadAudioCalculationInitial(bad);});
 bad=bytes;Put(bad,56,0xf1000101);Reject([&]{ReadAudioCalculationInitial(bad);});
 auto published=result;unsigned failures=0;bool success=false;
 for(long b=0;b<20&&!success;++b){allocation_budget=b;try{result=ReadAudioCalculationInitial(bytes);success=true;}catch(const std::bad_alloc&){++failures;}allocation_budget=-1;if(!success)Check(result==published,"Failed calculation publication changed owner");}
 Check(success&&failures>=5,"Calculation allocation failure sweep missed ownership");
}
void Buffers()
{
 auto f=Make();auto bank=ReadAudioResidentBank(f.bytes,f.wave);auto calculation=ReadAudioCalculationInitial(Calculation());
 auto selected=Selected(bank);auto output=PrepareResidentAudio(selected,calculation);const auto pcm=DecodeAudioDsp(bank,selected.events[0].sample);
 Check(output->Mix().volume_db==0&&output->Mix().input==32767&&output->Mix().left==23197&&output->Mix().right==23197&&output->Mix().pan==64,"Original default gain/pan table differs");
 Check(output->Stereo().size()==pcm->Samples().size()*2&&output->Rate()==pcm->Rate(),"Stereo rate/count differs");
 for(unsigned i=0;i<pcm->Samples().size();++i)
 {
  const double expected=double(pcm->Samples()[i])*32767.0*23197.0/35184372088832.0;
  Check(std::abs(double(output->Stereo()[i*2])-expected)<1e-7&&output->Stereo()[i*2]==output->Stereo()[i*2+1],"Independent source gain oracle differs");
 }
 for(const float value:{-96.f,-90.4f,-90.3f,-6.f,0.f,6.f,100.f})
 {
  auto changed=f;Put(changed.bytes,changed.voices+4,Float(value));auto chosen=Selected(ReadAudioResidentBank(changed.bytes,changed.wave));
  auto rendered=PrepareResidentAudio(chosen,calculation);const auto mix=rendered->Mix();const float clamped=std::clamp(value,-96.f,6.f);
  Check(mix.volume_db==clamped,"Original zero-duration voice transition clamp differs");
  const int tenth=int(10*clamped);const unsigned expected=tenth<=-904?0:tenth>=60?65380:unsigned(std::pow(10.0,double(tenth)/200)*32767.0);
  Check(mix.input==expected,"MIX discrete input table differs from independent gain oracle");
 }
 Reject([&]{PrepareResidentAudio({},calculation);});Reject([&]{PrepareResidentAudio(selected,{});});
 auto wrong=selected;wrong.voice=1;Reject([&]{PrepareResidentAudio(wrong,calculation);});
 wrong=selected;wrong.events[0].source=99;Reject([&]{PrepareResidentAudio(wrong,calculation);});
 wrong=selected;wrong.events[0].sample=99;Reject([&]{PrepareResidentAudio(wrong,calculation);});
 wrong=selected;wrong.events[0].start_time=1;Reject([&]{PrepareResidentAudio(wrong,calculation);});
 for(const auto [offset,value]:std::initializer_list<std::pair<std::size_t,unsigned>>{{f.sounds+4,2},{f.sounds+20,0x01000000},{f.sounds+20,0x00010000},{f.sounds+40,Float(1)},{f.voices+8,Float(1)},{f.voices+12,99}})
 {auto changed=f;Put(changed.bytes,offset,value);auto chosen=Selected(ReadAudioResidentBank(changed.bytes,changed.wave));Reject([&]{PrepareResidentAudio(chosen,calculation);});}
 auto published=output;unsigned failures=0;bool success=false;
 for(long b=0;b<12&&!success;++b){allocation_budget=b;try{output=PrepareResidentAudio(selected,calculation);success=true;}catch(const std::bad_alloc&){++failures;}allocation_budget=-1;if(!success)Check(output==published,"Failed audio buffer publication changed owner");}
 Check(success&&failures>=6,"Audio buffer allocation failure sweep missed ownership");
 std::weak_ptr<const AudioResidentBank> wb=bank;std::weak_ptr<const AudioCalculationInitial> wc=calculation;
 selected={};wrong={};bank.reset();calculation.reset();f.bytes.clear();f.wave.clear();
 Check(!wb.expired()&&!wc.expired()&&!output->Stereo().empty(),"Audio buffer did not retain checked sources");
 output.reset();published.reset();Check(wb.expired()&&wc.expired(),"Audio buffer retained released source owners");
}
ResidentAudioBuffer::Handle FixtureBuffer()
{auto f=Make();return PrepareResidentAudio(Selected(ReadAudioResidentBank(f.bytes,f.wave)),ReadAudioCalculationInitial(Calculation()));}
void Device()
{
 SDL_SetHint(SDL_HINT_AUDIO_DRIVER,"dummy");
 auto buffer=FixtureBuffer();const auto init=SDL_WasInit(SDL_INIT_AUDIO);
 Reject([&]{ResidentAudioOutput bad({});});Reject([&]{ResidentAudioOutput bad(buffer,0x01234567);});
 Check(SDL_WasInit(SDL_INIT_AUDIO)==init,"Failed device open leaked SDL subsystem reference");
 for(unsigned session=0;session<4;++session)
 {
  auto current=FixtureBuffer();std::weak_ptr<const ResidentAudioBuffer> weak=current;
  ResidentAudioOutput device(current);current.reset();Check(!weak.expired(),"Prepared stream lost its buffer");
  auto status=device.Status();Check(status.state==ResidentAudioState::Prepared&&status.queued_input_bytes>0,"SDL device did not begin paused with queued bytes");
  std::this_thread::sleep_for(std::chrono::milliseconds(25));Check(device.Status().queued_input_bytes==status.queued_input_bytes,"Paused SDL stream progressed");
  bool rejected=false;std::thread other([&]{try{device.Start();}catch(const std::logic_error&){rejected=true;}});other.join();Check(rejected,"Cross-thread Start accepted");
  if(session!=0)
  {
   device.Start();Reject([&]{device.Start();});const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(3);
   if(session==1){while(device.Status().state!=ResidentAudioState::InputConsumed){if(std::chrono::steady_clock::now()>deadline)throw std::runtime_error("SDL dummy did not consume source queue");std::this_thread::sleep_for(std::chrono::milliseconds(2));}Check(!weak.expired(),"Input-consumed stream prematurely released buffer");}
  }
  device.Stop();device.Stop();status=device.Status();Check(status.state==ResidentAudioState::Stopped&&!status.queued_input_bytes&&!status.available_output_bytes&&weak.expired(),"Stop did not destroy/discard/release stream");Reject([&]{device.Start();});
  Check(SDL_WasInit(SDL_INIT_AUDIO)==init,"Stopped output leaked SDL subsystem reference");
 }
 Check(SDL_InitSubSystem(SDL_INIT_AUDIO),"Test existing SDL audio reference failed");
 {ResidentAudioOutput first(buffer);{ResidentAudioOutput second(buffer);second.Start();}Check(SDL_WasInit(SDL_INIT_AUDIO)!=0,"One output destroyed another SDL audio owner");first.Start();}
 Check(SDL_WasInit(SDL_INIT_AUDIO)!=0,"Output discarded external SDL audio reference");SDL_QuitSubSystem(SDL_INIT_AUDIO);
 Check(SDL_WasInit(SDL_INIT_AUDIO)==init,"Final SDL subsystem reference did not recover");
}
Data Read(const char* path){std::ifstream f(path,std::ios::binary);if(!f)throw std::runtime_error("Cannot read supplied audio file");return {std::istreambuf_iterator<char>(f),{}};}
void DiskOutput()
{
 const auto directory=std::filesystem::temp_directory_path()/
  ("mscharged-audio-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
 Check(std::filesystem::create_directory(directory),"Cannot create isolated SDL output directory");
 struct Cleanup{std::filesystem::path path;~Cleanup(){std::error_code e;std::filesystem::remove_all(path,e);}} cleanup{directory};
 const auto path=directory/"stereo.raw";
 SDL_SetHint(SDL_HINT_AUDIO_DRIVER,"disk");SDL_SetHint(SDL_HINT_AUDIO_DISK_OUTPUT_FILE,path.string().c_str());
 SDL_SetHint(SDL_HINT_AUDIO_FORMAT,"F32");SDL_SetHint(SDL_HINT_AUDIO_FREQUENCY,"44100");SDL_SetHint(SDL_HINT_AUDIO_CHANNELS,"2");
 auto fixture=Make();for(unsigned i=0;i<5;++i)Put(fixture.bytes,fixture.sp+8+i*28,44100);
 const auto buffer=PrepareResidentAudio(Selected(ReadAudioResidentBank(fixture.bytes,fixture.wave)),ReadAudioCalculationInitial(Calculation()));
 ResidentAudioOutput output(buffer);output.Start();
 const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(3);
 while(output.Status().state!=ResidentAudioState::InputConsumed)
 {if(std::chrono::steady_clock::now()>deadline)throw std::runtime_error("SDL disk device did not consume queue");std::this_thread::sleep_for(std::chrono::milliseconds(2));}
 // Stream consumption precedes backend submission; let the test backend write
 // its current output block, then close synchronously before reading its file.
 std::this_thread::sleep_for(std::chrono::milliseconds(60));output.Stop();
 const auto bytes=Read(path.string().c_str());const auto samples=buffer->Stereo();
 const auto* first=reinterpret_cast<const std::uint8_t*>(samples.data());
 Check(std::any_of(samples.begin(),samples.end(),[](float v){return v!=0;}),"Output fixture has no signal");
 Check(std::search(bytes.begin(),bytes.end(),first,first+samples.size_bytes())!=bytes.end(),
  "SDL backend output does not contain original-gain stereo samples");
 SDL_ResetHint(SDL_HINT_AUDIO_DISK_OUTPUT_FILE);SDL_ResetHint(SDL_HINT_AUDIO_FORMAT);SDL_ResetHint(SDL_HINT_AUDIO_FREQUENCY);SDL_ResetHint(SDL_HINT_AUDIO_CHANNELS);SDL_ResetHint(SDL_HINT_AUDIO_DRIVER);
}
void Owned(const char* global,const char* metadata,const char* wave,bool audible)
{
 auto calculation=ReadAudioCalculationInitial(Read(global));Check(calculation->Size()==5&&calculation->Value(0)==1&&calculation->Value(1)==0,"Owned initial calculation values differ");
 auto bank=ReadAudioResidentBank(Read(metadata),Read(wave));AudioBankSelection select(bank);unsigned seed=1;std::array<bool,5> seen{};
 for(unsigned i=0;i<128;++i)
 {
  auto result=*select.Select({0xde83984e,0,0,0},seed);auto output=PrepareResidentAudio(result,calculation);seen.at(output->Sample())=true;
  Check(output->Rate()==32000&&output->Mix().volume_db==0&&output->Mix().input==32767&&output->Mix().left==23197&&output->Mix().right==23197,"Owned logo mix differs");
  if(audible&&i==0)
  {
   SDL_ResetHint(SDL_HINT_AUDIO_DRIVER);ResidentAudioOutput device(output);device.Start();auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(8);
   std::cout<<"SDL driver "<<SDL_GetCurrentAudioDriver()<<", logo sample "<<output->Sample()<<"\n";
   while(device.Status().state!=ResidentAudioState::InputConsumed){if(std::chrono::steady_clock::now()>deadline)throw std::runtime_error("Real SDL device did not consume logo");std::this_thread::sleep_for(std::chrono::milliseconds(5));}
   std::cout<<"Real SDL logo source consumed; physical completion is not asserted.\n";std::this_thread::sleep_for(std::chrono::milliseconds(200));device.Stop();
  }
 }
 Check(seen[0]&&seen[1]&&!seen[2]&&seen[3]&&seen[4],"Owned logo did not exercise all authored random choices");
 auto second=*select.Select({0xf394c076,0,0,0},seed);auto output=PrepareResidentAudio(second,calculation);Check(output->Rate()==44100&&output->Sample()==2,"Owned second cue metadata differs");
 std::cout<<"Owned: all four logo choices prepared at32000Hz; sample2 prepared at44100Hz.\n";
}
}
int main(int argc,char** argv)
{
 try{if(argc==5&&std::string(argv[4])!="--audible")throw std::runtime_error("Unknown audio output option");Calculations();Buffers();Device();DiskOutput();if(argc==4||argc==5)Owned(argv[1],argv[2],argv[3],argc==5);else if(argc!=1)throw std::runtime_error("Usage: audio_output_tests [GLOBAL RESBUN WAVE [--audible]]");std::cout<<checks<<" audio output checks passed\n";return 0;}
 catch(const std::exception& e){allocation_budget=-1;std::cerr<<e.what()<<" (check"<<checks<<")\n";return 1;}
}
