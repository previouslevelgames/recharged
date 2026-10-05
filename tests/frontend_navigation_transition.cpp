#include "runtime/frontend_navigation.h"
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
void Check(bool value,const char* message){++checks;if(!value)throw std::runtime_error(message);}
template<class F>void Reject(F f,std::source_location at=std::source_location::current()){++checks;try{f();}catch(const std::exception&){return;}throw std::runtime_error("Invalid NAV operation accepted at "+std::to_string(at.line()));}
void Near(float a,float b){Check(std::isfinite(a)&&std::abs(a-b)<.0001f,"Independent NAV bounds differ");}
std::vector<std::uint8_t> Load(const char* path)
{std::unique_ptr<nlFile> file(nlOpen(path));Check(bool(file),"Missing NAV input");const auto n=nlFileSize(file.get(),nullptr);std::vector<std::uint8_t> out(n);nlRead(file.get(),out.data(),n,n);return out;}
void Pump(FrontendSession& session)
{const auto end=std::chrono::steady_clock::now()+std::chrono::seconds(15);while(session.State()==FrontendSessionState::Loading){session.Service();Check(std::chrono::steady_clock::now()<end,"NAV assets timed out");SDL_Delay(1);}session.Result();}
auto Session(const char* path="/Art/fe/fe_overlay.fen")
{auto owner=std::make_shared<FrontendSession>();owner->Begin({path,FrontendLanguage::English,FrontendImageProfile::Main,"Slide1",true});Pump(*owner);return owner;}
using namespace audio_bank_fixture;
std::shared_ptr<FrontendAudio> Audio(bool owned,unsigned device=0)
{
 if(owned)
 {
  auto global=Load("/audio/nlxgs.bun");auto catalog=ReadAudioBankCatalog(global);AudioBankLoad owner(catalog,23,21);
  const auto end=std::chrono::steady_clock::now()+std::chrono::seconds(10);while(owner.State()==AudioBankLoadState::Loading){owner.Service();Check(std::chrono::steady_clock::now()<end,"NAV sound bank timed out");SDL_Delay(1);}
  return std::make_shared<FrontendAudio>(owner.Result(),ReadAudioCalculationInitial(global),AudioVoicesOptions{32,64*1024*1024,device});
 }
 auto f=Make();const auto word=[](const Data& d,std::size_t at){return (std::uint32_t(d.at(at))<<24)|(std::uint32_t(d.at(at+1))<<16)|(std::uint32_t(d.at(at+2))<<8)|d.at(at+3);};
 const auto chunks=[&](const Data& data){std::vector<std::pair<unsigned,Data>> out;for(std::size_t at=0;at<data.size();){const auto id=word(data,at),n=word(data,at+4);out.emplace_back(id,Data(data.begin()+at+8,data.begin()+at+8+n));at+=(n+11)&~3u;}return out;};
 const std::array keys{0xaccdca48u,0x6f6a3a07u,0xaccdca48u+1,0x6f6a3a07u+1};Data root;
 for(auto [id,data]:chunks(Data(f.bytes.begin()+8,f.bytes.end())))
 {
  if(id==0x80023000)
  {
   Data map,records;Append(map,0x23001,Words({4,0,0}));for(unsigned i=0;i<keys.size();++i){auto b=Words({keys[i],0,0,0,i});records.insert(records.end(),b.begin(),b.end());}Append(map,0x23003,records);data=std::move(map);
  }
  if(id==0x80023300)
  {
   Data graph;bool refs=false;
   for(auto [kind,part]:chunks(data))
   {
    if(kind==0x23301)Put(part,8,4);
    if(kind==0x23302){Data rows;for(auto key:keys){auto row=Data(part.begin()+40,part.end());Put(row,0,key);rows.insert(rows.end(),row.begin(),row.end());}part=std::move(rows);}
    if(kind==0x23308){if(!refs){for(unsigned i=0;i<4;++i)Append(graph,kind,Words({0xf0001000,0,Float(255),0,0}));refs=true;}continue;}
    Append(graph,kind,part);
   }
   data=std::move(graph);
  }
  Append(root,id,data);
 }
 auto bank=std::make_shared<const LoadedAudioBank>(LoadedAudioBank{23,21,{23,"FE_GEN_Sfx"},{21,0,1,false},ReadAudioResidentBank(Wrap(0x80000001,root),f.wave)});
 Data calc;Append(calc,0x23401,Words({2,0xf1000100,0}));Append(calc,0x23402,Words({0,0,0,0,0,0,1,0,0,0xf1000100,0,0}));
 return std::make_shared<FrontendAudio>(bank,ReadAudioCalculationInitial(Wrap(0x80000001,Wrap(0x80023400,calc))),AudioVoicesOptions{32,64*1024*1024,device});
}
unsigned NextSeed(unsigned v){const auto a=v^0x1d872b41u,b=a^(a>>5);return b^a^(b<<27);}
void Input(FrontendInput& input,bool pressed=false){std::array<FrontendPadSample,4> pads{};for(auto& p:pads)p.connected=true;pads[0].buttons=pressed?0x100:0;input.Update(pads,1.f/60);}
FrontendPointerViewport Viewport(bool wide=false){auto out=FrontendPointerViewport{1,960,720,1920,1440,0,0,1920,1440};out.widescreen=wide;return out;}
void Ack(FrontendNavigation& owner){owner.Acknowledge(owner.Current(),Viewport(owner.Status().widescreen));}
const FrontendInstance& Instance(const FrontendSession::Handle& f,std::uint32_t id)
{auto it=std::find_if(f->graph.instances.begin(),f->graph.instances.end(),[&](const auto& v){return v.offset==id;});Check(it!=f->graph.instances.end(),"NAV instance absent");return *it;}
const FrontendLibraryObject& Library(const FrontendSession::Handle& f,std::uint32_t id)
{auto it=std::find_if(f->graph.library.begin(),f->graph.library.end(),[&](const auto& v){return v.offset==id;});Check(it!=f->graph.library.end(),"NAV library absent");return *it;}
std::uint32_t Find(const FrontendSession::Handle& f,std::initializer_list<std::string_view> path)
{auto node=FindFrontendNode(f->graph,{},FrontendNamedPath(std::span(path.begin(),path.size())));Check(node&&node->kind==FrontendNodeKind::Instance,"NAV source path absent");return node->id;}
std::string Active(const FrontendSession::Handle& f,std::uint32_t id)
{const auto& lib=Library(f,*Instance(f,id).library);const auto it=std::find_if(f->graph.slides.begin(),f->graph.slides.end(),[&](const auto& v){return v.offset==lib.active_slide;});Check(it!=f->graph.slides.end(),"NAV active slide absent");return it->name;}
std::string BackState(const FrontendNavigation& n)
{auto f=n.Current();return Active(f,Find(f,{"Slide1","Layer","back",n.Status().widescreen?"16:9":"4:3","back"}));}
struct CallbackProbe {FrontendNavigation& owner;bool called=false;};
void Callback(nlFile*,void*,unsigned,nlFileAsyncParam value)
{
 auto& p=*reinterpret_cast<CallbackProbe*>(value);auto f=p.owner.Current();p.called=true;
 Reject([&]{p.owner.AdvanceVisual(f,0);});Reject([&]{p.owner.SetButtons(f,4);});Reject([&]{p.owner.DeliverPointer(f,{0,{0,0}});});Reject([&]{p.owner.Release();});Check(p.owner.Current()==f,"Callback rejection changed NAV");
}
void Transitions(bool owned)
{
 FrontendInput input;Input(input);auto session=Session();auto audio=Audio(owned);unsigned seed=12;FrontendNavigation nav(session,input,audio,seed);
 const auto transition=Find(nav.Current(),{"Slide1","Layer","transition"});
 const auto time=[&](const FrontendSession::Handle& f,std::uint32_t component){const auto& object=Library(f,*Instance(f,component).library);auto it=std::find_if(f->graph.slides.begin(),f->graph.slides.end(),[&](const auto& v){return v.offset==object.active_slide;});Check(it!=f->graph.slides.end(),"Transition active slide is absent");return it->time;};
 const auto doors=[&]{std::array<std::uint32_t,8> out{};for(unsigned i=0;i<8;++i){const auto name="door_"+std::to_string(i+1);out[i]=Find(nav.Current(),{"Slide1","Layer","transition","Slide1","Group",name});}return out;}();
 nav.AdvanceVisual(nav.Current(),.2f);unsigned calls=0;FrontendSession::Handle at_callback;
 auto callback=[&](std::string_view function){++calls;Check(function=="TransitionMainMenuToOptions","Transition command changed");Check(!nav.Status().transition_pending&&nav.Status().transition_playing,"Callback source flag order differs");at_callback=nav.Current();unsigned images=0;for(const auto& entry:at_callback->layout.entries)if(const auto* image=std::get_if<FrontendLayoutImage>(&entry);image&&image->name=="doors")++images;if(images!=8){std::cerr<<"Door count="<<images<<" call="<<calls<<" parenttime="<<time(at_callback,transition)<<" hidden="<<at_callback->layout.hidden<<"\n";for(const auto& [reason,count]:at_callback->layout.unavailable)std::cerr<<reason<<"="<<count<<"\n";}Check(images==8,"Shown transition lost an authored door image");Check(Instance(at_callback,transition).visible,"Callback ran before source show or after hide");Reject([&]{nav.AdvanceVisual(at_callback,0);});Reject([&]{nav.StartTransition(at_callback,"bad",[](auto){});});Reject([&]{nav.Release();});};
 const auto prior=nav.Current();nav.StartTransition(prior,"TransitionMainMenuToOptions",callback);Check(nav.Status().transition_pending&&nav.Status().transition_playing&&calls==0,"StartTransition called script eagerly");
 Near(time(nav.Current(),transition),0);for(auto id:doors)Near(time(nav.Current(),id),0);Check(time(prior,transition)>.1f,"Transition reset test did not retain old clock");
 const auto pending=nav.Current();Reject([&]{nav.StartTransition(pending,"overlap",callback);});Check(nav.Current()==pending,"Rejected overlapping transition changed frame");
 nav.AdvanceVisual(nav.Current(),0);Check(calls==1&&!nav.Status().transition_pending,"NAV pending update did not invoke exactly once");Check(nav.Current()==at_callback,"Callback stage was not retained");
 float expected=0;unsigned steps=0;while(nav.Status().transition_playing&&steps<100){expected+=1.f/60;nav.AdvanceVisual(nav.Current(),1.f/60);++steps;Near(time(nav.Current(),transition),expected);Check(nav.Status().transition_playing==(expected<.6f),"Independent original inclusive endpoint differs");}
 Check(steps==36&&calls==1&&!Instance(nav.Current(),transition).visible,"NAV endpoint or one-shot callback differs");Check(Instance(at_callback,transition).visible,"Later hide mutated retained callback frame");
 Check(nav.Status().pointer_input_enabled,"NAV endpoint did not restore input flag");nav.AdvanceVisual(nav.Current(),0);Check(calls==1,"Idle NAV replayed transition callback");
 std::array<FrontendNavigationPointerSample,4> samples{};nav.UpdatePointers(nav.Current(),samples,true);nav.StartTransition(nav.Current(),"TransitionMainMenuToOptions",callback);nav.AdvanceVisual(nav.Current(),.6f);
 Check(calls==2&&!nav.Status().transition_playing&&!nav.Status().pointer_input_enabled&&Instance(at_callback,transition).visible&&!Instance(nav.Current(),transition).visible,"Same-step callback/hide or hidden input restore differs");
 // callback happens even when its first update already reaches the endpoint.
 nav.UpdatePointers(nav.Current(),samples,false);nav.StartTransition(nav.Current(),"TransitionMainMenuToOptions",callback);nav.AdvanceVisual(nav.Current(),std::nextafter(.6f,0.f));Check(nav.Status().transition_playing&&calls==3,"Below-endpoint update ended early");
 nav.AdvanceVisual(nav.Current(),.6f-std::nextafter(.6f,0.f));Check(!nav.Status().transition_playing&&calls==3,"Exact inclusive endpoint did not finish");
 bool drained=false;auto lifetime=std::shared_ptr<int>(new int(1),[&](int* value){drained=true;delete value;Reject([&]{nav.Release();});Reject([&]{nav.AdvanceVisual(nav.Current(),0);});});std::weak_ptr<int> weak=lifetime;nav.StartTransition(nav.Current(),"TransitionMainMenuToOptions",[lifetime,callback](auto name){callback(name);});lifetime.reset();Check(!weak.expired(),"Pending NAV lost callback ownership");auto retained=nav.Current();nav.Release();Check(weak.expired()&&drained,"NAV release retained callback owner");Check(calls==3&&retained->images&&retained->visuals,"Pending transition teardown invoked or lost owned resources");audio->Unload();
}
void TransitionFailures()
{
 FrontendInput input;Input(input);auto audio=Audio(false);unsigned seed=16;
 auto missing=Session("/Art/fe/nav-missing-door.fen");FrontendNavigation invalid(missing,input,audio,seed);const auto old=invalid.Current();Reject([&]{invalid.StartTransition(old,"TransitionMainMenuToOptions",[](auto){});});Check(invalid.Current()==old&&!invalid.Status().transition_playing,"Missing authored door partially reset source state");invalid.Release();
 auto session=Session();FrontendNavigation nav(session,input,audio,seed);const auto original=nav.Current();
 Reject([&]{nav.StartTransition(original,"",[](auto){});});Reject([&]{nav.StartTransition(original,"x",{});});
 unsigned failures=0;bool success=false;for(long budget=0;budget<100000&&!success;budget+=257)
 {allocation_budget=budget;bool failed=false;try{nav.StartTransition(original,"TransitionMainMenuToOptions",[](auto){});}catch(const std::bad_alloc&){failed=true;}allocation_budget=-1;if(failed){++failures;Check(nav.Current()==original&&!nav.Status().transition_playing,"Failed transition reset partially published");}else success=true;}
 Check(success&&failures>=3,"Transition OOM rollback was not reached");nav.Release();
 bool admitted=false;unsigned allocation_failures=0;
 for(long budget=0;budget<120000&&!admitted;budget+=997)
 {
  auto trial_session=Session();FrontendNavigation trial(trial_session,input,audio,seed);unsigned count=0;
  trial.StartTransition(trial.Current(),"TransitionMainMenuToOptions",[&](auto){++count;});
  allocation_budget=budget;bool failed=false;try{trial.AdvanceVisual(trial.Current(),.6f);}catch(const std::bad_alloc&){failed=true;}allocation_budget=-1;
  if(failed){++allocation_failures;Check(count==0,"Endpoint allocation failed after real script admission");}
  else{Check(count==1&&!trial.Status().transition_playing,"Endpoint callback/hide did not commit");admitted=true;}
  trial.Release();
 }
 Check(admitted&&allocation_failures>=3,"Endpoint allocation gate was not exercised");
 session=Session();FrontendNavigation throwing(session,input,audio,seed);unsigned calls=0;
 throwing.StartTransition(throwing.Current(),"TransitionMainMenuToOptions",[&](auto){++calls;throw std::runtime_error("Real command failed");});Reject([&]{throwing.AdvanceVisual(throwing.Current(),.6f);});Check(calls==1&&throwing.Status().failed&&!throwing.Status().transition_pending&&throwing.Status().transition_playing,"Failed callback hid, replayed or forgot source pending state");
 Reject([&]{throwing.AdvanceVisual(session->Current(),0);});throwing.Release();Check(calls==1,"Failed callback replayed during release");
 session=Session();FrontendNavigation foreign(session,input,audio,seed);foreign.StartTransition(foreign.Current(),"TransitionMainMenuToOptions",[&](auto){session->Advance(0);});Reject([&]{foreign.AdvanceVisual(foreign.Current(),.6f);});Check(foreign.Status().failed,"Callback session mutation escaped generation check");foreign.Release();audio->Unload();
}
struct Host{bool live=false,disc=false;~Host(){if(live)ResetStartupFiles();if(disc)aurora_dvd_close();if(live){ResetStartupMemory();aurora_shutdown();}}};
}
int main(int argc,char** argv)
{
 try
 {
  Check(argc==4,"Supply disc/output/generated or owned");const bool owned=std::string_view(argv[3])=="owned";const auto folder=(std::filesystem::path(argv[2])/"navigation-transition-host").string();std::filesystem::create_directories(folder);AuroraConfig config{};
  config.appName="Charged original NAV transition";config.userPath=config.cachePath=folder.c_str();config.resourcesPath=SDL_GetBasePath();config.desiredBackend=BACKEND_NULL;config.windowWidth=320;config.windowHeight=240;config.windowPosX=config.windowPosY=-1;config.mem1Size=MEM1_DEFAULT_SIZE;config.mem2Size=64*1024*1024;config.logLevel=LOG_WARNING;
  Host host;const auto h=aurora_initialize(argc,argv,&config);host.live=true;Check(h.window,"Aurora failed");InitializeStartupOS();nlInitMemory();Check(aurora_dvd_open(argv[1]),"NAV disc missing");host.disc=true;nlInitFileSystem();
  for(unsigned i=0;i<3;++i){const auto a=StandardAllocator.TotalFreeMemory(),b=VirtualAllocator.TotalFreeMemory();Transitions(owned);if(!owned&&i==0)TransitionFailures();Check(!nlAsyncReadsPending(nullptr)&&a==StandardAllocator.TotalFreeMemory()&&b==VirtualAllocator.TotalFreeMemory(),"NAV transition teardown did not restore arenas/files");}
  std::cout<<checks<<" original NAV transition checks passed; real caller owns script service\n";
 }
 catch(const std::exception& e){allocation_budget=-1;std::cerr<<"FAILED: "<<e.what()<<" (check "<<checks<<")\n";return 1;}
}
