#include "runtime/frontend_audio_options.h"
#include "runtime/frontend_handler.h"
#include "runtime/frontend_music.h"
#include "runtime/startup.h"
#include "runtime/startup_files.h"
#include "audio_bank_fixture.h"
#include "frontend_music_fixture.h"
#include "NL/nlFileGC.h"
#include "NL/MemAlloc.h"
#include <aurora/aurora.h>
#include <aurora/dvd.h>
#include <SDL3/SDL.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <source_location>
#include <thread>
#include <cstdlib>
#include <new>
thread_local long allocation_budget=-1;
void* operator new(std::size_t n){if(allocation_budget==0)throw std::bad_alloc();if(allocation_budget>0)--allocation_budget;if(auto p=std::malloc(n?n:1))return p;throw std::bad_alloc();}
void* operator new[](std::size_t n){return ::operator new(n);}
void operator delete(void* p)noexcept{std::free(p);}void operator delete[](void* p)noexcept{std::free(p);}
void operator delete(void* p,std::size_t)noexcept{std::free(p);}void operator delete[](void* p,std::size_t)noexcept{std::free(p);}
using namespace mscharged;using namespace mscharged::resources;using namespace audio_bank_fixture;
namespace
{
unsigned checks=0;
void Check(bool ok,const char* why){++checks;if(!ok)throw std::runtime_error(why);}
template<class F>void Reject(F f,std::source_location p=std::source_location::current()){++checks;try{f();}catch(const std::exception&){return;}throw std::runtime_error("Invalid Audio Options accepted at "+std::to_string(p.line()));}
constexpr std::array<float,11> levels{-96,-36,-18,-15,-12,-9,-6,-4.5,-3,-1.5,0};
unsigned Gain(float db){const int d=int(10*db);return d<=-904?0:d>=60?65380:unsigned(std::pow(10.0,double(d)/200)*32767.0);}
Data Calculation()
{
 Data header,records(120);Append(header,0x23401,Words({5,0xf1000100,0}));
 for(unsigned i=0;i<5;++i){Put(records,i*24,i);Put(records,i*24+4,0x1234+i);Put(records,i*24+12,i?0xf1000100:0);}Append(header,0x23402,records);return Wrap(0x80000001,Wrap(0x80023400,header));
}
Data Load(const char* path){std::unique_ptr<nlFile> f(nlOpen(path));Check(bool(f),"Required owned/synthetic NL file is absent");const auto n=nlFileSize(f.get(),nullptr);Data b(n);nlRead(f.get(),b.data(),n,n);return b;}
void SaveFile(const std::filesystem::path& p,const Data& b){std::ofstream f(p,std::ios::binary);f.write(reinterpret_cast<const char*>(b.data()),b.size());Check(bool(f),"Cannot write synthetic fixture");}
void Categories()
{
 auto raw=Calculation();auto profile=ReadAudioVolumeProfile(raw);auto volumes=std::make_shared<AudioCategoryVolumes>(profile,std::array{10,5,0});
 Check(volumes->Snapshot().values==std::array{0.f,-9.f,-96.f},"Explicit initial categories differ");Check(volumes->Value(0)==1&&volumes->Value(1)==0,"Original calculation dependency order differs");
 std::uint64_t frame=0;
 for(unsigned category=0;category<3;++category)for(int level=0;level<=10;++level)
 {auto before=volumes->Snapshot();volumes->Set(AudioCategory(category),level);auto pending=volumes->Snapshot();Check(pending.values==before.values&&pending.targets[category]==levels[level],"Target assignment advanced original time");volumes->Update(++frame,0);Check(volumes->Snapshot().values[category]==levels[level],"Independent category table differs");}
 volumes->SetAll({-1,99,5});volumes->Update(++frame,1);Check(volumes->Snapshot().settings==std::array{0,10,5},"Original category integer clamp differs");
 const auto before=volumes->Snapshot();Reject([&]{volumes->Update(frame,0);});Reject([&]{volumes->Update(frame+1,NAN);});Reject([&]{volumes->Update(frame+1,-1);});Reject([&]{volumes->Update(frame+1,1.01f);});Reject([&]{volumes->Set(AudioCategory(8),5);});Reject([&]{volumes->Value(5);});Check(volumes->Snapshot().frame==before.frame&&volumes->Snapshot().settings==before.settings,"Rejected category input mutated state");
 Reject([&]{AudioCategoryVolumes bad(profile,{-1,10,10});});Reject([&]{AudioCategoryVolumes bad({}, {10,10,10});});
 bool wrong=false;std::thread thread([&]{try{volumes->Snapshot();}catch(...){wrong=true;}});thread.join();Check(wrong,"Cross-thread category access accepted");
 for(std::size_t n=0;n<raw.size();++n)Reject([&]{ReadAudioVolumeProfile(Bytes(raw).first(n));});
 for(auto [at,value]:std::initializer_list<std::pair<unsigned,unsigned>>{{44,1},{52,Float(1)},{56,0xf1000100},{80,0},{80,0xf1000118}}){auto bad=raw;Put(bad,at,value);Reject([&]{ReadAudioVolumeProfile(bad);});}
}
void QueueGains()
{
 auto f=Make();auto bank=ReadAudioResidentBank(f.bytes,f.wave);unsigned seed=1;AudioBankSelection select(bank);auto selected=*select.Select({0x10203040,0,0,0},seed);auto initial=ReadAudioCalculationInitial(Calculation());
 float maximum_error=0;
 for(float db:levels)
 {
  auto buffer=PrepareResidentAudio(selected,initial,db);auto before=buffer->BeforeInputGain();Check(std::abs(int(buffer->Mix().input)-int(Gain(db)))<=1,"Original MIX table exceeds independent quantization tolerance");
  for(unsigned i=0;i<before.size();++i){const auto diff=std::abs(before[i]*(float(buffer->Mix().input)/32768)-buffer->Stereo()[i]);maximum_error=std::max(maximum_error,diff);Check(diff<=2.4e-7f,"Dynamic/static float transport differs beyond two float epsilons");}
  ResidentAudioOutput output(buffer,0,true);auto queued=output.Status().queued_input_bytes;Check(queued>0,"Prepared real SDL stream has no queued data");
  output.SetVolume(-96);Check(output.Status().input_gain==0,"Mute did not change actual SDL stream gain");output.SetVolume(0);Check(output.Status().input_gain==32767.f/32768&&output.Status().queued_input_bytes==queued,"Unmute lost queued pre-gain PCM");
  Reject([&]{output.SetVolume(NAN);});Reject([&]{output.SetVolume(7);});output.Stop();Reject([&]{output.SetVolume(0);});
 }
 for(float db:levels)
 {
  const auto buffer=PrepareResidentAudio(selected,initial,db);const float gain=float(buffer->Mix().input)/32768,pan=float(buffer->Mix().left)/32768;
  for(int sample=-32768;sample<32768;++sample){const float pcm=float(sample)/32768;const float a=(pcm*gain)*pan,b=(pcm*pan)*gain;maximum_error=std::max(maximum_error,std::abs(a-b));Check(std::abs(a-b)<=2.4e-7f,"Exhaustive int16 live/static float transport diverged");}
 }
 auto buffer=PrepareResidentAudio(selected,initial);ResidentAudioOutput ordinary(buffer);Reject([&]{ordinary.SetVolume(-6);});Check(ordinary.Status().input_gain==1,"Default output gain behavior changed");ordinary.Stop();
 std::cout<<"Static/live float maximum difference "<<maximum_error<<'\n';
}
std::shared_ptr<FrontendAudio> Audio(bool owned,AudioCategoryVolumes::Handle volumes)
{
 LoadedAudioBank::Handle loaded;
 if(owned){auto catalog=ReadAudioBankCatalog(Load("/audio/nlxgs.bun"));AudioBankLoad load(catalog,23,21);const auto end=std::chrono::steady_clock::now()+std::chrono::seconds(15);while(load.State()==AudioBankLoadState::Loading){load.Service();Check(std::chrono::steady_clock::now()<end,"Resident audio timed out");SDL_Delay(1);}loaded=load.Result();}
 else
 {
  auto f=Make();const auto chunks=[](Bytes data){std::vector<std::pair<unsigned,Data>> out;for(std::size_t at=0;at<data.size();){const auto id=U32(data,at),n=U32(data,at+4);out.emplace_back(id,Data(data.begin()+at+8,data.begin()+at+8+n));at+=(n+11)&~3u;}return out;};
  constexpr std::array keys{0x96deb5c3u,0x3021a1eeu,0x1f824c84u,0x304fdd1eu};Data root;
  for(auto [id,data]:chunks(Bytes(f.bytes).subspan(8)))
  {
   if(id==0x80023000){Data map,records;Append(map,0x23001,Words({4,0,0}));for(unsigned i=0;i<keys.size();++i){auto row=Words({keys[i],0,0,0,i});records.insert(records.end(),row.begin(),row.end());}Append(map,0x23003,records);data=std::move(map);}
   if(id==0x80023300){Data graph;bool refs=false;for(auto [kind,part]:chunks(data)){
    if(kind==0x23301)Put(part,8,4);
    if(kind==0x23302){Data rows;for(auto key:keys){auto row=Data(part.begin()+40,part.end());Put(row,0,key);rows.insert(rows.end(),row.begin(),row.end());}part=std::move(rows);}
    if(kind==0x23303)Put(part,12,4); // Synthetic real sound source belongs to SFX category.
    if(kind==0x23308){if(!refs){for(unsigned i=0;i<4;++i)Append(graph,kind,Words({0xf0001000,0,Float(255),0,0}));refs=true;}continue;}Append(graph,kind,part);}data=std::move(graph);}
   Append(root,id,data);
  }
  loaded=std::make_shared<const LoadedAudioBank>(LoadedAudioBank{23,21,{23,"FE_GEN_Sfx"},{21,0,1,false},ReadAudioResidentBank(Wrap(0x80000001,root),f.wave)});
 }
 AudioVoicesOptions options;options.category_volumes=volumes;return std::make_shared<FrontendAudio>(loaded,volumes->Initial(),options);
}
auto Session(const char* path="/Art/fe/options_audio_options.fen")
{auto owner=std::make_shared<FrontendSession>();owner->Begin({path,FrontendLanguage::English,FrontendImageProfile::Main,"OPTIONS_IN",true});const auto end=std::chrono::steady_clock::now()+std::chrono::seconds(15);while(owner->State()==FrontendSessionState::Loading){owner->Service();Check(std::chrono::steady_clock::now()<end,"Audio options resources timed out");SDL_Delay(1);}owner->Result();return owner;}
void Ack(FrontendAudioOptions& owner){owner.Acknowledge(owner.Current(),{1,640,480,640,480,0,0,640,480});}
std::array<float,2> Center(FrontendPointerBounds b){return{(b.min_x+b.max_x)/2,(b.min_y+b.max_y)/2};}
const FrontendInstance& Find(const FrontendSession::Handle& f,std::initializer_list<std::string_view> names)
{auto n=FindFrontendNode(f->graph,{},FrontendNamedPath(std::span(names.begin(),names.size())));Check(n&&n->kind==FrontendNodeKind::Instance,"Source authored lookup absent");const auto it=std::find_if(f->graph.instances.begin(),f->graph.instances.end(),[&](auto& i){return i.offset==n->id;});Check(it!=f->graph.instances.end(),"Source instance absent");return *it;}
void VisualOracle(const FrontendSession::Handle& frame,std::array<int,3> settings)
{
 const std::array groups{"MUSIC VOLUME","SFX VOLUME","VOX VOLUME"},labels{"MUSIC SETTING","SFX SETTING2","VOX SETTING3"};
 for(unsigned cat=0;cat<3;++cat)
 {
  for(unsigned i=0;i<10;++i){const std::string name="whitebox"+(i?std::to_string(i+1):std::string{});const auto& bar=Find(frame,{"OPTIONS_IN","Layer","visual_options",groups[cat],name});Check(bar.attributes.colour==(i<unsigned(settings[cat])?std::array<std::uint8_t,4>{0xa9,0xd0,0x46,0xff}:std::array<std::uint8_t,4>{0,0,0,0xff}),"Independent volume bar color oracle differs");}
  const auto& label=Find(frame,{"OPTIONS_IN","Layer","visual_options",labels[cat]});const auto n=std::to_string(settings[cat]);Check(label.text==u"LEVEL "+std::u16string(n.begin(),n.end()),"Original format label differs");
 }
}
void Menu(bool owned)
{
 FrontendInput input;std::array<FrontendPadSample,4> samples{};samples[0].connected=true;input.Update(samples,0);
 auto profile=ReadAudioVolumeProfile(owned?Load("/audio/nlxgs.bun"):Calculation());auto volumes=std::make_shared<AudioCategoryVolumes>(profile,std::array{5,5,5});auto audio=Audio(owned,volumes);auto session=Session();unsigned seed=17;FrontendAudioOptions owner(session,input,audio,volumes,seed);
 Check(owner.Status().settings==std::array{5,5,5}&&owner.Status().backup==std::array{5,5,5},"Explicit settings/backup differs");VisualOracle(owner.Current(),{5,5,5});
 Check(!Find(owner.Current(),{"OPTIONS_IN","Layer","blackbox"}).visible&&!Find(owner.Current(),{"OPTIONS_OUT","Layer","blackbox"}).visible,"Original main-mode blackboxes not hidden");
 Ack(owner);auto first=owner.Current();owner.DeliverPointer(first,{0,{0,0},true});Check(owner.Current()==first,"Intro admitted pointer mutation");
 FrontendHandler lock(session,input);lock.SetExclusiveInput(true);owner.AdvanceVisual(first,.1f);Check(owner.Current()==first,"Input lock did not precede base update");lock.SetExclusiveInput(false);lock.Release();
 for(unsigned n=0;owner.Status().state==0&&n<100;++n)owner.AdvanceVisual(owner.Current(),1.f/60);
 Check(owner.Status().state==1&&owner.Status().initialized,"Audio options authored intro did not finish");Ack(owner);auto bounds=owner.Bounds();
 std::uint64_t clock=0;
 for(unsigned item=0;item<6;++item)
 {
  auto old=owner.Current();auto settings=owner.Status().settings;owner.DeliverPointer(old,{0,Center(bounds[item]),true});settings[item/2]+=item%2?1:-1;
  Check(owner.Status().settings==settings&&volumes->Snapshot().settings==settings,"Original decrease/increase category differs");VisualOracle(owner.Current(),settings);auto prior=settings;prior[item/2]-=item%2?1:-1;VisualOracle(old,prior);
  volumes->Update(++clock,0);audio->Update(0);audio->ServiceAudio();audio->Update(0);
  Ack(owner);owner.DeliverPointer(owner.Current(),{0,{-999,-999}});Ack(owner);
 }
 for(unsigned category=0;category<3;++category)
 {
  for(unsigned item:{category*2,category*2+1})for(unsigned n=0;n<12;++n)
  {
   auto expected=owner.Status().settings;expected[category]=std::clamp(expected[category]+(item%2?1:-1),0,10);
   owner.DeliverPointer(owner.Current(),{0,Center(bounds[item]),true});Check(owner.Status().settings==expected,"Bounded source button eligibility differs");
   VisualOracle(owner.Current(),expected);volumes->Update(++clock,0);audio->Update(0);audio->ServiceAudio();audio->Update(0);
   Ack(owner);owner.DeliverPointer(owner.Current(),{0,{-999,-999}});Ack(owner);
  }
 }
 const auto before=owner.Current();Reject([&]{owner.Save(before);});Check(owner.Current()==before&&owner.Status().state==1,"Unavailable original save claimed mutation/success");
 owner.NotifyBackButton(before);Check(owner.Status().state==3&&volumes->Snapshot().settings==std::array{5,5,5},"Back did not restore genuine category settings");
 bool push=false;for(unsigned n=0;n<100&&!push;++n){owner.AdvanceVisual(owner.Current(),1.f/60);for(auto command:owner.Status().commands)if(command.kind==FrontendAudioOptionsCommandKind::PushOptions){Check(command.argument==13,"Source return scene differs");push=true;}}
 Check(push,"Authored out did not reach explicit Options push request");auto retained=owner.Current();owner.Release();owner.Release();Check(audio->Handles().empty()&&retained->visuals&&retained->images,"Audio options teardown lost resources or kept cues");Reject([&]{owner.Current();});audio->Unload();
 if(!owned){auto missing=Session("/Art/fe/audio-missing.fen");auto a=Audio(false,volumes);const auto previous=missing->Current();Reject([&]{FrontendAudioOptions invalid(missing,input,a,volumes,seed);});Check(missing->Current()==previous,"Failed original path lookup published partial visuals");a->Unload();}
}
void FailureOwnership()
{
 FrontendInput input;std::array<FrontendPadSample,4> samples{};samples[0].connected=true;input.Update(samples,0);auto volumes=std::make_shared<AudioCategoryVolumes>(ReadAudioVolumeProfile(Calculation()),std::array{5,5,5});auto audio=Audio(false,volumes);auto session=Session();unsigned seed=23,failures=0;bool done=false;
 allocation_budget=1000000000;auto measured=std::make_unique<FrontendAudioOptions>(session,input,audio,volumes,seed);const long allocations=1000000000-allocation_budget;allocation_budget=-1;measured->Release();measured.reset();const auto before=session->Current();
 for(long budget:{0l,1l,3l,31l,allocations/4,allocations/2,allocations-1,allocations})
 {
  std::unique_ptr<FrontendAudioOptions> owner;bool failed=false;allocation_budget=budget;try{owner=std::make_unique<FrontendAudioOptions>(session,input,audio,volumes,seed);}catch(const std::bad_alloc&){failed=true;}allocation_budget=-1;
  if(failed){++failures;Check(session->Current()==before&&audio->Handles().empty()&&volumes->Snapshot().settings==std::array{5,5,5}&&seed==23,"Failed Audio options constructor published partial effects");}else{done=true;owner->Release();}
 }
 Check(done&&failures>=4,"Audio options allocation sweep did not cover partial ownership");
 auto active=Session();FrontendAudioOptions owner(active,input,audio,volumes,seed);for(unsigned i=0;owner.Status().state==0&&i<100;++i)owner.AdvanceVisual(owner.Current(),1.f/60);Ack(owner);
 auto old=owner.Current();bool wrong=false;std::thread other([&]{try{owner.AdvanceVisual(old,0);}catch(...){wrong=true;}});other.join();Check(wrong&&owner.Current()==old,"Wrong-thread audio visual update mutated state");
 Reject([&]{owner.Acknowledge(before,{1,640,480,640,480,0,0,640,480});});Check(owner.Current()==old,"Stale presentation mutated owner");
 audio->Enable(false);Reject([&]{owner.DeliverPointer(old,{0,Center(owner.Bounds()[0]),true});});Check(owner.Status().failed&&active->Current()==old&&volumes->Snapshot().settings==std::array{5,5,5},"Rejected real audio callback published visual/setting mutation");Reject([&]{owner.Current();});owner.Release();audio->Unload();
}
void Music(bool owned)
{
 auto profile=ReadAudioVolumeProfile(owned?Load("/audio/nlxgs.bun"):Calculation());auto volumes=std::make_shared<AudioCategoryVolumes>(profile,std::array{0,10,10});AudioBankCatalog::Handle catalog;
 if(owned)catalog=ReadAudioBankCatalog(Load("/audio/nlxgs.bun"));else{auto c=std::make_shared<AudioBankCatalog>();c->names.resize(27);c->slots.resize(23);c->names[26]={26,"FE_GEN_Music"};c->slots[22]={22,0,0,true};catalog=c;}
 FrontendMusicOptions options;options.category_volumes=volumes;FrontendMusic music(catalog,volumes->Initial(),options);unsigned seed=1;music.BeginSelect(0,seed);
 const auto pump=[&]{const auto end=std::chrono::steady_clock::now()+std::chrono::seconds(15);while(music.Status().load==FrontendMusicLoadState::Loading){music.Service();Check(std::chrono::steady_clock::now()<end,"Music gain preparation timed out");SDL_Delay(1);}music.Check();};pump();
 Check(music.Status().input_gain==0,"Initially muted music retains input gain");const float base=-6.f; // Independently read Title voice: synthetic index0/owned index4.
 std::uint64_t clock=0;for(unsigned level:{10u,4u,0u,8u}){volumes->Set(AudioCategory::Music,level);volumes->Update(++clock,0);music.Poll();const auto status=music.Status();const auto expected=std::clamp(levels[level]+base,-96.f,6.f);Check(status.volume_db==expected&&std::abs(status.input_gain-float(Gain(expected))/32768)<=1.f/32768,"Current streamed source did not use actual category gain");}
 music.BeginSelect(1,seed);pump();Check(music.Status().input_gain>0,"Future music selection lost live settings");Check(volumes->Snapshot().frame!=0&&seed==1,"Music consumed category clock or RNG");music.Unload();
}
void DiskGain(const std::filesystem::path& folder)
{
 SDL_SetHint(SDL_HINT_AUDIO_DRIVER,"disk");const auto path=folder/"gain-output.raw";SDL_SetHint(SDL_HINT_AUDIO_DISK_OUTPUT_FILE,path.string().c_str());SDL_SetHint(SDL_HINT_AUDIO_FORMAT,"F32");SDL_SetHint(SDL_HINT_AUDIO_FREQUENCY,"44100");SDL_SetHint(SDL_HINT_AUDIO_CHANNELS,"2");
 auto f=Make();for(unsigned i=0;i<5;++i)Put(f.bytes,f.sp+8+i*28,44100);auto bank=ReadAudioResidentBank(f.bytes,f.wave);AudioBankSelection choose(bank);unsigned seed=1;auto selected=*choose.Select({0x10203040,0,0,0},seed);auto buffer=PrepareResidentAudio(selected,ReadAudioCalculationInitial(Calculation()),-96);
 Check(std::all_of(buffer->Stereo().begin(),buffer->Stereo().end(),[](float v){return v==0;}),"Initially muted fixture unexpectedly contains output");
 auto wanted=buffer->BeforeInputGain();for(auto& v:wanted)v*=32767.f/32768;Check(std::any_of(wanted.begin(),wanted.end(),[](float v){return v!=0;}),"Retained pre-gain fixture contains no signal");
 ResidentAudioOutput output(buffer,0,true);Check(output.Status().input_gain==0,"Muted queue starts with nonzero gain");output.SetVolume(0);output.Start();const auto until=std::chrono::steady_clock::now()+std::chrono::seconds(3);
 while(output.Status().state!=ResidentAudioState::InputConsumed){Check(std::chrono::steady_clock::now()<until,"Disk output did not consume unmuted queue");SDL_Delay(2);}SDL_Delay(60);output.Stop();
 std::ifstream file(path,std::ios::binary);Data bytes(std::istreambuf_iterator<char>(file),{});const auto* first=reinterpret_cast<const std::uint8_t*>(wanted.data());Check(std::search(bytes.begin(),bytes.end(),first,first+wanted.size()*sizeof(float))!=bytes.end(),"Real SDL disk output did not recover queued muted PCM");std::cout<<checks<<" actual queued-gain disk checks passed\n";
}
struct Host{bool live=false,disc=false;~Host(){if(live)ResetStartupFiles();if(disc)aurora_dvd_close();if(live){ResetStartupMemory();aurora_shutdown();}}};
}
int main(int argc,char** argv)
{
 try{
 if(argc==3&&std::string_view(argv[1])=="--gain-disk"){DiskGain(argv[2]);return 0;}
 if(argc==3&&std::string_view(argv[1])=="--fixture"){auto f=frontend_music_fixture::Make();SaveFile(std::filesystem::path(argv[2])/"music.resbun",f.metadata);SaveFile(std::filesystem::path(argv[2])/"music.nlxwb",f.wave);return 0;}
 Check(argc==4,"Supply disc/output/generated or owned");const bool owned=std::string_view(argv[3])=="owned";
 const auto folder=(std::filesystem::path(argv[2])/"audio-options-host").string();std::filesystem::create_directories(folder);AuroraConfig config{};config.appName="Charged original Audio options";config.userPath=config.cachePath=folder.c_str();config.resourcesPath=SDL_GetBasePath();config.desiredBackend=BACKEND_NULL;config.windowWidth=320;config.windowHeight=240;config.windowPosX=config.windowPosY=-1;config.mem1Size=MEM1_DEFAULT_SIZE;config.mem2Size=64*1024*1024;config.logLevel=LOG_WARNING;
 Host host;auto result=aurora_initialize(argc,argv,&config);host.live=true;Check(result.window,"Aurora failed");InitializeStartupOS();nlInitMemory();Check(aurora_dvd_open(argv[1]),"Audio options disc absent");host.disc=true;nlInitFileSystem();
 const auto a=StandardAllocator.TotalFreeMemory(),b=VirtualAllocator.TotalFreeMemory();Categories();QueueGains();for(unsigned i=0;i<2;++i)Menu(owned);if(!owned)FailureOwnership();Music(owned);Check(!nlAsyncReadsPending(nullptr)&&a==StandardAllocator.TotalFreeMemory()&&b==VirtualAllocator.TotalFreeMemory(),"Audio options did not recover arenas/reads");std::cout<<checks<<" Audio options/live gain checks passed\n";
 }catch(const std::exception& e){allocation_budget=-1;std::cerr<<"FAILED: "<<e.what()<<" (check "<<checks<<")\n";return 1;}
}
