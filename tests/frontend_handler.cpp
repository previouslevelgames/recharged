#include "runtime/frontend_handler.h"
#include "runtime/startup.h"
#include "runtime/startup_files.h"
#include "NL/MemAlloc.h"
#include "NL/nlMemory.h"
#include "NL/nlFile.h"
#include "NL/nlFileGC.h"
#include <aurora/aurora.h>
#include <aurora/dvd.h>
#include <SDL3/SDL.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <source_location>
#include <thread>
using namespace mscharged;
using namespace mscharged::resources;
namespace
{
unsigned checks=0;
void Check(bool value,const char* message){++checks;if(!value)throw std::runtime_error(message);}
template<class F>void Reject(F f,std::source_location at=std::source_location::current())
{++checks;try{f();}catch(const std::exception&){return;}throw std::runtime_error("Expected rejection at line "+std::to_string(at.line()));}
void Near(float a,float b){Check(std::abs(a-b)<1e-6f,"Independent clock differs");}
FrontendSessionRequest Request(const char* path="/Art/fe/notification.fen")
{FrontendSessionRequest result;result.path=path;return result;}
void Pump(FrontendSession& session)
{
 const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(10);
 while(session.State()==FrontendSessionState::Loading){session.Service();Check(std::chrono::steady_clock::now()<deadline,"Handler scene loading timed out");SDL_Delay(1);}
 session.Result();
}
const FrontendInstance& Instance(const FrontendSession::Handle& frame,unsigned id)
{const auto it=std::find_if(frame->graph.instances.begin(),frame->graph.instances.end(),[&](const auto& value){return value.offset==id;});Check(it!=frame->graph.instances.end(),"Missing authored instance");return *it;}
FrontendInstanceChange Visible(unsigned id,bool value)
{FrontendInstanceChange edit;edit.instance=id;edit.property=FrontendInstanceProperty::Visible;edit.flag=value;return edit;}
void Generated()
{
 FrontendInput input;auto session=std::make_shared<FrontendSession>();FrontendHandler first(session,input),second(session,input);
 Reject([&]{first.Update({},0);});Reject([&]{first.AddScreen({});});
 session->Begin(Request());Pump(*session);auto initial=session->Current();Check(initial->layout.ImageCount()==1,"Nested notification layout is missing");
 std::array<FrontendPadSample,4> pads{};pads[0].connected=true;input.Update(pads,0);pads[0].buttons=0x100;input.Update(pads,0);
 unsigned activations=0;
 const auto toggle=first.AddScreen([&](const auto& frame,auto& owner){
   ++activations;
   Reject([&]{owner.Update(frame,0);});Reject([&]{owner.Release();});Reject([&]{owner.AddScreen({});});
   if(owner.Button(frame,FrontendAction::Accept,FrontendButtonQuery::Pressed))return std::vector{Visible(0x600,false)};
   return std::vector<FrontendInstanceChange>{};
 });
 const auto no_edits=first.AddScreen([](const auto&,auto&){return std::vector<FrontendInstanceChange>{};});
 Check(first.Screens()==std::vector{toggle,no_edits},"Original screen ring insertion order changed");
 second.SetExclusiveInput(true);first.Activate(initial,toggle);
 Check(activations==1&&session->Current()==initial&&first.ActiveScreen()==toggle,"Unfocused native adapter consumed input");
 first.Update(initial,.125f);auto advanced=first.Current();Near(advanced->graph.presentation_time,.125f);
 Check(!first.Button(advanced,FrontendAction::Accept,FrontendButtonQuery::Held)&&second.Button(advanced,FrontendAction::Accept,FrontendButtonQuery::Held),"Original focus gate did not route input");
 second.SetExclusiveInput(false);first.SetExclusiveInput(true);first.Activate(advanced,toggle);auto hidden=first.Current();
 Check(!Instance(hidden,0x600).visible&&Instance(advanced,0x600).visible&&hidden->layout.entries.empty(),"Input-driven edits did not atomically retain old scene");
 first.OriginalRemove(toggle);Check(first.Screens()==std::vector{toggle,no_edits}&&first.ActiveScreen()==toggle,"Original empty Remove unexpectedly detached a screen");
 first.Activate(hidden,no_edits);Check(first.ActiveScreen()==no_edits,"Activation did not change active adapter");
 const auto failing=first.AddScreen([](const auto&,auto&)->std::vector<FrontendInstanceChange>{throw std::runtime_error("activation failed");});
 Reject([&]{first.Activate(hidden,failing);});Check(first.ActiveScreen()==no_edits&&first.Current()==hidden,"Callback failure changed published state");
 const auto bad_edits=first.AddScreen([](const auto&,auto&){return std::vector{Visible(0x600,true),Visible(0xbad,false)};});
 Reject([&]{first.Activate(hidden,bad_edits);});Check(first.Current()==hidden&&first.ActiveScreen()==no_edits,"Rejected edit batch published partial activation");
 Reject([&]{second.Activate(hidden,toggle);});Reject([&]{first.Update(initial,0);});
 first.Detach(no_edits);Check(first.ActiveScreen()==0,"Native detach retained active pointer");Reject([&]{first.Activate(hidden,no_edits);});
 first.SetExclusiveInput(false);
 // Explicit native tracking of an already visible component, not HOME delivery.
 session->Begin(Request());Pump(*session);auto notice=session->Current();first.WatchLoadingNotification(notice,0x600);
 first.Update(notice,.25f);Near(first.Current()->graph.presentation_time,.25f);Check(first.LoadingNotificationActive()&&Instance(first.Current(),0x600).visible,"Notification completed early");
 first.Update(first.Current(),.25f);Check(first.LoadingNotificationActive(),"Notification ignored authored start time");
 auto before_end=first.Current();first.Update(before_end,.125f);Check(!first.LoadingNotificationActive()&&!Instance(first.Current(),0x600).visible,"Inclusive notification completion failed");
 Check(Instance(before_end,0x600).visible&&first.Current()->layout.entries.empty(),"Notification mutated retained or visible frame");
 // Missing active slide after external selection must roll back clock and flag.
 session->Begin(Request());Pump(*session);notice=session->Current();first.WatchLoadingNotification(notice,0x600);
 Check(!session->SelectComponent(0x690,"missing"),"Missing component slide was invented");auto missing=session->Current();
 Reject([&]{first.Update(missing,.25f);});Check(first.Current()==missing&&first.LoadingNotificationActive(),"Failed loading update published partial clock");
 session->Begin(Request());session->Cancel();Check(first.Current()==missing,"Cancelled replacement lost retained current");
 session->Begin(Request("/Art/fe/bad.fen"));while(session->State()==FrontendSessionState::Loading){session->Service();SDL_Delay(1);}
 Check(session->State()==FrontendSessionState::Failed&&first.Current()==missing,"Malformed replacement changed handler scene");
 session->Begin(Request());Pump(*session);first.Update(first.Current(),.125f);Check(!first.LoadingNotificationActive(),"New resource generation inherited old notification identity");
 auto retained=first.Current();first.Update(retained,0);Reject([&]{second.Update(retained,0);});
 // An externally captured session may mutate inside a user callback. Detect
 // that conflict and never publish its returned edits over the newer frame.
 const auto external=first.AddScreen([&](const auto&,auto&){session->Reset();return std::vector{Visible(0x600,false)};});
 const auto prior_active=first.ActiveScreen();Reject([&]{first.Activate(first.Current(),external);});
 Check(first.ActiveScreen()==prior_active&&Instance(first.Current(),0x600).visible,"External mutation conflict was overwritten");
 bool wrong_thread=false;std::thread foreign([&]{try{first.Release();}catch(const std::logic_error&){wrong_thread=true;}});foreign.join();Check(wrong_thread,"Foreign thread released handler");
 first.SetExclusiveInput(true);second.SetExclusiveInput(true);Reject([&]{first.Release();});second.SetExclusiveInput(false);first.SetExclusiveInput(false);
 auto capture=std::make_shared<int>(7);std::weak_ptr<int> weak=capture;
 const auto held=first.AddScreen([capture](const auto&,auto&){return std::vector<FrontendInstanceChange>{};});capture.reset();Check(!weak.expired(),"Activation capture not retained");first.Detach(held);Check(weak.expired(),"Detached callback capture leaked");
 struct DestructorProbe
 {
  FrontendHandler& handler;bool& rejected;
  DestructorProbe(FrontendHandler& h,bool& r):handler(h),rejected(r){}
  ~DestructorProbe(){try{handler.SetExclusiveInput(false);}catch(const std::logic_error&){rejected=true;}}
 };
 bool rejected=false;auto probe=std::make_shared<DestructorProbe>(first,rejected);
 const auto guarded=first.AddScreen([probe](const auto&,auto&){return std::vector<FrontendInstanceChange>{};});probe.reset();
 first.Detach(guarded);Check(rejected,"Captured destructor reentered handler removal");
 second.Release();std::weak_ptr<FrontendSession> lifetime=session;session.reset();Check(!lifetime.expired()&&first.Current(),"Handler did not retain session owner");
 first.Release();first.Release();Check(lifetime.expired(),"Released handler retained session");Reject([&]{first.Current();});Check(retained->visuals&&retained->images,"Retained frame lost resources");
 // Last-owner destruction drains real pending NL reads.
 auto pending=std::make_shared<FrontendSession>();{FrontendHandler holder(pending,input);pending->Begin(Request());pending.reset();}
 Check(!nlAsyncReadsPending(nullptr),"Handler destruction retained native file work");
}
void Owned()
{
 FrontendInput input;auto session=std::make_shared<FrontendSession>();FrontendHandler handler(session,input);
 auto request=Request("/Art/fe/game_summary.fen");request.initial_slide="Slide1";request.image_profile=FrontendImageProfile::InGame;
 session->Begin(request);Pump(*session);auto initial=session->Current();Check(initial->layout.TextCount()==8&&initial->layout.ImageCount()==11,"Owned initial mixed frame differs");
 const auto object=std::find_if(initial->layout.entries.begin(),initial->layout.entries.end(),[](const auto& p){return std::holds_alternative<FrontendLayoutText>(p);});Check(object!=initial->layout.entries.end(),"Owned frame has no visible text instance");
 const auto id=std::get<FrontendLayoutText>(*object).instance;
 std::array<FrontendPadSample,4> pads{};pads[2].connected=true;input.Update(pads,0);pads[2].buttons=0x100;input.Update(pads,0);
 const auto screen=handler.AddScreen([id](const auto& frame,auto& owner){if(owner.Button(frame,FrontendAction::Accept,FrontendButtonQuery::Pressed,2))return std::vector{Visible(id,false)};return std::vector<FrontendInstanceChange>{};});
 handler.Activate(initial,screen);Check(!Instance(handler.Current(),id).visible&&Instance(initial,id).visible&&handler.Current()->layout.TextCount()==7,"Owned input mutation failed retained visible layout");
 for(unsigned i=0;i<120;++i)handler.Update(handler.Current(),1.f/60);
 session->Begin(request);session->Cancel();Check(handler.Current()!=initial,"Owned cancelled reload lost current");
 handler.Detach(screen);handler.Release();session->Pop();Check(initial->layout.TextCount()==8&&initial->images->textures.size()==20,"Owned release invalidated retained resources");
 std::cout<<"Owned handler: 120 updates, pad2 activation, retained original 8 text/11 image frame\n";
}
struct Host
{
 bool live=false,disc=false;
 ~Host(){if(live)ResetStartupFiles();if(disc)aurora_dvd_close();if(live){ResetStartupMemory();aurora_shutdown();}}
};
}
int main(int argc,char**argv)
{
 try
 {
  Check(argc==4,"Supply disc, output directory and mode");const bool owned=std::string_view(argv[3])=="owned";
  const auto folder=(std::filesystem::path(argv[2])/"frontend-handler-data").string();std::filesystem::create_directories(folder);
  AuroraConfig config{};config.appName="Charged frontend handler";config.userPath=config.cachePath=folder.c_str();config.resourcesPath=SDL_GetBasePath();
  config.desiredBackend=BACKEND_NULL;config.windowWidth=320;config.windowHeight=240;config.windowPosX=config.windowPosY=-1;config.logLevel=LOG_WARNING;
  config.mem1Size=MEM1_DEFAULT_SIZE;config.mem2Size=64*1024*1024;
  Host host;const auto state=aurora_initialize(argc,argv,&config);host.live=true;Check(state.window,"Aurora initialization failed");
  InitializeStartupOS();nlInitMemory();Check(aurora_dvd_open(argv[1]),"Cannot open handler disc");host.disc=true;nlInitFileSystem();
  for(unsigned repeat=0;repeat<3;++repeat)
  {
   const auto a=StandardAllocator.TotalFreeMemory(),b=VirtualAllocator.TotalFreeMemory();if(owned)Owned();else Generated();
   Check(!nlAsyncReadsPending(nullptr)&&StandardAllocator.TotalFreeMemory()==a&&VirtualAllocator.TotalFreeMemory()==b,"Handler lifetime failed native cleanup");
  }
  std::cout<<checks<<" frontend handler checks passed\n";
 }
 catch(const std::exception& e){std::cerr<<"FAILED: "<<e.what()<<" (check "<<checks<<")\n";return 1;}
}
