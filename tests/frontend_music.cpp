#include "runtime/frontend_music.h"
#include "runtime/startup.h"
#include "runtime/startup_files.h"
#include "frontend_music_fixture.h"
#include "NL/MemAlloc.h"
#include "NL/nlFileGC.h"
#include <aurora/aurora.h>
#include <aurora/dvd.h>
#include <dolphin/dvd.h>
#include <SDL3/SDL.h>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <thread>
#include <source_location>
#include <cstdlib>
#include <new>
#include <cmath>
#include <cstring>
#include <atomic>
#include <mutex>
#include <condition_variable>

thread_local long allocation_budget=-1;
void* operator new(std::size_t n){if(allocation_budget==0)throw std::bad_alloc();if(allocation_budget>0)--allocation_budget;if(auto p=std::malloc(n?n:1))return p;throw std::bad_alloc();}
void* operator new[](std::size_t n){return ::operator new(n);}
void operator delete(void* p)noexcept{std::free(p);}void operator delete[](void* p)noexcept{std::free(p);}
void operator delete(void* p,std::size_t)noexcept{std::free(p);}void operator delete[](void* p,std::size_t)noexcept{std::free(p);}
using namespace mscharged;using namespace mscharged::resources;using namespace frontend_music_fixture;
namespace
{
unsigned checks=0;
void Check(bool value,const char* why){++checks;if(!value)throw std::runtime_error(why);}
template<class F>void Reject(F f,std::source_location location=std::source_location::current())
{++checks;try{f();}catch(const std::exception&){return;}throw std::runtime_error("Invalid music accepted at "+std::to_string(location.line()));}
Data Load(const std::filesystem::path& path){std::ifstream f(path,std::ios::binary);Check(bool(f),"Missing fixture");return Data(std::istreambuf_iterator<char>(f),{});}
void Save(const std::filesystem::path& path,const Data& data){std::ofstream f(path,std::ios::binary);f.write(reinterpret_cast<const char*>(data.data()),data.size());Check(bool(f),"Cannot write generated fixture");}
// Independent signed integer decoder: no production helper or shifted negative
// integer round assumption. It reads full per-channel bytes by file offset.
std::vector<std::int16_t> Oracle(const Fixture& f,const AudioStreamFormat& fmt,unsigned channel,unsigned cycles)
{
 const auto& h=fmt.Channels()[channel];std::int64_t h1=h.history1,h2=h.history2;std::vector<std::int16_t> out;
 for(unsigned cycle=0;cycle<cycles;++cycle)for(unsigned n=0;n<((h.nibbles+1)/2+31)/32*64;++n)
 {
  if(n%16<2)continue;const auto local=n/2;const auto block=local/f.block,at=local%f.block;
  const auto frame=fmt.BlockOffset(block,channel)+at-at%8;const auto byte=f.wave.at(fmt.BlockOffset(block,channel)+at);
  const unsigned ps=f.wave.at(frame);int nibble=n%2?byte&15:byte>>4;if(nibble>=8)nibble-=16;
  std::int64_t value=std::int64_t(nibble)*(1<<(ps&15))*2048+h.coefficients[(ps>>4)*2]*h1+h.coefficients[(ps>>4)*2+1]*h2+1024;
  value=value>=0?value/2048:-((-value+2047)/2048);value=std::clamp<std::int64_t>(value,-32768,32767);
  h2=h1;h1=value;out.push_back(static_cast<std::int16_t>(value));
 }
 return out;
}
void Decoder(Fixture f)
{
 auto bank=ReadAudioStreamBank(f.metadata,f.wave.size());Check(bank->Tracks().size()==2,"Stream count differs");
 for(unsigned track=0;track<2;++track)
 {
  auto fmt=ReadAudioStreamFormat(bank,track,Bytes(f.wave).subspan(bank->Tracks()[track].offset,204));
  auto d=BeginAudioStream(fmt);auto original=d;std::vector<std::int16_t> left,right;
  for(unsigned cycle=0;cycle<3;++cycle)for(unsigned block=0;block<fmt->Blocks();++block)
  {
   auto a=Bytes(f.wave).subspan(fmt->BlockOffset(block,0),fmt->BlockBytes()),b=Bytes(f.wave).subspan(fmt->BlockOffset(block,1),fmt->BlockBytes());
   auto pcm=DecodeAudioStreamBlock(fmt,d,a,b);for(unsigned i=0;i<pcm.frames;++i){left.push_back(pcm.stereo[i*2]);right.push_back(pcm.stereo[i*2+1]);}
  }
  const auto expected_l=Oracle(f,*fmt,0,3),expected_r=Oracle(f,*fmt,1,3);
  Check(left==expected_l&&right==expected_r,"Independent DSP integer/interleave/loop oracle differs");Check(left.size()==fmt->CycleFrames()*3,"Stream sample total differs");
  auto before=d;auto bad=Data(f.block,0xff);Reject([&]{DecodeAudioStreamBlock(fmt,d,bad,bad);});Check(d.next_block==before.next_block&&d.history1==before.history1&&d.history2==before.history2,"Bad decode changed histories");
  Reject([&]{DecodeAudioStreamBlock(fmt,d,{},{});});Reject([&]{fmt->BlockOffset(fmt->Blocks(),0);});Reject([&]{fmt->BlockOffset(0,2);});
  d=BeginAudioStream(fmt);Check(d.history1==original.history1&&d.history2==original.history2&&d.next_block==0,"Restart did not reset source history");
 }
 for(unsigned n=0;n<f.metadata.size();++n)Reject([&]{ReadAudioStreamBank(Bytes(f.metadata).first(n),f.wave.size());});
 auto b=f.wave;b[0]=0;Reject([&]{ReadAudioStreamFormat(bank,0,Bytes(b).first(204));});
 b=f.wave;Half(b,24,1);Reject([&]{ReadAudioStreamFormat(bank,0,Bytes(b).first(204));});
 for(unsigned n=0;n<204;++n)Reject([&]{ReadAudioStreamFormat(bank,0,Bytes(f.wave).first(n));});
 Reject([&]{ReadAudioStreamBank(f.metadata,1);});
}
struct Session{bool live=false,disc=false;~Session(){if(live)ResetStartupFiles();if(disc)aurora_dvd_close();if(live){ResetStartupMemory();aurora_shutdown();}}};
Data ReadFile(const char* path)
{std::unique_ptr<nlFile> f(nlOpen(path));Check(bool(f),"Missing NL profile");const auto size=nlFileSize(f.get(),nullptr);Check(size&&size<=MaximumAssetBytes,"Oversize NL profile");Data b(size);nlRead(f.get(),b.data(),size,size);return b;}
void Pump(FrontendMusic& music)
{const auto until=std::chrono::steady_clock::now()+std::chrono::seconds(10);while(music.Status().load==FrontendMusicLoadState::Loading){music.Service();Check(std::chrono::steady_clock::now()<until,"Music preparation timed out");std::this_thread::sleep_for(std::chrono::milliseconds(1));}music.Check();}
void ReentryCallback(nlFile*,void*,unsigned,nlFileAsyncParam data)
{
 auto& music=*reinterpret_cast<FrontendMusic*>(data);const auto before=music.Status();unsigned seed=12;
 Reject([&]{music.BeginSelect(0,seed);});Reject([&]{music.Service();});Reject([&]{music.Poll();});
 Reject([&]{music.Pause();});Reject([&]{music.Resume();});Reject([&]{music.Stop();});Reject([&]{music.Unload();});
 Check(music.Status().cue==before.cue&&seed==12,"Reentrant mutation changed active stream");
}
void Disk(resources::AudioBankCatalog::Handle catalog,resources::AudioCalculationInitial::Handle calc,const std::filesystem::path& folder)
{
 const auto path=folder/"music-output.raw";
 SDL_SetHint(SDL_HINT_AUDIO_DRIVER,"disk");SDL_SetHint(SDL_HINT_AUDIO_DISK_OUTPUT_FILE,path.string().c_str());
 SDL_SetHint(SDL_HINT_AUDIO_FORMAT,"F32");SDL_SetHint(SDL_HINT_AUDIO_FREQUENCY,"44100");SDL_SetHint(SDL_HINT_AUDIO_CHANNELS,"2");
 {
  FrontendMusic music(catalog,calc);unsigned seed=1;music.BeginSelect(1,seed);Pump(music);
  const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(4);
  while(music.Status().source_state!=1){music.Service();Check(std::chrono::steady_clock::now()<deadline,"Disk stream did not complete");std::this_thread::sleep_for(std::chrono::milliseconds(1));}
  Check(music.Status().event_state==8,"Original sound event did not wait for source state1");music.Unload();
 }
 auto fixture=Make(true);auto bank=ReadAudioStreamBank(fixture.metadata,fixture.wave.size());auto fmt=ReadAudioStreamFormat(bank,1,Bytes(fixture.wave).subspan(6368,204));
 const auto left=Oracle(fixture,*fmt,0,1),right=Oracle(fixture,*fmt,1,1);const auto bytes=Load(path);
 Check(bytes.size()%8==0,"SDL disk output is not stereo float");std::vector<float> samples(bytes.size()/4);std::memcpy(samples.data(),bytes.data(),bytes.size());
 // Original zero-parent Update(0) leaves duration1: generated slider2 is+1dB.
 // Independent nearest-integer MIX gain at-6.5dB is15504.
 const unsigned input=unsigned(std::lround(std::pow(10.0,-65.0/200.0)*32767.0));const double gain=double(input)*32767.0/(32768.0*32768.0*32768.0);
 bool found=false;
 for(std::size_t at=0;at+left.size()*2<=samples.size();at+=2)
 {
  if(std::abs(samples[at]-float(left[0]*gain))>1e-7||std::abs(samples[at+1]-float(right[0]*gain))>1e-7)continue;
  bool same=true;for(unsigned i=0;i<left.size();++i)if(std::abs(samples[at+i*2]-float(left[i]*gain))>2e-7||std::abs(samples[at+i*2+1]-float(right[i]*gain))>2e-7){same=false;break;}
  if(same){found=true;break;}
 }
 Check(found,"Real SDL stereo output differs from independent ADPCM/MIX oracle");
 SDL_ResetHint(SDL_HINT_AUDIO_DISK_OUTPUT_FILE);SDL_ResetHint(SDL_HINT_AUDIO_FORMAT);SDL_ResetHint(SDL_HINT_AUDIO_FREQUENCY);SDL_ResetHint(SDL_HINT_AUDIO_CHANNELS);SDL_ResetHint(SDL_HINT_AUDIO_DRIVER);
}
struct FaultWave
{
 Data bytes=Make().wave;std::atomic<int> mode{0};std::atomic<unsigned> handles{0};std::atomic<bool> entered{false};
 std::mutex mutex;std::condition_variable condition;bool release=false;
 struct Handle{FaultWave* owner;std::int64_t position=0;};
 static void* Open(void* p){auto& o=*static_cast<FaultWave*>(p);auto* h=new Handle{&o};++o.handles;return h;}
 static void Close(void* p){auto* h=static_cast<Handle*>(p);--h->owner->handles;delete h;}
 static std::int64_t Seek(void* p,std::int64_t n,std::int32_t origin){auto& h=*static_cast<Handle*>(p);if(origin||n<0||std::uint64_t(n)>h.owner->bytes.size())return -1;return h.position=n;}
 static std::int64_t Read(void* p,std::uint8_t* output,std::size_t count)
 {
  auto& h=*static_cast<Handle*>(p);auto& o=*h.owner;
  if(h.position>=6368&&o.mode)
  {
   o.entered=true;
   if(o.mode==1)return -1;
   if(o.mode==2){if(h.position>=6371)return 0;count=std::min<std::size_t>(count,3);}
   if(o.mode==3){std::unique_lock lock(o.mutex);if(!o.condition.wait_for(lock,std::chrono::seconds(3),[&]{return o.release;}))return -1;}
  }
  count=std::min<std::size_t>(count,o.bytes.size()-h.position);std::memcpy(output,o.bytes.data()+h.position,count);h.position+=count;return count;
 }
};
void FailedDiscOpen()
{
 // The nod provider allocates its native handle before C++ command metadata.
 const auto entry=DVDConvertPathToEntrynum("/audio/FE_GEN_Music.nlxwb");
 Check(entry>=0,"Missing DVD allocation fixture");DVDFileInfo info{};
 allocation_budget=0;const auto opened=DVDFastOpen(entry,&info);allocation_budget=-1;
 Check(!opened&&!info.cb.userData,"Failed nod command allocation published a handle");
 Check(DVDClose(&info),"Failed DVD admission was not safely closable");
 std::unique_ptr<nlFile> file(nlOpen("audio/FE_GEN_Music.nlxwb"));
 alignas(32) std::array<std::array<std::uint8_t,32>,65> buffers{};
 allocation_budget=0;bool failed=false;
 try{nlReadAsync(file.get(),buffers[0].data(),32,nullptr,0,32);}catch(const std::bad_alloc&){failed=true;}
 allocation_budget=-1;
 Check(failed&&!nlAsyncReadsPending(file.get()),"Failed raw open retained PendingAsync");
 // None of the 64 native request slots may be consumed by failed admission.
 for(unsigned i=0;i<64;++i){nlSeek(file.get(),0,0);nlReadAsync(file.get(),buffers[i].data(),32,nullptr,0,32);}
 Reject([&]{nlReadAsync(file.get(),buffers[64].data(),32,nullptr,0,32);});
 nlCancelPendingAsyncReads(file.get(),nullptr);Check(!nlAsyncReadsPending(nullptr),"Restored NL queue did not drain");
}
void Faults(AudioBankCatalog::Handle catalog,AudioCalculationInitial::Handle calc)
{
 FaultWave fault;const AuroraOverlayCallbacks callbacks{FaultWave::Open,FaultWave::Close,FaultWave::Read,FaultWave::Seek};aurora_dvd_overlay_callbacks(&callbacks);
 const AuroraOverlayFile overlay{"/audio/FE_GEN_Music.nlxwb",&fault,static_cast<std::uint32_t>(fault.bytes.size())};aurora_dvd_overlay_files(&overlay,1,nullptr);
 struct Clear{~Clear(){aurora_dvd_overlay_files(nullptr,0,nullptr);}} clear;
 // Fail both callback allocation and the metadata allocation after a successful
 // callback open. Every provider handle must close exactly once.
 const auto entry=DVDConvertPathToEntrynum("/audio/FE_GEN_Music.nlxwb");
 for(long budget:{0L,1L})
 {
  DVDFileInfo info{};allocation_budget=budget;const auto opened=DVDFastOpen(entry,&info);allocation_budget=-1;
  Check(!opened&&!info.cb.userData&&fault.handles==0,"Failed overlay open leaked provider ownership");
  Check(DVDClose(&info),"Failed overlay admission was not safely closable");
 }

 for(int mode:{1,2,3})
 {
  fault.mode=0;fault.entered=false;fault.release=false;
  {
   FrontendMusic music(catalog,calc);unsigned seed=4;music.BeginSelect(0,seed);Pump(music);music.Pause();fault.mode=mode;music.BeginSelect(1,seed);
   if(mode!=3){Reject([&]{Pump(music);});Check(fault.entered&&music.Status().load==FrontendMusicLoadState::Failed,"Actual short/error stream read was accepted");}
   else
   {
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(3);
    while(!fault.entered){music.Service();Check(std::chrono::steady_clock::now()<deadline,"Music worker never entered controlled read");std::this_thread::sleep_for(std::chrono::milliseconds(1));}
    std::jthread release([&]{std::this_thread::sleep_for(std::chrono::milliseconds(10));{std::lock_guard lock(fault.mutex);fault.release=true;}fault.condition.notify_all();});
    music.CancelPending();release.join();Check(music.Status().load==FrontendMusicLoadState::Cancelled,"Active-worker cancellation published music");
   }
   Check(music.Status().cue==0xe326f931&&music.Status().paused&&seed==4,"Failed/cancelled real refill replaced current music");music.Unload();
  }
  Check(fault.handles==0&&!nlAsyncReadsPending(nullptr),"Music fault cleanup retained workers/files");
 }
}
void Runtime(int argc,char** argv)
{
 const std::string mode=argv[3];const auto folder=(std::filesystem::path(argv[2])/"music-host").string();std::filesystem::create_directories(folder);
 AuroraConfig config{};config.appName="Charged streamed frontend music";config.userPath=config.cachePath=folder.c_str();config.resourcesPath=SDL_GetBasePath();config.desiredBackend=BACKEND_NULL;
 config.windowWidth=320;config.windowHeight=240;config.windowPosX=config.windowPosY=-1;config.logLevel=LOG_WARNING;config.mem1Size=MEM1_DEFAULT_SIZE;config.mem2Size=64*1024*1024;
 Session session;auto host=aurora_initialize(argc,argv,&config);session.live=true;Check(host.window,"Aurora initialization failed");InitializeStartupOS();nlInitMemory();Check(aurora_dvd_open(argv[1]),"Cannot mount music disc");session.disc=true;nlInitFileSystem();
 AudioBankCatalog::Handle catalog;AudioCalculationInitial::Handle calculation;
 if(mode=="owned"){auto global=ReadFile("audio/nlxgs.bun");catalog=ReadAudioBankCatalog(global);calculation=ReadAudioCalculationInitial(global);}
 else{auto c=std::make_shared<AudioBankCatalog>();c->names.resize(27);c->slots.resize(23);c->names[26]={26,"FE_GEN_Music"};c->slots[22]={22,0,0,true};catalog=c;calculation=ReadAudioCalculationInitial(ReadFile("audio/calculation.bun"));}
 if(mode=="disk"){Disk(catalog,calculation,argv[2]);return;}
 if(mode=="success"){FailedDiscOpen();Faults(catalog,calculation);}
 const auto init=SDL_WasInit(SDL_INIT_AUDIO);
 for(unsigned attempt=0;attempt<3;++attempt)
 {
  const auto m1=StandardAllocator.TotalFreeMemory(),m2=VirtualAllocator.TotalFreeMemory();
  {
   FrontendMusic music(catalog,calculation);unsigned seed=123;Reject([&]{music.BeginSelect(2,seed);});
   if(mode=="missing")Reject([&]{music.BeginSelect(0,seed);});
   else
   {
    music.BeginSelect(0,seed);Check(seed==123&&music.Status().requested_reads==1,"Music metadata/seed ordering differs");
    const auto pending_title=music.Status();unsigned duplicate_seed=456;
    music.BeginSelect(0,duplicate_seed);const auto repeated_title=music.Status();
    Check(repeated_title.load==FrontendMusicLoadState::Loading&&repeated_title.requested_reads==pending_title.requested_reads
        &&repeated_title.completed_reads==pending_title.completed_reads&&!repeated_title.cue&&duplicate_seed==456,
        "Repeated pending Title selection restarted or published its real load");
    Reject([&]{music.BeginSelect(1,duplicate_seed);});
    Check(music.Status().load==FrontendMusicLoadState::Loading&&music.Status().requested_reads==pending_title.requested_reads,
        "Different pending selection changed the actual Title request");
    if(mode=="cancel")
    {
     while(music.Status().requested_reads<attempt+1){music.Service();std::this_thread::sleep_for(std::chrono::milliseconds(1));}
     music.CancelPending();Check(music.Status().load==FrontendMusicLoadState::Cancelled&&!music.Status().cue,"Cancelled music published output");
    }
    else if(mode=="malformed"||mode=="badheader")
    {Reject([&]{Pump(music);});Check(music.Status().load==FrontendMusicLoadState::Failed&&!music.Status().cue,"Invalid stream became ready");}
    else
    {
     Pump(music);auto status=music.Status();Check(status.cue==0xe326f931&&status.source_state==4&&status.submitted_frames>0,"Title did not admit real output");
     Check(status.completed_reads>=3&&status.requested_reads>=3,"Title did not perform NL metadata/header/block reads");
     Check(music.Pause()&&!music.Pause(),"Original stream pause state differs");music.Poll();const auto paused=music.Status();Check(paused.source_state==7&&paused.paused,"Paused source state differs");
     Check(music.Resume()&&!music.Resume(),"Original stream resume state differs");
     if(mode=="success"&&attempt==0)
     {
      std::unique_ptr<nlFile> callback_file(nlOpen("audio/calculation.bun"));std::array<std::uint8_t,32> storage{};
      nlReadAsync(callback_file.get(),storage.data(),storage.size(),ReentryCallback,reinterpret_cast<nlFileAsyncParam>(&music),storage.size());
      while(nlAsyncReadsPending(callback_file.get())){music.Service();std::this_thread::sleep_for(std::chrono::milliseconds(1));}
      bool thread_rejected=false;std::thread other([&]{try{music.Pause();}catch(const std::logic_error&){thread_rejected=true;}});other.join();Check(thread_rejected,"Foreign-thread mutation succeeded");
      music.Pause();unsigned failures=0;bool complete=false;
      for(long budget=0;budget<80&&!complete;++budget)
      {
       allocation_budget=budget;try{music.BeginSelect(1,seed);complete=true;}catch(const std::bad_alloc&){++failures;}allocation_budget=-1;
       Check(music.Status().cue==0xe326f931&&music.Status().paused&&seed==123,"Allocation-failed replacement changed current stream");
       music.CancelPending();
      }
      Check(complete&&failures>=2,"Music begin allocation sweep missed ownership");music.Resume();
     }
     music.BeginSelect(0,seed);Check(music.Status().load==FrontendMusicLoadState::Ready,"Same cue unnecessarily reloaded");
     if(mode=="badmain")
     {
      music.BeginSelect(1,seed);Reject([&]{Pump(music);});Check(music.Status().cue==0xe326f931&&music.Status().source_state==4&&seed==123,"Failed replacement lost previous output or RNG");
     }
     else
     {
      music.BeginSelect(1,seed);Check(music.Status().cue==0xe326f931,"Pending replacement retired old music");
      const auto pending_main=music.Status();duplicate_seed=789;music.BeginSelect(1,duplicate_seed);
      Check(music.Status().load==FrontendMusicLoadState::Loading&&music.Status().cue==pending_main.cue
          &&music.Status().requested_reads==pending_main.requested_reads&&music.Status().completed_reads==pending_main.completed_reads&&duplicate_seed==789,
          "Repeated pending Main selection replaced its request or current output");
      Reject([&]{music.BeginSelect(0,duplicate_seed);});Pump(music);
      Check(music.Status().cue==0x445abf3a&&seed==123,"Main replacement/seed differs");
      const auto until=std::chrono::steady_clock::now()+std::chrono::milliseconds(mode=="owned"?(attempt==0?9500:250):400);
      while(std::chrono::steady_clock::now()<until){music.Service();std::this_thread::sleep_for(std::chrono::milliseconds(1));}
      if(mode=="owned"&&attempt==0)Check(music.Status().completed_cycles>=1&&music.Status().completed_reads>=10,"Owned Main stream did not refill/wrap");
      if(mode=="success")Check(music.Status().completed_cycles>=2,"Generated stream never refilled/wrapped");
      if(mode=="finite")Check(music.Status().source_state==1&&music.Status().event_state==8&&music.Status().completed_cycles==1,"Finite stream did not drain through state6 to1");
      std::cout<<"Music cue "<<std::hex<<music.Status().cue<<std::dec<<": reads="<<music.Status().completed_reads<<" submitted="<<music.Status().submitted_frames<<" cycles="<<music.Status().completed_cycles<<'\n';
     }
     music.Stop();Check(music.Status().source_state==6&&music.Status().queued_input_bytes==0,"Music stop did not discard output");music.Poll();Check(music.Status().source_state==1,"Music stop did not reach original idle state");
    }
   }
   music.Unload();Check(music.Status().load==FrontendMusicLoadState::Unloaded,"Music unload differs");Reject([&]{music.BeginSelect(0,seed);});
  }
  Check(!nlAsyncReadsPending(nullptr),"Music teardown kept NL reads");Check(StandardAllocator.TotalFreeMemory()==m1&&VirtualAllocator.TotalFreeMemory()==m2,"Music teardown failed arena recovery");
  Check(SDL_WasInit(SDL_INIT_AUDIO)==init,"Music teardown leaked SDL audio reference");
 }
 if(mode=="success")
 {
  FrontendMusic bad(catalog,calculation,{0x1234567});unsigned seed=2;bad.BeginSelect(0,seed);Reject([&]{Pump(bad);});Check(seed==2&&!bad.Status().cue,"Invalid output device published music");
 }
}
}
int main(int argc,char** argv)
{
 try
 {
  if(argc==3&&std::string(argv[1])=="--fixture")
  {std::filesystem::create_directories(argv[2]);auto f=Make();Save(std::filesystem::path(argv[2])/"music.resbun",f.metadata);Save(std::filesystem::path(argv[2])/"music.nlxwb",f.wave);Save(std::filesystem::path(argv[2])/"finite.resbun",Make(true).metadata);Save(std::filesystem::path(argv[2])/"calculation.bun",Calculation());Decoder(f);}
  else if(argc==4)Runtime(argc,argv);
  else if(argc==3&&std::string(argv[1])=="--owned-decode")
  {
   const std::filesystem::path p=argv[2];Fixture f;f.metadata=Load(p/"FE_GEN_Music.resbun");f.wave=Load(p/"FE_GEN_Music.nlxwb");auto bank=ReadAudioStreamBank(f.metadata,f.wave.size());f.block=bank->BlockBytes();
   for(unsigned index:{7u,9u})
   {
    auto fmt=ReadAudioStreamFormat(bank,index,Bytes(f.wave).subspan(bank->Tracks()[index].offset,204));auto d=BeginAudioStream(fmt);std::vector<std::int16_t> actual;
    for(unsigned block=0;block<fmt->Blocks();++block){auto pcm=DecodeAudioStreamBlock(fmt,d,Bytes(f.wave).subspan(fmt->BlockOffset(block,0),f.block),Bytes(f.wave).subspan(fmt->BlockOffset(block,1),f.block));actual.insert(actual.end(),pcm.stereo.begin(),pcm.stereo.end());}
    auto l=Oracle(f,*fmt,0,1),r=Oracle(f,*fmt,1,1);Check(actual.size()==l.size()*2,"Owned PCM length differs");for(unsigned i=0;i<l.size();++i)Check(actual[i*2]==l[i]&&actual[i*2+1]==r[i],"Owned independent stereo PCM differs");
    std::cout<<"Owned IDSP source"<<index<<": "<<l.size()<<" stereo frames at"<<fmt->Channels()[0].rate<<"Hz, "<<fmt->Blocks()<<" block pairs\n";
   }
  }
  else Decoder(Make());
  std::cout<<checks<<" frontend music checks passed\n";
 }
 catch(const std::exception& e){std::cerr<<"FAILED: "<<e.what()<<" (check"<<checks<<")\n";return 1;}
}
