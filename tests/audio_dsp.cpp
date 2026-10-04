#include "resources/audio_dsp.h"
#include "audio_bank_fixture.h"
#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <new>
#include <source_location>

thread_local long allocation_budget=-1;
void* operator new(std::size_t n){if(allocation_budget==0)throw std::bad_alloc();if(allocation_budget>0)--allocation_budget;if(auto p=std::malloc(n?n:1))return p;throw std::bad_alloc();}
void* operator new[](std::size_t n){return ::operator new(n);}
void operator delete(void* p)noexcept{std::free(p);}void operator delete[](void* p)noexcept{std::free(p);}
void operator delete(void* p,std::size_t)noexcept{std::free(p);}void operator delete[](void* p,std::size_t)noexcept{std::free(p);}
using namespace mscharged::resources;
using namespace audio_bank_fixture;
namespace
{
unsigned checks=0;
void Check(bool v,const char* why){++checks;if(!v)throw std::runtime_error(why);}
template<class F>void Reject(F f,std::source_location at=std::source_location::current())
{++checks;try{f();}catch(const std::exception&){return;}throw std::runtime_error("Invalid DSP input accepted at "+std::to_string(at.line()));}
void Half(Data& b,std::size_t at,std::uint16_t n){b.at(at)=n>>8;b.at(at+1)=n;}
void State(Fixture& f,unsigned sample,unsigned ps,int h1,int h2,const std::array<std::int16_t,16>& coefficients)
{
 auto a=f.sp+4+5*28+sample*46;
 for(unsigned j=0;j<16;++j)Half(f.bytes,a+j*2,std::uint16_t(coefficients[j]));
 Half(f.bytes,a+34,ps);Half(f.bytes,a+36,h1);Half(f.bytes,a+38,h2);
}
std::vector<std::int16_t> Oracle(const AudioResidentBank& bank,unsigned index)
{
 const auto& s=bank.Samples().at(index);const auto encoded=bank.SampleBytes(index);
 unsigned predictor=s.predictor_scale/16,scale=s.predictor_scale%16;
 std::int64_t h1=s.history1,h2=s.history2;std::vector<std::int16_t> out;
 for(std::uint32_t address=s.current_nibble;address<=s.end_nibble;++address)
 {
  const auto byte=encoded[address/2-s.first_byte];
  if(address%16<2){if(address%16==0){predictor=(byte/16)%8;scale=byte%16;}continue;}
  const int raw=address%2?byte%16:byte/16;const int nibble=raw>=8?raw-16:raw;
  const std::int64_t accumulated=s.coefficients[predictor*2]*h1+s.coefficients[predictor*2+1]*h2+std::int64_t(nibble)*(std::int64_t(1)<<scale)*2048;
  // Independent division formulation: floor((acc+1024)/2048), then PCM16 clamp.
  // C++ signed division truncates toward zero, so explicitly floor negatives.
  const auto value=accumulated+1024;
  const auto rounded=value>=0?value/2048:-((-value+2047)/2048);
  const auto pcm=std::clamp<std::int64_t>(rounded,-32768,32767);
  out.push_back(static_cast<std::int16_t>(pcm));h2=h1;h1=pcm;
 }
 return out;
}
void Compare(AudioResidentBank::Handle bank,unsigned index)
{
 const auto expected=Oracle(*bank,index);const auto result=DecodeAudioDsp(bank,index);
 Check(result->Rate()==bank->Samples()[index].rate&&result->SourceSample()==index&&result->Samples().size()==expected.size(),"DSP output identity/count differs");
 for(unsigned i=0;i<expected.size();++i)Check(result->Samples()[i]==expected[i],"Independent integer DSP sample differs");
}
std::uint64_t Hash(std::span<const std::int16_t> pcm)
{std::uint64_t h=14695981039346656037ULL;for(auto v:pcm)for(unsigned shift:{0u,8u}){h^=std::uint8_t(std::uint16_t(v)>>shift);h*=1099511628211ULL;}return h;}
void Generated()
{
 auto fixture=Make();std::array<std::int16_t,16> coefficients{};
 Put(fixture.bytes,fixture.sp+20,15);State(fixture,0,0,0,0,coefficients);fixture.wave[0]=0x7f;
 const std::array<std::uint8_t,7> sequence{0x12,0x34,0x56,0x78,0x9a,0xbc,0xde};std::copy(sequence.begin(),sequence.end(),fixture.wave.begin()+1);
 auto bank=ReadAudioResidentBank(fixture.bytes,fixture.wave);auto pcm=DecodeAudioDsp(bank,0);
 const std::array<std::int16_t,14> expected{1,2,3,4,5,6,7,-8,-7,-6,-5,-4,-3,-2};Check(std::equal(pcm->Samples().begin(),pcm->Samples().end(),expected.begin(),expected.end()),"Signed nibble order or initial SP state differs");
 // First SP predictor overrides the first encoded header; later headers own their frame.
 coefficients[0]=2048;coefficients[1]=-1024;coefficients[2]=1024;coefficients[3]=512;coefficients[14]=-32768;coefficients[15]=32767;
 for(unsigned predictor=0;predictor<8;++predictor)for(unsigned scale:{0u,1u,7u,12u,15u})
 {
  auto f=Make();State(f,0,predictor*16+scale,12345,-23456,coefficients);f.wave[0]=0xff;f.wave[8]=0x91;
  for(unsigned j=1;j<8;++j)f.wave[j]=std::uint8_t(0xf0+j*17);for(unsigned j=9;j<16;++j)f.wave[j]=std::uint8_t(j*31);
  Compare(ReadAudioResidentBank(f.bytes,f.wave),0);
 }
 // Every possible midframe start and both endpoint parities, including the
 // crossing of a frame header. Histories refer to the stated current address.
 for(unsigned first=2;first<16;++first)for(unsigned last=first;last<32;++last)if(last%16>=2)
 {auto f=Make();Put(f.bytes,f.sp+24,first);Put(f.bytes,f.sp+20,last);State(f,0,0x13,-17,31,coefficients);f.wave[8]=0xf4;Compare(ReadAudioResidentBank(f.bytes,f.wave),0);}
 // Exact half-step ties, negative floor, and saturation with extreme coefficients.
 struct Vector{int c0,c1,h1,h2,nibble,scale,expected;};
 const Vector vectors[]={{1,0,1024,0,0,0,1},{1,0,-1024,0,0,0,0},{1,0,-1025,0,0,0,-1},{1,0,1023,0,0,0,0},
  {32767,32767,32767,32767,7,15,32767},{-32768,32767,32767,-32768,-8,15,-32768},{0,0,0,0,-8,15,-32768},{0,0,0,0,7,15,32767}};
 for(const auto& v:vectors)
 {auto f=Make();Put(f.bytes,f.sp+20,2);coefficients={};coefficients[0]=v.c0;coefficients[1]=v.c1;State(f,0,v.scale,v.h1,v.h2,coefficients);f.wave[1]=std::uint8_t((v.nibble&15)<<4);
  auto b=ReadAudioResidentBank(f.bytes,f.wave);auto decoded=DecodeAudioDsp(b,0);Check(decoded->Samples().size()==1&&decoded->Samples()[0]==v.expected,"Known DSP rounding/clamp vector failed");Compare(b,0);}
 Reject([&]{DecodeAudioDsp({},0);});Reject([&]{DecodeAudioDsp(bank,5);});Reject([&]{DecodeAudioDsp(bank,0,0);});Reject([&]{DecodeAudioDsp(bank,0,13);});Reject([&]{DecodeAudioDsp(bank,0,16*1024*1024+1);});
 auto bad=Make();Half(bad.bytes,bad.sp+4+5*28+32,1);auto unqualified=ReadAudioResidentBank(bad.bytes,bad.wave);Reject([&]{DecodeAudioDsp(unqualified,0);});
 bad=Make();Half(bad.bytes,bad.sp+4+5*28+34,0x80);Reject([&]{ReadAudioResidentBank(bad.bytes,bad.wave);});
 auto retained=pcm;std::weak_ptr<const AudioResidentBank> weak=bank;bank.reset();fixture.bytes.clear();fixture.bytes.shrink_to_fit();fixture.wave.clear();fixture.wave.shrink_to_fit();Check(weak.expired()&&retained->Samples()[7]==-8,"PCM kept borrowed data or depended on released SP storage");
 auto f=Make();bank=ReadAudioResidentBank(f.bytes,f.wave);unsigned failures=0;bool success=false;
 for(long budget=0;budget<10&&!success;++budget)
 {bool failed=false;allocation_budget=budget;try{pcm=DecodeAudioDsp(bank,0);}catch(const std::bad_alloc&){failed=true;}allocation_budget=-1;
  if(failed){++failures;Check(pcm==retained,"Failed decode replaced published PCM");}else success=true;}
 Check(success&&failures>=3,"Decoder allocation sweep missed publication ownership");
 for(unsigned i=0;i<5;++i)Compare(bank,i);
}
Data Read(const char* path){std::ifstream f(path,std::ios::binary);if(!f)throw std::runtime_error("Cannot read supplied audio file");return {std::istreambuf_iterator<char>(f),{}};}
void Owned(const char* metadata,const char* wave)
{
 auto bank=ReadAudioResidentBank(Read(metadata),Read(wave));std::vector<AudioPcmSample::Handle> samples;
 for(unsigned i=0;i<bank->Samples().size();++i)
 {Compare(bank,i);auto pcm=DecodeAudioDsp(bank,i);std::cout<<"DSP "<<i<<" rate "<<pcm->Rate()<<" samples "<<pcm->Samples().size()<<" PCM16LE-FNV "<<std::hex<<Hash(pcm->Samples())<<std::dec<<'\n';samples.push_back(std::move(pcm));}
 std::weak_ptr<const AudioResidentBank> weak=bank;bank.reset();Check(weak.expired(),"PCM unnecessarily retains encoded bank");for(auto pcm:samples)Check(!pcm->Samples().empty(),"PCM disappeared with encoded bank");
}
}
int main(int argc,char**argv)
{try{Check(argc==1||argc==3,"Supply optional resbun and nlxwb");Generated();if(argc==3)Owned(argv[1],argv[2]);std::cout<<checks<<" DSP decoder checks passed\n";}catch(const std::exception&e){std::cerr<<"FAILED: "<<e.what()<<" (check "<<checks<<")\n";return 1;}}
