#include "runtime/frontend_visual_options.h"
#include "runtime/frontend_handler.h"
#include "runtime/startup.h"
#include "runtime/startup_files.h"
#include "audio_bank_fixture.h"
#include "NL/nlFileGC.h"
#include "NL/MemAlloc.h"
#include <aurora/aurora.h>
#include <aurora/dvd.h>
#include <SDL3/SDL.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <new>
#include <source_location>
#include <thread>
thread_local long allocation_budget=-1;
void* operator new(std::size_t n){if(allocation_budget==0)throw std::bad_alloc();if(allocation_budget>0)--allocation_budget;if(auto p=std::malloc(n?n:1))return p;throw std::bad_alloc();}
void* operator new[](std::size_t n){return ::operator new(n);}
void operator delete(void*p)noexcept{std::free(p);}void operator delete[](void*p)noexcept{std::free(p);}
void operator delete(void*p,std::size_t)noexcept{std::free(p);}void operator delete[](void*p,std::size_t)noexcept{std::free(p);}
using namespace mscharged;using namespace mscharged::resources;
namespace
{
unsigned checks=0;
void Check(bool v,const char* message){++checks;if(!v)throw std::runtime_error(message);}
template<class F>void Reject(F f,std::source_location p=std::source_location::current()){++checks;try{f();}catch(const std::exception&){return;}throw std::runtime_error("Invalid visual options accepted at "+std::to_string(p.line()));}
void Near(float a,float b){Check(std::isfinite(a)&&std::abs(a-b)<.001f,"Independent coordinate or setting differs");}
std::vector<std::uint8_t> Load(const char* path){std::unique_ptr<nlFile> f(nlOpen(path));Check(bool(f),"Owned NL input absent");auto n=nlFileSize(f.get(),nullptr);std::vector<std::uint8_t> b(n);nlRead(f.get(),b.data(),n,n);return b;}
using namespace audio_bank_fixture;
std::shared_ptr<FrontendAudio> Audio(bool owned,unsigned device=0)
{
 if(owned)
 {
  auto global=Load("/audio/nlxgs.bun");auto catalog=ReadAudioBankCatalog(global);AudioBankLoad owner(catalog,23,21);
  const auto end=std::chrono::steady_clock::now()+std::chrono::seconds(10);while(owner.State()==AudioBankLoadState::Loading){owner.Service();Check(std::chrono::steady_clock::now()<end,"NAV sound bank timed out");SDL_Delay(1);}
  return std::make_shared<FrontendAudio>(owner.Result(),ReadAudioCalculationInitial(global),[&]{AudioVoicesOptions options;options.capacity=32;options.device_id=device;return options;}());
 }
 auto f=Make();const auto word=[](const Data& d,std::size_t at){return (std::uint32_t(d.at(at))<<24)|(std::uint32_t(d.at(at+1))<<16)|(std::uint32_t(d.at(at+2))<<8)|d.at(at+3);};
 const auto chunks=[&](const Data& data){std::vector<std::pair<unsigned,Data>> out;for(std::size_t at=0;at<data.size();){const auto id=word(data,at),n=word(data,at+4);out.emplace_back(id,Data(data.begin()+at+8,data.begin()+at+8+n));at+=(n+11)&~3u;}return out;};
 const std::array keys{0x96deb5c3u,0x3021a1eeu,0xf6eb899eu,0x362f2841u,0x304fdd1eu,0xf0afd586u};Data root;
 for(auto [id,data]:chunks(Data(f.bytes.begin()+8,f.bytes.end())))
 {
  if(id==0x80023000)
  {
   Data map,records;Append(map,0x23001,Words({unsigned(keys.size()),0,0}));for(unsigned i=0;i<keys.size();++i){auto b=Words({keys[i],0,0,0,i});records.insert(records.end(),b.begin(),b.end());}Append(map,0x23003,records);data=std::move(map);
  }
  if(id==0x80023300)
  {
   Data graph;bool refs=false;
   for(auto [kind,part]:chunks(data))
   {
    if(kind==0x23301)Put(part,8,unsigned(keys.size()));
    if(kind==0x23302){Data rows;for(auto key:keys){auto row=Data(part.begin()+40,part.end());Put(row,0,key);rows.insert(rows.end(),row.begin(),row.end());}part=std::move(rows);}
    if(kind==0x23308){if(!refs){for(unsigned i=0;i<keys.size();++i)Append(graph,kind,Words({0xf0001000,0,Float(255),0,0}));refs=true;}continue;}
    Append(graph,kind,part);
   }
   data=std::move(graph);
  }
  Append(root,id,data);
 }
 auto bank=std::make_shared<const LoadedAudioBank>(LoadedAudioBank{23,21,{23,"FE_GEN_Sfx"},{21,0,1,false},ReadAudioResidentBank(Wrap(0x80000001,root),f.wave)});
 Data calc;Append(calc,0x23401,Words({2,0xf1000100,0}));Append(calc,0x23402,Words({0,0,0,0,0,0,1,0,0,0xf1000100,0,0}));
 return std::make_shared<FrontendAudio>(bank,ReadAudioCalculationInitial(Wrap(0x80000001,Wrap(0x80023400,calc))),[&]{AudioVoicesOptions options;options.capacity=32;options.device_id=device;return options;}());
}

auto Session(const char* path="/Art/fe/options_visual_options.fen")
{
 auto s=std::make_shared<FrontendSession>();s->Begin({path,FrontendLanguage::English,FrontendImageProfile::Main,"OPTIONS_IN",true});const auto end=std::chrono::steady_clock::now()+std::chrono::seconds(15);
 while(s->State()==FrontendSessionState::Loading){s->Service();Check(std::chrono::steady_clock::now()<end,"Visual options NL loading timed out");SDL_Delay(1);}s->Result();return s;
}
void Ack(FrontendVisualOptions& o){o.Acknowledge(o.Current(),{1,640,480,640,480,0,0,640,480});}
void Ready(FrontendVisualOptions& o){for(unsigned i=0;i<120&&o.Status().state==0;++i)o.AdvanceVisual(o.Current(),1.f/60);Check(o.Status().state==1&&o.Status().initialized,"Authored intro did not finish");Ack(o);}
auto Center(FrontendPointerBounds b){return std::array<float,2>{(b.min_x+b.max_x)/2,(b.min_y+b.max_y)/2};}
const FrontendInstance& Find(const FrontendSession::Handle& f,std::initializer_list<std::string_view> path)
{
 auto n=FindFrontendNode(f->graph,{},FrontendNamedPath(std::span(path.begin(),path.size())));Check(n&&n->kind==FrontendNodeKind::Instance,"Authored lookup is absent");
 const auto it=std::find_if(f->graph.instances.begin(),f->graph.instances.end(),[&](auto& i){return i.offset==n->id;});Check(it!=f->graph.instances.end(),"Authored instance absent");return *it;
}
std::string Active(const FrontendSession::Handle& f,const FrontendInstance& instance)
{
 Check(instance.library.has_value(),"Missing component library");const auto& all=f->graph.library;auto l=std::find_if(all.begin(),all.end(),[&](auto& x){return x.offset==*instance.library;});Check(l!=all.end()&&l->active_slide,"Missing selected component slide");
 auto s=std::find_if(f->graph.slides.begin(),f->graph.slides.end(),[&](auto& s){return s.offset==*l->active_slide;});Check(s!=f->graph.slides.end(),"Unresolved selected slide");return s->name;
}
void Label(const FrontendSession::Handle& f,int level)
{
 const auto& t=Find(f,{"OPTIONS_IN","Layer","visual_options","SERIES SETTING"});
 auto expected=std::u16string(f->visuals->localization->Get(FrontendLowerHash("OPTIONS_VISUAL_ZOOMLEVEL")));
 const auto at=expected.find(u"{0}");Check(at!=std::u16string::npos,"Independent localized format lacks placeholder");expected.replace(at,3,1,char16_t('0'+level));
 Check(t.text==expected,"Original zoom label differs from independent substitution");
}
void Authority()
{
 Reject([]{FrontendVisualSettings invalid(true,NAN);});Reject([]{FrontendVisualSettings invalid(true,-.01f);});Reject([]{FrontendVisualSettings invalid(true,1.01f);});
 FrontendVisualSettings v(true,.61f);auto old=v.Snapshot();v.Set(old,false,1);Check(!v.Snapshot().auto_zoom&&v.Snapshot().zoom==1,"Explicit visual settings assignment failed");Reject([&]{v.Set(old,true,.5f);});
 auto value=v.Snapshot();Reject([&]{v.Set(value,true,INFINITY);});Check(v.Snapshot()==value,"Invalid authority update changed settings");
 bool rejected=false;std::thread t([&]{try{v.Snapshot();}catch(...){rejected=true;}});t.join();Check(rejected,"Foreign visual authority accepted");
}
void Menu(bool owned)
{
 FrontendInput input;std::array<FrontendPadSample,4> pads{};pads[0].connected=true;input.Update(pads,0);
 auto audio=Audio(owned);auto session=Session();auto settings=std::make_shared<FrontendVisualSettings>(true,.61f);unsigned seed=17;
 FrontendVisualOptions owner(session,input,audio,settings,seed);Check(owner.Status().settings==std::array{0,2}&&owner.Status().backup==std::array{0,2},"Original setting truncation/backup differs");Near(settings->Snapshot().zoom,.61f);Label(owner.Current(),3);
 Check(!Find(owner.Current(),{"OPTIONS_IN","Layer","blackbox"}).visible&&!Find(owner.Current(),{"OPTIONS_OUT","Layer","blackbox"}).visible,"Main-mode blackboxes remain visible");
 Check(owner.Status().pointer_states[2]==std::array{2,2,2,2}&&owner.Status().pointer_states[5]==std::array{2,2,2,2},"Selected buttons did not reserve all four pointer states");
 Ack(owner);auto first=owner.Current();owner.DeliverPointer(first,{0,{0,0},true});Check(owner.Current()==first,"Intro admitted pointer mutation");
 FrontendHandler lock(session,input);lock.SetExclusiveInput(true);owner.AdvanceVisual(first,.1f);Check(owner.Current()==first,"Lock did not return before base advancement");lock.SetExclusiveInput(false);lock.Release();
 Ready(owner);const auto bounds=owner.Bounds();
 for(unsigned i=0;i<7;++i){Check(bounds[i].max_x>bounds[i].min_x&&bounds[i].max_y>bounds[i].min_y,"Original measured button is empty");std::cout<<"bound "<<i<<' '<<bounds[i].min_x<<' '<<bounds[i].max_x<<' '<<bounds[i].min_y<<' '<<bounds[i].max_y<<'\n';}
 if(!owned){for(unsigned i=0;i<5;++i){Near(bounds[i].min_x,-130.f+float(i)*60);Near(bounds[i].max_x,-90.f+float(i)*60);Near(bounds[i].min_y,20);Near(bounds[i].max_y,60);}Near(bounds[5].min_x,-120);Near(bounds[5].max_x,-80);Near(bounds[5].min_y,-80);Near(bounds[5].max_y,-40);Near(bounds[6].min_x,80);Near(bounds[6].max_x,120);}
 auto click=[&](unsigned item,unsigned index=0)
 {
  owner.DeliverPointer(owner.Current(),{index,Center(bounds[item]),true});Ack(owner);owner.DeliverPointer(owner.Current(),{index,{-999,-999}});Ack(owner);
  audio->Update(0);audio->ServiceAudio();audio->Update(0);
 };
 auto before=owner.Current();const auto old_seed=seed;const auto old_handles=audio->Handles().size();click(2);Check(seed==old_seed&&audio->Handles().size()==old_handles&&settings->Snapshot().revision==0,"Selected level consumed sound/RNG/settings");
 click(6);Check(!settings->Snapshot().auto_zoom&&owner.Status().settings[0]==1,"Manual mode did not apply source bool");Near(settings->Snapshot().zoom,.61f);Label(owner.Current(),3);
 click(5);Check(settings->Snapshot().auto_zoom,"Auto mode did not restore source bool");
 for(unsigned level=0;level<5;++level)
 {
  const auto previous=owner.Current();const int old_level=owner.Status().settings[1];click(level,level%4);Near(settings->Snapshot().zoom,float(level)/4);Check(owner.Status().settings[1]==int(level),"Source selected level differs");Label(owner.Current(),level+1);Label(previous,old_level+1);
  Check(owner.Status().pointer_states[level]==std::array{2,2,2,2},"Selected level lost four-pointer reservation");
  const auto& b=Find(owner.Current(),{"OPTIONS_IN","Layer","visual_options","ZOOM LEVELS","BUTTON_"+std::to_string(level)});auto active=Active(owner.Current(),b);for(auto& c:active)if(c>='A'&&c<='Z')c+='a'-'A';Check(active=="down","Selected level did not choose authored down slide");
 }
 auto current=owner.Current();Reject([&]{owner.DeliverPointer(before,{0,{0,0},true});});Reject([&]{owner.Save(current);});Check(owner.Current()==current&&!owner.Status().failed,"Unavailable full save changed visible state");
 owner.NotifyBackButton(current);Check(owner.Status().state==3&&settings->Snapshot().auto_zoom,"Back did not restore auto setting");Near(settings->Snapshot().zoom,.5f); // Original backup index quantizes .61.
 bool push=false;for(unsigned i=0;i<120&&!push;++i){owner.AdvanceVisual(owner.Current(),1.f/60);for(auto c:owner.Status().commands)if(c.kind==FrontendVisualOptionsCommandKind::PushOptions){Check(c.argument==13,"Visual return scene differs");push=true;}}Check(push,"Original out clock did not request Options13");
 auto retained=owner.Current();owner.Release();owner.Release();Check(audio->Handles().empty()&&retained->visuals&&retained->images,"Teardown did not retain assets/cancel own cues");Reject([&]{owner.Current();});audio->Unload();
}
void SaveAndFailures(bool owned,const std::filesystem::path& output)
{
 FrontendInput input;std::array<FrontendPadSample,4> pads{};pads[0].connected=true;input.Update(pads,0);unsigned seed=31;
 auto audio=Audio(owned);auto settings=std::make_shared<FrontendVisualSettings>(false,.75f);auto session=Session();FrontendVisualOptions owner(session,input,audio,settings,seed);Ready(owner);
 auto preferences=std::make_shared<NativePreferences>(output/(owned?"visual-owned.pref":"visual-generated.pref"));preferences->StartLoad();const auto until=std::chrono::steady_clock::now()+std::chrono::seconds(5);while(preferences->Status().host_pending){preferences->Poll();Check(std::chrono::steady_clock::now()<until,"Preferences load timed out");SDL_Delay(1);}
 const auto preserved_audio=preferences->Current()->audio;auto previous=owner.Current();owner.SaveNativePreferences(previous,preferences);
 Check(owner.Status().native_save_admitted&&owner.Status().state==3&&preferences->Status().host_pending&&!preferences->Status().full_save_complete,"Native save admission claimed wrong scope or state");
 owner.Release();while(preferences->Status().host_pending){preferences->Poll();Check(std::chrono::steady_clock::now()<until,"Preferences write timed out");SDL_Delay(1);}
 Check(preferences->Current()->audio==preserved_audio&&!preferences->Current()->auto_zoom&&preferences->Current()->camera_zoom==.75f,"Actual native persistence lost visual/audio fields");audio->Unload();
 auto a=Audio(owned);auto s=Session();auto explicit_settings=std::make_shared<FrontendVisualSettings>(true,.5f);FrontendVisualOptions foreign(s,input,a,explicit_settings,seed);Ready(foreign);auto frame=foreign.Current();explicit_settings->Set(explicit_settings->Snapshot(),false,1);Reject([&]{foreign.AdvanceVisual(frame,0);});Check(s->Current()==frame,"Foreign settings mutation changed retained frame");foreign.Release();a->Unload();
 if(!owned)
 {
  auto missing=Session("/Art/fe/visual-missing.fen");auto b=Audio(false);auto before=missing->Current();Reject([&]{FrontendVisualOptions bad(missing,input,b,settings,seed);});Check(missing->Current()==before,"Failed authored lookup published partial setup");b->Unload();
  unsigned failed=0,succeeded=0;for(long budget:{0,1,8,32,128,512,2048,8192})
  {
   auto x=Session();auto b=Audio(false);auto before=x->Current();auto authority=std::make_shared<FrontendVisualSettings>(true,.5f);
   try{allocation_budget=budget;FrontendVisualOptions attempt(x,input,b,authority,seed);allocation_budget=-1;++succeeded;attempt.Release();}
   catch(const std::bad_alloc&){allocation_budget=-1;++failed;Check(x->Current()==before&&authority->Snapshot().revision==0,"Constructor OOM changed frame/settings");}
   allocation_budget=-1;b->Unload();
  }Check(failed&&succeeded,"Visual constructor allocation sweep missed success/failure");
  auto mutation_session=Session();auto mutation_audio=Audio(false);auto mutation_settings=std::make_shared<FrontendVisualSettings>(true,.5f);
  FrontendVisualOptions mutation(mutation_session,input,mutation_audio,mutation_settings,seed);Ready(mutation);
  const auto unchanged=mutation.Current();const auto prior_settings=mutation_settings->Snapshot();const auto prior_seed=seed;
  try{allocation_budget=0;mutation.DeliverPointer(unchanged,{0,Center(mutation.Bounds()[0]),true});allocation_budget=-1;throw std::logic_error("Mutation allocation failure was not reached");}
  catch(const std::bad_alloc&){allocation_budget=-1;Check(mutation_session->Current()==unchanged&&mutation_settings->Snapshot()==prior_settings&&seed==prior_seed,"Mutation OOM exposed frame/settings/audio effects");}
  Check(mutation.Status().failed,"Mutation failure did not require explicit teardown");mutation.Release();Check(mutation_audio->Handles().empty(),"Failed mutation leaked real cue owner");mutation_audio->Unload();
 }
}
struct Host{bool live=false,disc=false;~Host(){if(live)ResetStartupFiles();if(disc)aurora_dvd_close();if(live){ResetStartupMemory();aurora_shutdown();}}};
}
int main(int argc,char** argv)
{
 try
 {
  Check(argc==4,"Supply disc/output/generated or owned");const bool owned=std::string_view(argv[3])=="owned";const auto output=std::filesystem::absolute(argv[2]);const auto folder=(output/"visual-options-host").string();std::filesystem::create_directories(folder);
  AuroraConfig config{};config.appName="Charged original visual options";config.userPath=config.cachePath=folder.c_str();config.resourcesPath=SDL_GetBasePath();config.desiredBackend=BACKEND_NULL;config.windowWidth=320;config.windowHeight=240;config.windowPosX=config.windowPosY=-1;config.mem1Size=MEM1_DEFAULT_SIZE;config.mem2Size=64*1024*1024;config.logLevel=LOG_WARNING;
  Host host;auto result=aurora_initialize(argc,argv,&config);host.live=true;Check(result.window,"Aurora init failed");InitializeStartupOS();nlInitMemory();Check(aurora_dvd_open(argv[1]),"Visual options disc absent");host.disc=true;nlInitFileSystem();
  const auto a=StandardAllocator.TotalFreeMemory(),b=VirtualAllocator.TotalFreeMemory();Authority();Menu(owned);SaveAndFailures(owned,output);Check(!nlAsyncReadsPending(nullptr)&&a==StandardAllocator.TotalFreeMemory()&&b==VirtualAllocator.TotalFreeMemory(),"Visual options did not recover native reads/arenas");std::cout<<checks<<" original visual options checks passed\n";
 }
 catch(const std::exception& e){allocation_budget=-1;std::cerr<<"FAILED: "<<e.what()<<" (check "<<checks<<")\n";return 1;}
}
