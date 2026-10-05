#include "runtime/frontend_visual_options.h"
#include "runtime/frontend_handler.h"
#include "runtime/frontend_visual_navigation.h"
#include "runtime/frontend_stack.h"
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
#include <fstream>
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
 const std::array keys{0x96deb5c3u,0x3021a1eeu,0xf6eb899eu,0x362f2841u,0x304fdd1eu,0xf0afd586u,0xaccdca48u,0x6f6a3a07u,0xaa73ef33u};Data root;
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

FrontendStackRequest Request(){FrontendStackRequest r;r.scene=15;r.initial_slide="OPTIONS_IN";r.resources_mode=FrontendSessionResourcesMode::PermanentMain;return r;}
void Pump(FrontendSceneStack& stack,FrontendSceneStack::Token token)
{const auto end=std::chrono::steady_clock::now()+std::chrono::seconds(20);while(stack.Entry(token).state==FrontendStackState::Queued||stack.Entry(token).state==FrontendStackState::Loading){stack.Service();Check(std::chrono::steady_clock::now()<end,"Visual NAV source load timed out");SDL_Delay(1);}stack.RethrowFailure(token);}
void Pump(FrontendSession& s)
{const auto end=std::chrono::steady_clock::now()+std::chrono::seconds(20);while(s.State()==FrontendSessionState::Loading){s.Service();Check(std::chrono::steady_clock::now()<end,"NAV source load timed out");SDL_Delay(1);}s.Result();}
void Pump(NativePreferences& p)
{const auto end=std::chrono::steady_clock::now()+std::chrono::seconds(10);while(p.Status().host_pending){p.Poll();Check(std::chrono::steady_clock::now()<end,"Native visual save timed out");SDL_Delay(1);}p.RethrowFailure();}
auto Preferences(const std::filesystem::path& folder,unsigned number)
{
 const auto path=std::filesystem::absolute(folder/("visual-navigation"+std::to_string(number)+".bin"));auto values=DefaultNativePreferences();values.audio={2,3,7};values.audio_defaults={9,8,6};values.auto_zoom=true;values.camera_zoom=.61f;
 const auto bytes=EncodeNativePreferences(values);{std::ofstream f(path,std::ios::binary);f.write(reinterpret_cast<const char*>(bytes.data()),bytes.size());Check(bool(f),"Preference fixture write failed");}
 auto p=std::make_shared<NativePreferences>(path);p->StartLoad();Pump(*p);return p;
}
FrontendPointerViewport Viewport(){return{1,640,480,640,480,0,0,640,480};}
void Input(FrontendInput& input){std::array<FrontendPadSample,4> samples{};for(auto& p:samples)p.connected=true;input.Update(samples,0);}
std::string DoneSlide(const FrontendSession::Handle& nav)
{const auto& node=Find(nav,{"Slide1","Layer","done"});auto value=Active(nav,node);for(auto& c:value)if(c>='A'&&c<='Z')c+='a'-'A';return value;}
void DoneLabel(const FrontendSession::Handle& nav)
{
 const auto expected=nav->visuals->localization->Get(FrontendLowerHash("OPTIONS_ACCEPT"));
 for(auto state:{"off","over","down"})
 {
  const auto& label=Find(nav,{"Slide1","Layer","done",state,"Group","done"});Check(label.type==3,"Done authored label is not text");
  Check(label.localization_hash==FrontendLowerHash("OPTIONS_ACCEPT"),"Original Done label ID differs");Check(!expected.empty(),"Done original localization is absent");
 }
}
void Lifecycle(bool owned,bool overlap,const std::filesystem::path& folder,unsigned iteration)
{
 FrontendInput input;Input(input);auto audio=Audio(owned);auto preferences=Preferences(folder,iteration);const auto original=*preferences->Current();auto settings=std::make_shared<FrontendVisualSettings>(original.auto_zoom,original.camera_zoom);unsigned seed=341,drains=0;
 FrontendSceneStack stack(input,[&]{++drains;});std::shared_ptr<FrontendVisualOptions> menu;std::shared_ptr<FrontendSession> menu_session;
 auto token=stack.QueuePush(Request());stack.BindVisual(token,[&](auto c){menu_session=c.session;menu=std::make_shared<FrontendVisualOptions>(c.session,input,audio,settings,seed,c.handler);return menu;});Pump(stack,token);
 auto nav_session=std::make_shared<FrontendSession>();nav_session->BeginShared({"/Art/fe/fe_overlay.fen",FrontendLanguage::English,FrontendImageProfile::Main,"Slide1",true},stack.Resources(token));Pump(*nav_session);
 auto nav=std::make_shared<FrontendNavigation>(nav_session,input,audio,seed);auto glue=std::make_unique<FrontendVisualNavigation>(menu,nav,input,audio,seed,preferences);
 Check(menu->Current()->images==nav->Current()->images&&menu->Current()->visuals==nav->Current()->visuals,"Menu/NAV do not retain one actual shared resource generation");Check(nav->Status().visible_buttons==0,"Visual creation did not hide NAV");
 auto publish=[&]{auto frame=stack.Entry(token).prepared;stack.Publish(token,frame);menu->Acknowledge(frame,Viewport());nav->Acknowledge(nav->Current(),Viewport());glue->Acknowledge(nav->Current());};publish();
 auto update=[&](float delta,auto&& action){stack.Update(delta,[&](auto id,const auto& shown){Check(id==token,"Unexpected Visual stack input token");glue->ApplyCommands();action(shown);glue->ApplyCommands();});stack.RethrowFailure(token);};
 for(unsigned n=0;menu->Status().state==0&&n<100;++n)
 {
  const auto hidden_nav=nav->Current();
  update(.125f,[&](const auto& shown){
   if(menu->Status().state==1)
   {
    glue->Deliver(shown,hidden_nav,{0,{0,-212},false});
    const auto state=glue->DoneStatus();
    Check(state.pointer_states[0]==0&&state.hover_feedback_requests==0&&audio->ActiveCount(0xaa73ef33)==0&&!preferences->Status().host_pending,
      "Newly shown Done received input before its first actual publication");
   }
  });
  nav->AdvanceVisual(nav->Current(),.125f);publish();
 }
 Check(menu->Status().state==1&&nav->Status().visible_buttons==0x24,"Original visual intro did not show Back/Done");DoneLabel(nav->Current());
 const auto bounds=glue->DoneBounds();Near(bounds.min_x,-84);Near(bounds.max_x,84);Near(bounds.min_y,-259);Near(bounds.max_y,-165);Check(glue->DoneStatus().bound,"Actual Done binding was not acknowledged");
 auto before=menu->Current(),nav_before=nav->Current();const auto initial_seed=seed;const auto initial_settings=settings->Snapshot();FrontendHandler locked(menu_session,input);locked.SetExclusiveInput(true);
 Check(!glue->Deliver(before,nav_before,{0,Center(bounds),true}).back_pressed,"Locked explicit event admitted NAV Back");
 stack.Update(.125f,[&](auto,const auto&){throw std::runtime_error("Locked Visual input callback ran");});stack.RethrowFailure(token);Check(menu->Current()==before&&nav->Current()==nav_before&&seed==initial_seed&&settings->Snapshot()==initial_settings&&!preferences->Status().host_pending,"Input lock admitted NAV/settings/save");locked.SetExclusiveInput(false);locked.Release();
 // Source Done bounds deliberately differ from its generated authored visual.
 // Hover/leave preserve the original per-pointer state and real cue ownership.
 const auto done=Center(bounds);FrontendNavigationDispatch dispatch;
 update(0,[&](const auto& shown){dispatch=glue->Deliver(shown,nav_before,{0,done,false});});Check(!dispatch.back_pressed&&glue->DoneStatus().pointer_states[0]==1&&DoneSlide(nav->Current())=="over"&&audio->ActiveCount(0xaa73ef33)==1,"Original Done enter/cue was not executed after visual listeners");
 Check(stack.Entry(token).published==before&&nav_before!=nav->Current(),"Done hover implicitly acknowledged NAV candidate");publish();
 nav_before=nav->Current();update(0,[&](const auto& shown){glue->Deliver(shown,nav_before,{1,done,false});});Check(glue->DoneStatus().pointer_states[1]==1&&audio->ActiveCount(0xaa73ef33)==1,"Second Done pointer replayed first hover cue");publish();
 nav_before=nav->Current();update(0,[&](const auto& shown){glue->Deliver(shown,nav_before,{0,{300,220},false});});Check(glue->DoneStatus().pointer_states[0]==0&&DoneSlide(nav->Current())=="over","Done leave ignored another inside pointer");publish();
 nav_before=nav->Current();update(0,[&](const auto& shown){glue->Deliver(shown,nav_before,{1,{300,220},false});});Check(glue->DoneStatus().pointer_states[1]==0&&DoneSlide(nav->Current())=="off","Last Done leave did not select off");publish();
 if(iteration==0)
 {
  // Return via the actual source Back owner; it must short-circuit Done/save.
  before=menu->Current();nav_before=nav->Current();const auto old_done=glue->DoneStatus();update(0,[&](const auto& shown){dispatch=glue->Deliver(shown,nav_before,{0,Center(nav->Bounds()),true});});
  Check(dispatch.back_pressed&&menu->Status().state==3&&settings->Snapshot().auto_zoom&&settings->Snapshot().zoom==.5f&&!preferences->Status().host_pending&&!glue->DoneStatus().pressed&&glue->DoneStatus().pointer_states==old_done.pointer_states,"Back did not precede/short-circuit Visual and Done source listeners");
  Check(nav->Status().visible_buttons==0&&audio->ActiveCount(0x6f6a3a07)==1,"Actual Back cue/hide service missing");publish();
 }
 else
 {
  if(!overlap){nav_before=nav->Current();update(0,[&](const auto& shown){glue->Deliver(shown,nav_before,{0,Center(menu->Bounds()[4]),true});});publish();}
  before=menu->Current();nav_before=nav->Current();const auto old_values=*preferences->Current();
  update(0,[&](const auto& shown){dispatch=glue->Deliver(shown,nav_before,{0,done,true});});
  Check(!dispatch.back_pressed&&menu->Status().state==3&&menu->Status().native_save_admitted&&settings->Snapshot().zoom==1&&preferences->Status().host_pending&&*preferences->Current()==old_values,"Done save did not observe the preceding same-event Visual setting");
  Check(glue->DoneStatus().pressed&&DoneSlide(nav->Current())=="down"&&nav->Status().visible_buttons==0&&stack.Entry(token).published==before,"Source Done feedback/save publication differs");
  Check(audio->ActiveCount(0xf0afd586)==1&&audio->ActiveCount(0x304fdd1e)==1,"Source visual save cues are absent");publish();
 }
 auto retained_menu=menu->Current(),retained_nav=nav->Current();auto feedback=glue->DoneStatus();Check(feedback.speaker_context==(iteration?1u:2u)&&feedback.hover_feedback_requests==(iteration?3u:2u),"Done speaker request invalid");
 glue->Release();glue.reset();stack.QueuePop(token);stack.Poll();Reject([&]{menu->Current();});nav->Release();nav.reset();nav_session.reset();menu_session.reset();stack.Release();
 if(iteration){Pump(*preferences);const auto saved=*preferences->Current();Check(saved.camera_zoom==1&&saved.auto_zoom&&saved.audio==original.audio&&saved.audio_defaults==original.audio_defaults&&!preferences->Status().original_normal_save_loaded&&!preferences->Status().full_save_complete,"Real Done save failed after owners were removed or claimed game-save readiness");}
 Check(retained_menu->visuals&&retained_nav->images&&audio->Handles().empty()&&drains>1,"Navigation teardown lost retained resources or real cues");audio->Unload();
}

