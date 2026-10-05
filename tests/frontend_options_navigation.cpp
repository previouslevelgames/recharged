#include "runtime/frontend_options_navigation.h"
#include "runtime/frontend_options.h"
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
template<class F>void Reject(F f,std::source_location at=std::source_location::current()){++checks;try{f();}catch(const std::exception&){return;}throw std::runtime_error("Invalid Options/navigation operation accepted at "+std::to_string(at.line()));}
void Near(float a,float b,std::source_location at=std::source_location::current()){++checks;if(!std::isfinite(a)||std::abs(a-b)>=.0001f)throw std::runtime_error("Independent clock differs at "+std::to_string(at.line())+": "+std::to_string(a)+" vs "+std::to_string(b));}
std::vector<std::uint8_t> Load(const char* path)
{std::unique_ptr<nlFile> file(nlOpen(path));Check(bool(file),"Missing NAV input");const auto n=nlFileSize(file.get(),nullptr);std::vector<std::uint8_t> out(n);nlRead(file.get(),out.data(),n,n);return out;}
void Pump(FrontendSession& session)
{const auto end=std::chrono::steady_clock::now()+std::chrono::seconds(15);while(session.State()==FrontendSessionState::Loading){session.Service();Check(std::chrono::steady_clock::now()<end,"NAV assets timed out");SDL_Delay(1);}session.Result();}
auto Session(const char* path="/Art/fe/main_menu_v3.fen")
{auto owner=std::make_shared<FrontendSession>();owner->Begin({path,FrontendLanguage::English,FrontendImageProfile::Main,"MAIN",true});Pump(*owner);return owner;}
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
 const std::array keys{0x6b0689d4u,0x0a93e9a0u,0xf0afd586u,0x304fdd1eu,0xf6eb899eu,0x4430b152u,0xaccdca48u,0x6f6a3a07u};Data root;
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
 return std::make_shared<FrontendAudio>(bank,ReadAudioCalculationInitial(Wrap(0x80000001,Wrap(0x80023400,calc))),AudioVoicesOptions{32,64*1024*1024,device});
}
void Input(FrontendInput& input)
{std::array<FrontendPadSample,4> pads{};for(auto& p:pads)p.connected=true;input.Update(pads,1.f/60);}
FrontendPointerViewport Viewport(){return {1,960,720,1920,1440,0,0,1920,1440};}
std::array<float,2> Center(FrontendPointerBounds b){return {(b.min_x+b.max_x)/2,(b.min_y+b.max_y)/2};}
FrontendStackRequest Request(unsigned scene)
{FrontendStackRequest r;r.scene=scene;r.initial_slide=scene==1?"MAIN":"in";return r;}
void Pump(FrontendSceneStack& stack,FrontendSceneStack::Token token)
{
 const auto end=std::chrono::steady_clock::now()+std::chrono::seconds(15);
 while(stack.Entry(token).state==FrontendStackState::Queued||stack.Entry(token).state==FrontendStackState::Loading)
 {stack.Service();Check(std::chrono::steady_clock::now()<end,"Visual stack loading timed out");SDL_Delay(1);}
 stack.RethrowFailure(token);
}
float ExpectedTime(const FrontendSession::Handle& frame,float delta)
{
 const auto& graph=frame->graph;const auto slide=std::find_if(graph.slides.begin(),graph.slides.end(),[&](const auto& x){return x.offset==graph.active_slide;});Check(slide!=graph.slides.end(),"Oracle requires active slide");
 const float end=slide->start+slide->duration;float next=graph.presentation_time+delta;
 if(slide->play_mode==0)return std::min(next,end);
 if(slide->play_mode==1&&next>end)next-=end;
 return next;
}
void Lifecycle(bool owned,bool back)
{
 FrontendInput input;Input(input);auto audio=Audio(owned);unsigned seed=17,drains=0,music_calls=0;
 FrontendSceneStack stack(input,[&]{++drains;});auto request=Request(13);request.resources_mode=FrontendSessionResourcesMode::PermanentMain;
 const auto token=stack.QueuePush(request);std::shared_ptr<FrontendOptions> options;
 stack.BindVisual(token,[&](auto c){options=std::make_shared<FrontendOptions>(c.session,input,audio,seed,c.handler,0);return options;});Pump(stack,token);
 Check(stack.Entry(token).state==FrontendStackState::AwaitingPublication&&!stack.Entry(token).full_scene_created,"Selected Options claimed full scene creation");
 auto nav_session=std::make_shared<FrontendSession>();nav_session->BeginShared({"/Art/fe/fe_overlay.fen",FrontendLanguage::English,FrontendImageProfile::Main,"Slide1",true},stack.Resources(token));Pump(*nav_session);
 FrontendNavigation nav(nav_session,input,audio,seed);
 FrontendOptionsNavigation control(*options,nav,[&](unsigned index){Check(index==1,"Original Options music request differs");++music_calls;});
 Check(control.BackBound()&&music_calls==1,"Original Options setup commands were not admitted");control.ApplyCommands();Check(music_calls==1,"Options setup commands replayed");
 Check(options->Current()->visuals==nav.Current()->visuals&&options->Current()->images==nav.Current()->images,"Composed scenes registered separate resources");
 FrontendSession::Handle menu_shown,nav_shown;
 const auto publish=[&]{menu_shown=stack.Entry(token).prepared;stack.Publish(token,menu_shown);options->Acknowledge(menu_shown,Viewport());nav_shown=nav.Current();nav.Acknowledge(nav_shown,Viewport());};
 publish();Reject([&]{control.Route(menu_shown,nav_shown,{});});
 bool accept=false;
 const auto update=[&](float dt,const std::function<void(const FrontendSession::Handle&)>& route={}){
  std::array<FrontendPadSample,4> pads{};for(auto& pad:pads)pad.connected=true;pads[0].buttons=accept?0x100:0;input.Update(pads,1.f/60);const auto before=options->Current();
  stack.Update(dt,[&](auto id,const auto& shown){Check(id==token&&shown==menu_shown,"Stack supplied unpresented Options input");control.ApplyCommands();nav.AdvanceVisual(nav.Current(),dt);if(route)route(shown);control.ApplyCommands();});
  stack.RethrowFailure(token);Check(!control.Failed()&&!nav.Status().failed,"Composed update lost retained owner");
  Check(menu_shown==before&&stack.Entry(token).published==menu_shown,"Input fabricated a displayed frame");
  publish();
 };
 for(unsigned i=0;i<80&&!options->Status().initialized;++i)update(1.f/60);
 Check(options->Status().initialized&&options->Status().state==1&&nav.Status().visible_buttons==4,"Original intro did not activate NAV Back");
 std::uint64_t sequence=0;FrontendPointerDesktopSample sample{1,960,720,1920,1440,0,42,480,360,true,true};
 const auto point=[&](std::array<float,2> xy){sample.x=(xy[0]+320)/640*960;sample.y=(240-xy[1])/480*720;};
 const auto send=[&](bool down){sample.sequence=++sequence;accept=back&&down;sample.primary_down=!back&&down;FrontendNavigationDispatch result;update(1.f/60,[&](const auto& shown){result=control.Route(shown,nav_shown,sample);});return result;};
 point({0,-200});send(false);send(false);
 const auto target=back?Center(nav.Bounds()):Center(options->Bounds()[1]);point(target);
 const auto entered=send(false);Check(entered.pointer.active&&!entered.back_pressed,"Neutral pointer did not enter original bounds");
 if(!back)Check(options->Status().pointer_states[1][0]==1,"NAV forwarding lost original Options hover");
 sample.focused=false;Check(!send(true).pointer.active&&options->Status().state==1,"Unfocused held input selected a menu");
 sample.focused=true;Check(!send(true).pointer.active&&options->Status().state==1,"Focus regain leaked a held press");
 Check(!send(false).pointer.active,"Focus recovery skipped neutral observation");Check(send(false).pointer.active,"Focus recovery did not rearm");
 sample.captured=true;Check(!send(true).pointer.active&&options->Status().state==1,"Captured held input selected a menu");
 sample.captured=false;Check(!send(true).pointer.active,"Capture release leaked a held press");send(false);Check(send(false).pointer.active,"Capture release did not rearm");
 const auto pressed=send(true);Check(pressed.pointer.active&&pressed.back_pressed==back,"Original back versus Options press dispatch differs");
 Check(options->Status().state==(back?3:2)&&options->Status().next_scene==(back?-1:14),"Original source selection differs");
 const auto frozen=options->Current();Reject([&]{control.Route(menu_shown,nav_shown,sample);});Check(options->Current()==frozen,"Inactive submenu input changed Options");
 for(unsigned i=0;i<80&&!options->Status().transition;++i)update(1.f/60);
 const auto status=options->Status();Check(bool(status.transition),"Original outro did not emit its typed service request");
 Check(status.transition->kind==(back?FrontendOptionsCommandKind::TransitionOptionsToMainMenu:FrontendOptionsCommandKind::PushScene)
  &&status.transition->scene==(back?-1:14),"Composed flow manufactured a destination");
 Check(music_calls==1&&stack.Entry(token).scene==13&&!stack.Entry(token).full_scene_created,"Commands replayed music or fabricated a new ready scene");
 Check(nav_session->Progress().visual_completed_mask==0&&nav_session->Progress().image_completed_files==0,"NAV reread shared dependencies");
 bool rejected=false;std::thread foreign([&]{try{control.BackBound();}catch(...){rejected=true;}});foreign.join();Check(rejected,"Composed control accepted foreign thread");
 nav.Release();stack.Release();options.reset();nav_session.reset();audio->Unload();Check(drains>0,"Composed teardown did not drain");
}
struct Host{bool live=false,disc=false;~Host(){if(live)ResetStartupFiles();if(disc)aurora_dvd_close();if(live){ResetStartupMemory();aurora_shutdown();}}};
}
int main(int argc,char** argv)
{
 try
 {
  Check(argc==4,"Supply disc/output/generated or owned");const bool owned=std::string_view(argv[3])=="owned";const auto folder=(std::filesystem::path(argv[2])/"options-navigation-host").string();std::filesystem::create_directories(folder);AuroraConfig config{};
  config.appName="Charged original Options navigation";config.userPath=config.cachePath=folder.c_str();config.resourcesPath=SDL_GetBasePath();config.desiredBackend=BACKEND_NULL;config.windowWidth=320;config.windowHeight=240;config.windowPosX=config.windowPosY=-1;config.mem1Size=MEM1_DEFAULT_SIZE;config.mem2Size=64*1024*1024;config.logLevel=LOG_WARNING;
  Host host;const auto h=aurora_initialize(argc,argv,&config);host.live=true;Check(h.window,"Aurora failed");InitializeStartupOS();nlInitMemory();Check(aurora_dvd_open(argv[1]),"Disc missing");host.disc=true;nlInitFileSystem();
  for(unsigned i=0;i<3;++i){const auto a=StandardAllocator.TotalFreeMemory(),b=VirtualAllocator.TotalFreeMemory();Lifecycle(owned,false);Lifecycle(owned,true);Check(!nlAsyncReadsPending(nullptr)&&a==StandardAllocator.TotalFreeMemory()&&b==VirtualAllocator.TotalFreeMemory(),"Composed teardown did not restore arenas/files");}
  std::cout<<checks<<" original Options/NAV composition checks passed; destination services remain explicit\n";
 }
 catch(const std::exception& e){allocation_budget=-1;std::cerr<<"FAILED: "<<e.what()<<" (check "<<checks<<")\n";return 1;}
}
