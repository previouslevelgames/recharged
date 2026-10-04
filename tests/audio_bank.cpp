#include "resources/audio_bank.h"
#include "audio_bank_fixture.h"
#include "runtime/audio_bank_selection.h"
#include <algorithm>
#include <bit>
#include <fstream>
#include <iostream>
#include <source_location>
#include <thread>
#include <cstdlib>
#include <new>
// Fail only this test thread's allocations inside transactional operations.
thread_local long allocation_budget=-1;
void* operator new(std::size_t size)
{
 if(allocation_budget==0)throw std::bad_alloc();if(allocation_budget>0)--allocation_budget;
 if(auto* p=std::malloc(size?size:1))return p;throw std::bad_alloc();
}
void* operator new[](std::size_t size){return ::operator new(size);}
void operator delete(void* p)noexcept{std::free(p);}
void operator delete[](void* p)noexcept{std::free(p);}
void operator delete(void* p,std::size_t)noexcept{std::free(p);}
void operator delete[](void* p,std::size_t)noexcept{std::free(p);}
using namespace mscharged;
using namespace mscharged::resources;
namespace
{
using namespace audio_bank_fixture;
unsigned checks=0;
void Check(bool v,const char* why){++checks;if(!v)throw std::runtime_error(why);}
template<class F>void Reject(F f,std::source_location at=std::source_location::current())
{++checks;try{f();}catch(const std::exception&){return;}throw std::runtime_error("Invalid bank accepted at "+std::to_string(at.line()));}
unsigned Random(unsigned range,unsigned& seed)
{if(!range)return 0;auto result=seed%range;auto a=seed^0x1d872b41u;auto b=a^(a>>5);seed=b^(a^(b<<27));return result;}
unsigned Choice(unsigned value,const std::vector<AudioBankChoice>& entries)
{
 // Independent interval oracle from the high end: first authored entry owns
 // [sum-following,sum-total), rather than usual low-to-high cumulative weights.
 unsigned below=0;for(auto it=entries.rbegin();it!=entries.rend();++it){if(value<below+it->weight)return it->index;below+=it->weight;}throw std::logic_error("oracle range");
}
void Generated()
{
 auto f=Make();auto bank=ReadAudioResidentBank(f.bytes,f.wave);Check(bank->Cues().size()==2&&bank->Voices().size()==2&&bank->Sounds().size()==2&&bank->Samples().size()==5,"Synthetic graph counts differ");
 Check(bank->Cues()[1].voices[0].voice==0&&bank->Cues()[0].voices[0].voice==1,"Serialized voice order lost");
 Check(bank->Sounds()[1].random_pitch&&bank->Sounds()[1].random_volume,"Sound flags were packed into padding");
 for(unsigned i=0;i<5;++i){const auto& s=bank->Samples()[i];Check(s.sample_count==28&&s.byte_count==16&&s.first_byte==i*16&&s.coefficients[0]==-1&&s.history1==-2&&s.history2==3,"SP nibble/sample/coefficient decoding differs");Check(bank->SampleBytes(i).front()==i*16,"Retained sample base differs");}
 const auto choices=bank->Sounds()[0].choices;f.bytes.clear();f.wave.clear();AudioBankSelection select(bank);bank.reset();unsigned seed=0x12345678,oracle=seed;
 for(unsigned i=0;i<100;++i)
 {const auto expected=Choice(Random(10,oracle),choices);auto got=select.Select({0x10203040,0,0,0},seed);Check(got&&got->cue==1&&got->voice==0&&got->events.size()==1&&got->events[0].source==expected&&seed==oracle,"Original source choice or RNG consumption differs");}
 const auto state=select.State(1);Check(state.selected==0&&state.counts==std::vector<unsigned>{100},"Original single-voice counter changed");
 auto before=seed;auto single=select.Select({0x99887766,0,0,0},seed);Check(single->events[0].sample==2&&seed==before,"Single choice consumed RNG for playback-only pitch/volume");
 Check(!select.Select({0x10203040,0,0,1},seed)&&seed==before,"Missing four-part key consumed RNG");
 auto kept=*single;select.Reset();Check(select.State(1).selected==0xffff&&select.State(1).counts[0]==0&&kept.bank->SampleBytes(2).size()==16,"Reset lost retained selected sample");
 bool rejected=false;std::thread other([&]{try{select.Select({0x10203040,0,0,0},seed);}catch(const std::logic_error&){rejected=true;}});other.join();Check(rejected&&seed==before,"Foreign selection mutated shared seed");
 for(unsigned mode=0;mode<=4;++mode)
 {
  auto fixture=Make(2,mode);auto b=ReadAudioResidentBank(fixture.bytes,fixture.wave);AudioBankSelection s(b);unsigned actual_seed=23,expected_seed=23,previous=0xffff;std::array<unsigned,2> counts{};
  for(unsigned n=0;n<100;++n)
  {
   unsigned chosen;
   if(mode==0||(mode==1&&previous!=0xffff))chosen=previous==0xffff?0:1-previous;
   else if(mode==3){const auto r=Random(previous==0xffff?5:previous==0?3:2,expected_seed);chosen=previous==0xffff?(r<=2?0:1):1-previous;}
   else
   {
    std::array<bool,2> eligible{true,true};if(mode==4&&previous!=0xffff&&counts[0]!=counts[1])eligible={counts[0]<counts[previous],counts[1]<counts[previous]};
    const unsigned total=(eligible[0]?2:0)+(eligible[1]?3:0);const auto r=Random(total,expected_seed);chosen=eligible[0]&&(!eligible[1]||r<=2)?0:1;
   }
   unsigned source=2;if(chosen==0)source=Choice(Random(10,expected_seed),b->Sounds()[0].choices);
   auto result=s.Select({0x10203040,0,0,0},actual_seed);Check(result->voice==chosen&&result->events[0].source==source&&actual_seed==expected_seed,"Independent cue mode/order/RNG oracle differs");++counts[chosen];previous=chosen;
   Check(s.State(1).counts==std::vector<unsigned>(counts.begin(),counts.end()),"Cue selection counters changed");
  }
 }
 auto delayed=Make(1,3,true,true);auto b=ReadAudioResidentBank(delayed.bytes,delayed.wave);AudioBankSelection delayed_select(b);seed=17;oracle=seed;const float time=1+(1.f/2147483647.f)*2.f*float(Random(0x7fffffff,oracle));const auto expected=Choice(Random(10,oracle),b->Sounds()[0].choices);
 auto selected=delayed_select.Select({0x10203040,0,0,0},seed);Check(selected->events.size()==2&&selected->events[0].event==0&&selected->events[1].event==1&&selected->events[0].source==expected&&std::bit_cast<unsigned>(selected->events[0].start_time)==std::bit_cast<unsigned>(time)&&seed==oracle,"Constructor delay/ordered events changed RNG");
 f=Make();const auto stable=ReadAudioResidentBank(f.bytes,f.wave);auto published=stable;
 for(unsigned size=0;size<f.bytes.size();++size)Reject([&]{published=ReadAudioResidentBank(Bytes(f.bytes).first(size),f.wave);});Check(published==stable,"Failed graph replacement exposed partial data");
 for(unsigned size=0;size<f.wave.size();++size)Reject([&]{ReadAudioResidentBank(f.bytes,Bytes(f.wave).first(size));});
 for(auto [off,value]:std::initializer_list<std::pair<std::size_t,unsigned>>{{f.header+16,4097},{f.header+20,0xfffffff0},{f.header+28,1},{f.header+40,1},{f.cues+12,0x01000000},{f.cues+16,5},{f.cues+24,1},{f.refs,0xf0001001},{f.refs,0xefffffff},{f.refs+8,0},{f.refs+8,Float(.5f)},{f.refs+8,0x7fc00000},{f.seqrefs,0xe0002018},{f.eventrefs,2},{f.eventrefs+4,0xd0003001},{f.choices,5},{f.choices+4,0},{f.choices+4,0xffffffff},{f.sp,4097},{f.sp+4,1},{f.sp+8,0},{f.sp+20,160},{f.sp+24,0},{f.sp+24,34},{f.sources,5},{f.sources-12,0x01000000},{f.map+16,2},{f.voices+32,1},{f.sounds+40,Float(-1)},{f.sounds+28,0x7f800000},{f.eventrefs-8,0x2330b}})
 {auto bad=f.bytes;Put(bad,off,value);Reject([&]{ReadAudioResidentBank(bad,f.wave);});}
 // Real host allocation failures before/after source math never publish a
 // partial owner, selection count or shared RNG state.
 unsigned failures=0;bool succeeded=false;
 for(long budget=0;budget<512&&!succeeded;++budget)
 {
  bool failed=false;allocation_budget=budget;
  try{published=ReadAudioResidentBank(f.bytes,f.wave);}catch(const std::bad_alloc&){failed=true;}
  allocation_budget=-1;
  if(failed){++failures;Check(published==stable,"Allocation failure replaced the prior audio bank");}
  else succeeded=true;
 }
 Check(succeeded&&failures>20,"Audio graph allocation sweep missed retained ownership stages");
 failures=0;succeeded=false;AudioBankSelection transaction(stable);
 for(long budget=0;budget<32&&!succeeded;++budget)
 {
  unsigned rng=0xabcdef12;const auto old=transaction.State(1);bool failed=false;allocation_budget=budget;
  try{(void)transaction.Select({0x10203040,0,0,0},rng);}catch(const std::bad_alloc&){failed=true;}
  allocation_budget=-1;
  if(failed){++failures;Check(rng==0xabcdef12&&transaction.State(1).selected==old.selected&&transaction.State(1).counts==old.counts,"Failed selection committed RNG/counter prefix");}
  else succeeded=true;
 }
 Check(succeeded&&failures>=4,"Selection allocation sweep missed post-math publication");
 // Both low and high inclusive nibble endpoints contain a final data sample.
 for(unsigned end:{30u,31u}){auto bytes=f.bytes;Put(bytes,f.sp+20,end);auto small=ReadAudioResidentBank(bytes,f.wave);Check(small->Samples()[0].sample_count==end-3&&small->SampleBytes(0).size()==16,"Inclusive ADPCM final nibble changed");}
}
Data Read(const char* path){std::ifstream f(path,std::ios::binary);if(!f)throw std::runtime_error("Cannot read audio fixture");return {std::istreambuf_iterator<char>(f),{}};}
void Owned(const char* metadata,const char* wave)
{
 auto b=ReadAudioResidentBank(Read(metadata),Read(wave));Check(b->WaveBytes()==303445&&b->Samples().size()==5,"Owned resident bank inventory differs");
 constexpr unsigned first[]{2,100850,209842,356722,484418},last[]{100844,209829,356711,484412,606889};
 for(unsigned i=0;i<5;++i){const auto&s=b->Samples()[i];Check(s.current_nibble==first[i]&&s.end_nibble==last[i]&&s.rate==(i==2?44100:32000),"Owned SP metadata differs");Check(s.first_byte==first[i]/16*8&&s.byte_count==last[i]/2+1-s.first_byte,"Owned inclusive retained sample extent differs");std::cout<<"SP "<<i<<" rate "<<s.rate<<" nibbles "<<first[i]<<".."<<last[i]<<" samples "<<s.sample_count<<" bytes "<<s.byte_count<<'\n';}
 Check(!b->Sounds()[1].random_pitch&&!b->Sounds()[1].random_volume,"Owned source padding became playback flags");
 const std::vector<AudioBankChoice> choices{{3,255},{4,255},{0,255},{1,255}};AudioBankSelection select(b);unsigned seed=0x12345678,oracle=seed;std::array<bool,5> seen{};
 for(unsigned i=0;i<1024;++i){const auto expected=Choice(Random(1020,oracle),choices);auto s=select.Select({0xde83984e,0,0,0},seed);Check(s->cue==1&&s->voice==0&&s->events[0].source==expected&&seed==oracle,"Owned logo selection/RNG differs");seen[expected]=true;}
 Check(seen[0]&&seen[1]&&seen[3]&&seen[4]&&!seen[2],"Owned logo did not retain its exact four authored choices");auto before=seed;auto other=select.Select({0xf394c076,0,0,0},seed);Check(other->cue==0&&other->voice==1&&other->events[0].sample==2&&seed==before,"Owned other cue consumed wrong RNG/sample");
}
}
int main(int argc,char**argv)
{try{Check(argc==1||argc==3,"Supply optional resbun and nlxwb");Generated();if(argc==3)Owned(argv[1],argv[2]);std::cout<<checks<<" audio bank graph/selection checks passed\n";}catch(const std::exception&e){std::cerr<<"FAILED: "<<e.what()<<" (check "<<checks<<")\n";return 1;}}