void UnobservedSaveFailure(bool owned,const std::filesystem::path& folder)
{
 FrontendInput input;Input(input);auto audio=Audio(owned);auto settings=std::make_shared<FrontendVisualSettings>(true,.5f);unsigned seed=17;
 auto preferences=std::make_shared<NativePreferences>(std::filesystem::absolute(folder/"unobserved-visual.bin"));
 FrontendSceneStack stack(input,[]{});std::shared_ptr<FrontendVisualOptions> menu;
 auto token=stack.QueuePush(Request());stack.BindVisual(token,[&](auto c){menu=std::make_shared<FrontendVisualOptions>(c.session,input,audio,settings,seed,c.handler);return menu;});Pump(stack,token);
 auto nav_session=std::make_shared<FrontendSession>();nav_session->BeginShared({"/Art/fe/fe_overlay.fen",FrontendLanguage::English,FrontendImageProfile::Main,"Slide1",true},stack.Resources(token));Pump(*nav_session);
 auto nav=std::make_shared<FrontendNavigation>(nav_session,input,audio,seed);FrontendVisualNavigation glue(menu,nav,input,audio,seed,preferences);
 auto publish=[&]{auto f=stack.Entry(token).prepared;stack.Publish(token,f);menu->Acknowledge(f,Viewport());nav->Acknowledge(nav->Current(),Viewport());glue.Acknowledge(nav->Current());};publish();
 for(unsigned n=0;menu->Status().state==0&&n<100;++n){stack.Update(.125f,[&](auto,const auto&){glue.ApplyCommands();});stack.RethrowFailure(token);publish();}
 auto shown=menu->Current(),nav_shown=nav->Current();const auto value=settings->Snapshot();
 stack.Update(0,[&](auto,const auto& frame){glue.ApplyCommands();glue.Deliver(frame,nav_shown,{0,Center(glue.DoneBounds()),true});glue.ApplyCommands();});
 Reject([&]{stack.RethrowFailure(token);});Check(glue.Failed()&&stack.Entry(token).state==FrontendStackState::Failed&&stack.Entry(token).published==shown,"Unobserved native save published a successful scene");
 Check(preferences->Status().state==NativePreferencesState::Idle&&!preferences->Status().host_pending&&!menu->Status().native_save_admitted&&settings->Snapshot()==value,"Rejected real save changed persistence/settings admission");
 Check(audio->ActiveCount(0xf0afd586)==0&&audio->ActiveCount(0x304fdd1e)==0,"Rejected save played its later source cues");
 glue.Release();stack.QueuePop(token);stack.Poll();nav->Release();nav.reset();nav_session.reset();stack.Release();Check(audio->Handles().empty(),"Failed Done callback leaked actual output");audio->Unload();
}
struct Host{bool live=false,disc=false;~Host(){if(live)ResetStartupFiles();if(disc)aurora_dvd_close();if(live){ResetStartupMemory();aurora_shutdown();}}};
}
int main(int argc,char** argv)
{
 try
 {
  Check(argc==4,"Supply disc/output/generated or overlap or owned");const bool owned=std::string_view(argv[3])=="owned",overlap=std::string_view(argv[3])=="overlap";const auto output=std::filesystem::absolute(argv[2]);const auto folder=(output/"visual-navigation-host").string();std::filesystem::create_directories(folder);
  AuroraConfig config{};config.appName="Charged actual Visual NAV";config.userPath=config.cachePath=folder.c_str();config.resourcesPath=SDL_GetBasePath();config.desiredBackend=BACKEND_NULL;config.windowWidth=320;config.windowHeight=240;config.windowPosX=config.windowPosY=-1;config.mem1Size=MEM1_DEFAULT_SIZE;config.mem2Size=64*1024*1024;config.logLevel=LOG_WARNING;
  Host host;auto result=aurora_initialize(argc,argv,&config);host.live=true;Check(result.window,"Aurora init failed");InitializeStartupOS();nlInitMemory();Check(aurora_dvd_open(argv[1]),"Visual NAV disc absent");host.disc=true;nlInitFileSystem();
  for(unsigned i=0;i<2;++i){const auto a=StandardAllocator.TotalFreeMemory(),b=VirtualAllocator.TotalFreeMemory();Lifecycle(owned,overlap,output,i);if(i==0&&!overlap)UnobservedSaveFailure(owned,output);Check(!nlAsyncReadsPending(nullptr)&&a==StandardAllocator.TotalFreeMemory()&&b==VirtualAllocator.TotalFreeMemory(),"Visual NAV did not recover native reads/arenas");}
  std::cout<<checks<<" actual Visual NAV checks passed\n";
 }
 catch(const std::exception& e){allocation_budget=-1;std::cerr<<"FAILED: "<<e.what()<<" (check "<<checks<<")\n";return 1;}
}
