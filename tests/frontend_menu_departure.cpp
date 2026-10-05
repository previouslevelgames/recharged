#include "runtime/frontend_menu_departure.h"
#include "runtime/frontend_camera_assets.h"
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
 const std::array keys{0x6b0689d4u,0x0a93e9a0u,0xf0afd586u,0x304fdd1eu,0xf6eb899eu,0x4430b152u,0xaccdca48u,0x6f6a3a07u,0xb19dbc20u};Data root;
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

void Catalog(CameraAssetLibrary& library)
{
 auto batch=LoadFrontendCameraAssets(library);const auto end=std::chrono::steady_clock::now()+std::chrono::seconds(15);
 while(batch.State()==CameraBatchState::Loading){batch.Service();Check(std::chrono::steady_clock::now()<end,"Camera assets timed out");SDL_Delay(1);}
 Check(batch.State()==CameraBatchState::Ready&&batch.Progress().succeeded==37,"Source camera catalog incomplete");batch.Publish();
}
void Lifecycle(bool owned,const std::vector<std::uint8_t>& script,CameraAssetLibrary& library,unsigned item)
{
 FrontendInput input;unsigned seed=0x194fe921;auto audio=Audio(owned);unsigned drains=0;
 OriginalCameras core;FrontendCameras cameras(core,library);cameras.Push("startidle");
 FrontendSceneStack stack(input,[&]{++drains;});FrontendMenuTransition flow(script,cameras,stack);
 auto request=Request(1);request.resources_mode=FrontendSessionResourcesMode::PermanentMain;
 const auto token=stack.QueuePush(request);std::shared_ptr<FrontendMainMenu> menu;
 stack.BindVisual(token,[&](auto c){menu=std::make_shared<FrontendMainMenu>(c.session,input,audio,seed,c.handler,false);return menu;});
 Pump(stack,token);Check(bool(menu),"Source Main selected visual was not created");
 auto nav_session=std::make_shared<FrontendSession>();nav_session->BeginShared({"/Art/fe/fe_overlay.fen",FrontendLanguage::English,FrontendImageProfile::Main,"Slide1",true},stack.Resources(token));Pump(*nav_session);
 FrontendNavigation nav(nav_session,input,audio,seed);nav.SetButtons(nav.Current(),4);
 const auto publish=[&]{const auto frame=menu->Current();stack.Publish(token,frame);menu->Acknowledge(frame,Viewport());return frame;};
 auto shown=publish();
 for(unsigned i=0;i<1200&&!menu->Status().interactive;++i){Input(input);stack.Update(1.f/60);shown=publish();}
 Check(menu->Status().interactive,"Main original intro did not initialize its seven controls");
 Reject([&]{BeginMainOptions(*menu,stack,token,nav,*audio,seed,flow);});
 Check(!stack.Entry(token).queued_pop&&flow.Status().state==FrontendMenuTransitionState::Idle,"Unselected Main changed services");
 Input(input);stack.Update(0,[&](auto t,const auto& visible){Check(t==token&&visible==shown,"Selection did not use exact published geometry");menu->DeliverPointer(visible,{0,Center(menu->Bounds()[item]),true});});
 const auto selected=menu->Status().selection;Check(selected&&selected->item==item&&selected->source==shown,"Original SelectItem did not reach its pending ApplyItem boundary");
 if(item!=6)
 {
  Reject([&]{BeginMainOptions(*menu,stack,token,nav,*audio,seed,flow);});
  Check(!stack.Entry(token).queued_pop&&nav.Status().visible_buttons==4&&flow.Status().state==FrontendMenuTransitionState::Idle,"Other Main action was misrouted to Options");
 }
 else
 {
  const auto cue_count=audio->ActiveCount(0xb19dbc20u);const auto before_seed=seed;
  BeginMainOptions(*menu,stack,token,nav,*audio,seed,flow);
  const std::array<std::string_view,3> back_path{"Slide1","Layer","back"};
  const auto back=FindFrontendNode(nav.Current()->graph,{},FrontendNamedPath(back_path));
  Check(bool(back),"Original navigation Back component is absent");
  const auto back_instance=std::find_if(nav.Current()->graph.instances.begin(),nav.Current()->graph.instances.end(),[&](const auto& v){return v.offset==back->id;});
  Check(stack.Entry(token).queued_pop&&nav.Status().visible_buttons==4&&back_instance!=nav.Current()->graph.instances.end()&&!back_instance->visible,
      "Original case6 Pop/Hide services did not execute or changed the stored mask");
  Check(audio->ActiveCount(0xb19dbc20u)==cue_count+1,"Actual departure resident cue was not admitted");
  const auto status=flow.Status();Check(status.state==FrontendMenuTransitionState::AwaitingService&&status.pending==FrontendMenuTransitionService::SaveGate&&!status.queued_scene,"Missing original save authority was bypassed");
  // The generated bank has random source selection; the owned departure cue
  // genuinely leaves the seed unchanged when no random modifier is present.
  if(!owned)Check(seed!=before_seed,"Generated departure cue did not use caller RNG");
  const auto after_seed=seed;Reject([&]{BeginMainOptions(*menu,stack,token,nav,*audio,seed,flow);});
  Check(seed==after_seed&&audio->ActiveCount(0xb19dbc20u)==cue_count+1,"Repeated source selection replayed admitted effects");
  stack.Poll();Reject([&]{stack.Entry(token);});
  Check(audio->ActiveCount(0xb19dbc20u)==cue_count+1,"Screen Pop cancelled its shared auto-release departure cue");
  flow.Update(1);Check(!flow.Status().queued_scene,"Elapsed time invented the missing original service");
 }
 flow.Release();nav.Release();stack.Release();menu.reset();nav_session.reset();audio->Unload();cameras.Release();
 Check(drains>0,"Original queued Pop did not drain the retained screen");
}
struct Host{bool live=false,disc=false;~Host(){if(live)ResetStartupFiles();if(disc)aurora_dvd_close();if(live){ResetStartupMemory();aurora_shutdown();}}};
}
int main(int argc,char** argv)
{
 try
 {
  Check(argc==4,"Supply disc/output/generated or owned");const bool owned=std::string_view(argv[3])=="owned";const auto folder=(std::filesystem::path(argv[2])/"menu-departure-host").string();std::filesystem::create_directories(folder);AuroraConfig config{};
  config.appName="Charged original Main departure";config.userPath=config.cachePath=folder.c_str();config.resourcesPath=SDL_GetBasePath();config.desiredBackend=BACKEND_NULL;config.windowWidth=320;config.windowHeight=240;config.windowPosX=config.windowPosY=-1;config.mem1Size=MEM1_DEFAULT_SIZE;config.mem2Size=64*1024*1024;config.logLevel=LOG_WARNING;
  Host host;const auto h=aurora_initialize(argc,argv,&config);host.live=true;Check(h.window,"Aurora failed");InitializeStartupOS();nlInitMemory();Check(aurora_dvd_open(argv[1]),"Disc missing");host.disc=true;nlInitFileSystem();
  const auto before1=StandardAllocator.TotalFreeMemory(),before2=VirtualAllocator.TotalFreeMemory();CameraAssetLibrary library;Catalog(library);const auto script=Load("/Art/scripts/fe_presentation.byte_code");
  for(unsigned i=0;i<3;++i){const auto a=StandardAllocator.TotalFreeMemory(),b=VirtualAllocator.TotalFreeMemory();Lifecycle(owned,script,library,0);Lifecycle(owned,script,library,6);Check(!nlAsyncReadsPending(nullptr)&&a==StandardAllocator.TotalFreeMemory()&&b==VirtualAllocator.TotalFreeMemory(),"Main departure did not restore arenas/files");}
  library.Clear();Check(before1==StandardAllocator.TotalFreeMemory()&&before2==VirtualAllocator.TotalFreeMemory(),"Catalog teardown did not restore arenas");
  std::cout<<checks<<" original Main Options departure checks passed; original save authority remains unavailable\n";
 }
 catch(const std::exception& e){allocation_budget=-1;std::cerr<<"FAILED: "<<e.what()<<" (check "<<checks<<")\n";return 1;}
}
