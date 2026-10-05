#include "runtime/frontend_options_navigation.h"
#include "runtime/frontend_menu_cursor.h"
#include "runtime/frontend_menu_back.h"
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
template<class F>void Reject(F f,std::source_location at=std::source_location::current()){++checks;try{f();}catch(const std::exception&){return;}throw std::runtime_error("Invalid menu input operation accepted at "+std::to_string(at.line()));}
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
struct SnapshotDriver
{
 FrontendInput& input;FrontendInputMap mapping;FrontendInputSnapshot value;
 explicit SnapshotDriver(FrontendInput& i):input(i){value.window=1;}
 const FrontendInputSnapshot& Update(float dt=1.f/60)
 {value.delta=dt;++value.sequence;value.mapped=mapping.Update(input,value.devices,value.capture,dt);return value;}
};
FrontendSession::Handle EmptyFrame(){return std::make_shared<const FrontendSessionFrame>();}
void Policy()
{
 FrontendInput input;SnapshotDriver driver(input);FrontendMenuCursor cursor;auto frame=EmptyFrame();auto view=Viewport();cursor.Acknowledge(1,frame,view);FrontendCursorMouse no_mouse;
 FrontendPointerHost host(input);auto publication=host.Publish(frame,view,{});
 const auto route=[&](float dt=1.f/60){return host.Route(publication,cursor.Sample(driver.Update(dt),no_mouse));};
 route();Check(route().active,"Keyboard cursor required a physical mouse");
 driver.value.devices.keys[SDL_SCANCODE_RIGHT]=true;
 for(unsigned i=0;i<10;++i)route();Near(cursor.Status().position[0],80);Near(cursor.Status().position[1],0);
 driver.value.devices.keys[SDL_SCANCODE_UP]=true;route();Near(cursor.Status().position[0],80+8/std::sqrt(2.f));Near(cursor.Status().position[1],8/std::sqrt(2.f));
 driver.value.capture.focused=false;const auto position=cursor.Status().position;Check(!route().active,"Unfocused virtual cursor remained active");Check(cursor.Status().position==position,"Unfocused cursor moved");driver.value.capture.focused=true;Check(!route().active&&cursor.Status().position==position,"Held focus regain moved cursor");driver.value.devices.keys.fill(false);route();Check(route().active,"Neutral virtual input did not recover");
 driver.value.devices.keys[SDL_SCANCODE_RETURN]=true;Check(route().event.pressed,"Keyboard Accept did not use source action30");Check(!route().event.pressed,"Held Accept repeated original action30");driver.value.devices.keys.fill(false);route();
 const auto old=cursor.Status().position;driver.value.delta=0;Reject([&]{cursor.Sample(driver.value,no_mouse);});Check(cursor.Status().position==old,"Stale sample changed cursor");auto bad=driver.value;bad.sequence++;bad.delta=std::nanf("");Reject([&]{cursor.Sample(bad,no_mouse);});
 bool foreign=false;std::thread thread([&]{try{cursor.Status();}catch(...){foreign=true;}});thread.join();Check(foreign,"Foreign thread observed owned cursor");
 // A newly connected held pad must not move/select until its own real neutral.
 driver.value.devices.pads[0].id=77;driver.value.devices.pads[0].left_x=32767;driver.value.devices.pads[0].buttons[SDL_GAMEPAD_BUTTON_SOUTH]=true;route();Check(cursor.Status().position==old,"Hotplug held controller moved cursor");driver.value.devices.pads[0].left_x=0;driver.value.devices.pads[0].buttons.fill(false);route();route();driver.value.devices.pads[0].left_x=-32768;route();Near(cursor.Status().position[0],old[0]-8);
 driver.value.capture.gamepad=true;const auto captured=cursor.Status().position;route();Check(cursor.Status().position==captured,"Captured gamepad moved cursor");driver.value.devices.keys[SDL_SCANCODE_LEFT]=true;route();Near(cursor.Status().position[0],captured[0]-8);driver.value.devices.keys.fill(false);driver.value.devices.pads[0].left_x=0;driver.value.capture.gamepad=false;route();route();
 // Real mouse movement wins; a stationary mouse does not steal keyboard mode.
 FrontendCursorMouse mouse{9,480,360,true,true};host.Route(publication,cursor.Sample(driver.Update(),mouse));mouse.x+=12;host.Route(publication,cursor.Sample(driver.Update(),mouse));Check(cursor.Status().source==FrontendCursorSource::Mouse,"Real mouse movement did not regain cursor");
 driver.value.devices.keys[SDL_SCANCODE_RIGHT]=true;host.Route(publication,cursor.Sample(driver.Update(),mouse));const auto switched=cursor.Status().position;host.Route(publication,cursor.Sample(driver.Update(),mouse));Check(cursor.Status().source==FrontendCursorSource::KeyboardGamepad,"Stationary mouse stole virtual cursor");Near(cursor.Status().position[0],switched[0]+8);driver.value.devices.keys.fill(false);
 // A new shown scene resets to center with held input quarantined.
 driver.value.devices.keys[SDL_SCANCODE_RIGHT]=true;cursor.Acknowledge(2,frame,view);host.Route(publication,cursor.Sample(driver.Update(),no_mouse));Near(cursor.Status().position[0],0);driver.value.devices.keys.fill(false);route();route();
 // Independent original integer-half clamp, wide projection and HiDPI oracle.
 view.widescreen=true;view.x=100;view.width=1720;cursor.Acknowledge(2,frame,view);publication=host.Publish(frame,view,{});route();route();driver.value.devices.keys[SDL_SCANCODE_RIGHT]=true;for(unsigned i=0;i<100;++i)route(.1f);Near(cursor.Status().position[0],407);const auto at_edge=route(.1f);Near(at_edge.event.position[0],407);driver.value.devices.keys.fill(false);
 host.Release();
}
void BackPolicy()
{
 FrontendInput input;SnapshotDriver driver(input);auto frame=EmptyFrame();FrontendNavigationBackBinding binding{frame,1,{-20,20,-10,10},true};
 driver.Update();driver.value.devices.keys[SDL_SCANCODE_ESCAPE]=true;driver.Update();Check(bool(FrontendDesktopBackEvent(input,binding)),"Fresh actual Back did not form a source pointer event");driver.Update();Check(!FrontendDesktopBackEvent(input,binding),"Held Back repeated shortcut");
 driver.value.devices.keys.fill(false);driver.Update();driver.value.capture.keyboard=true;driver.value.devices.keys[SDL_SCANCODE_ESCAPE]=true;driver.Update();Check(!FrontendDesktopBackEvent(input,binding),"Captured Back leaked shortcut");driver.value.capture.keyboard=false;driver.Update();Check(!FrontendDesktopBackEvent(input,binding),"Capture regain admitted held Back");driver.value.devices.keys.fill(false);driver.Update();
 int focus=0,other=0;input.PushFocus(&focus);input.Focus(&other);driver.value.devices.keys[SDL_SCANCODE_ESCAPE]=true;driver.Update();Check(!FrontendDesktopBackEvent(input,binding),"Locked original FEInput leaked Back shortcut");input.PopFocus(&focus);input.Focus(&other);driver.value.devices.keys.fill(false);driver.Update();driver.value.devices.keys[SDL_SCANCODE_ESCAPE]=true;driver.Update();binding.visible=false;Check(!FrontendDesktopBackEvent(input,binding),"Hidden presented NAV activated Back");
 driver.value.devices.keys.fill(false);driver.Update();driver.value.devices.keys[SDL_SCANCODE_ESCAPE]=true;driver.Update();Check(!FrontendDesktopBackEvent(input,{}),"Absent Main Back target activated a shortcut");
}
struct VirtualPad
{
 SDL_JoystickID id=0;SDL_Joystick* stick=nullptr;FrontendInputSDL reader;
 VirtualPad()
 {
  SDL_VirtualJoystickDesc d{};SDL_INIT_INTERFACE(&d);d.type=SDL_JOYSTICK_TYPE_GAMEPAD;d.vendor_id=0xffff;d.product_id=0xfe01;d.naxes=SDL_GAMEPAD_AXIS_COUNT;d.nbuttons=SDL_GAMEPAD_BUTTON_COUNT;d.axis_mask=(1u<<SDL_GAMEPAD_AXIS_COUNT)-1;d.button_mask=(1u<<SDL_GAMEPAD_BUTTON_COUNT)-1;d.name="Charged menu cursor fixture";
  id=SDL_AttachVirtualJoystick(&d);Check(id,SDL_GetError());stick=SDL_OpenJoystick(id);Check(stick,SDL_GetError());Set(0,0,false);
 }
 ~VirtualPad(){if(stick)SDL_CloseJoystick(stick);if(id&&!SDL_DetachVirtualJoystick(id))std::terminate();SDL_PumpEvents();}
 void Set(float x,float y,bool accept)
 {Check(SDL_SetJoystickVirtualAxis(stick,SDL_GAMEPAD_AXIS_LEFTX,Sint16(x<0?x*32768:x*32767)),SDL_GetError());Check(SDL_SetJoystickVirtualAxis(stick,SDL_GAMEPAD_AXIS_LEFTY,Sint16(y>0?-y*32768:-y*32767)),SDL_GetError());Check(SDL_SetJoystickVirtualButton(stick,SDL_GAMEPAD_BUTTON_SOUTH,accept),SDL_GetError());SDL_UpdateJoysticks();SDL_PumpEvents();}
 FrontendInputDevices Read(){auto d=reader.ReadDevices();Check(d.pads[0].id==id,"Actual SDL virtual pad did not occupy player one");return d;}
};
void Menu(bool owned,unsigned item,bool use_pad)
{
 FrontendInput input;SnapshotDriver driver(input);auto audio=Audio(owned);unsigned seed=17,music=0;FrontendSceneStack stack(input,[]{});auto req=Request(13);req.resources_mode=FrontendSessionResourcesMode::PermanentMain;auto token=stack.QueuePush(req);std::shared_ptr<FrontendOptions> options;
 stack.BindVisual(token,[&](auto c){options=std::make_shared<FrontendOptions>(c.session,input,audio,seed,c.handler,0);return options;});Pump(stack,token);
 auto nav_session=std::make_shared<FrontendSession>();nav_session->BeginShared({"/Art/fe/fe_overlay.fen",FrontendLanguage::English,FrontendImageProfile::Main,"Slide1",true},stack.Resources(token));Pump(*nav_session);FrontendNavigation nav(nav_session,input,audio,seed);FrontendOptionsNavigation control(*options,nav,[&](unsigned n){Check(n==1,"Menu music source command differs");++music;});FrontendMenuCursor cursor;std::unique_ptr<VirtualPad> pad;if(use_pad)pad=std::make_unique<VirtualPad>();
 FrontendSession::Handle menu_shown,nav_shown;const auto publish=[&]{menu_shown=stack.Entry(token).prepared;stack.Publish(token,menu_shown);options->Acknowledge(menu_shown,Viewport());nav_shown=nav.Current();nav.Acknowledge(nav_shown,Viewport());cursor.Acknowledge(token,menu_shown,Viewport());};publish();Check(!nav.BackButton(nav_shown).visible,"Unpresented NAV Back was exposed to desktop shortcut");
 FrontendNavigationDispatch last;bool routed=false;
 const auto update=[&](float dt=1.f/60){if(pad)driver.value.devices=pad->Read();const auto& sampled=driver.Update(dt);stack.Update(dt,[&](auto id,const auto& shown){Check(id==token&&shown==menu_shown,"Virtual cursor received unshown menu");control.ApplyCommands();nav.AdvanceVisual(nav.Current(),dt);if(options->Status().initialized){const auto binding=nav.BackButton(nav_shown);
    const auto back=FrontendDesktopBackEvent(input,binding);
    if(back){last={};last.pointer={*back,true,1};nav.WithPresentedInput(nav_shown,[&]{last.back_pressed=nav.DeliverPointer(nav_shown,*back);if(last.back_pressed)options->NotifyBackButton(shown);});}
    else last=control.Route(shown,nav_shown,cursor.Sample(sampled,{}));routed=true;}control.ApplyCommands();});stack.RethrowFailure(token);publish();};
 for(unsigned i=0;i<80&&!options->Status().initialized;++i)update();Check(options->Status().initialized,"Actual Options intro failed");update();update();
 const auto target=item==3?Center(nav.Bounds()):Center(options->Bounds()[item]);
 for(unsigned axis=0;axis<2;++axis)
 {
  for(unsigned n=0;n<200;++n)
  {
   const float gap=target[axis]-cursor.Status().position[axis];if(std::abs(gap)<.01f)break;const float dir=gap>0?1:-1;
   if(pad)pad->Set(axis?0:dir,axis?dir:0,false);
   else{driver.value.devices.keys.fill(false);driver.value.devices.keys[axis?(dir>0?SDL_SCANCODE_UP:SDL_SCANCODE_DOWN):(dir>0?SDL_SCANCODE_RIGHT:SDL_SCANCODE_LEFT)]=true;}
   update(std::min(1.f/60,std::abs(gap)/480));Check(n<199,"Virtual movement failed to reach source measured bounds");
  }
  if(pad)pad->Set(0,0,false);else driver.value.devices.keys.fill(false);update();
 }
 Check(routed&&last.pointer.active&&nav.BackButton(nav_shown).visible,"Source pointer route required a mouse or lost shown Back");Near(cursor.Status().position[0],target[0]);Near(cursor.Status().position[1],target[1]);if(item<3)Check(options->Status().pointer_states[item][0]==1,"Keyboard/gamepad cursor did not execute original hover");
 const auto before=seed;const auto sounds=audio->Handles().size();
 if(item==3){if(pad){pad->Set(0,0,true);Check(SDL_SetJoystickVirtualButton(pad->stick,SDL_GAMEPAD_BUTTON_EAST,true),SDL_GetError());SDL_UpdateJoysticks();SDL_PumpEvents();}else{driver.value.devices.keys[SDL_SCANCODE_ESCAPE]=true;driver.value.devices.keys[SDL_SCANCODE_RETURN]=true;}}
 else if(pad)pad->Set(0,0,true);else driver.value.devices.keys[SDL_SCANCODE_RETURN]=true;update();
 if(!(last.back_pressed==(item==3)&&options->Status().state==(item==3?3:2)))throw std::runtime_error("Original menu selection differs: item="+std::to_string(item)+" state="+std::to_string(options->Status().state)+" active="+std::to_string(last.pointer.active)+" pressed="+std::to_string(last.pointer.event.pressed)+" back="+std::to_string(last.back_pressed));++checks;
 if(item<3)Check(options->Status().next_scene==std::array{15,14,23}[item],"Source Options destination differs");
 Check(audio->Handles().size()>sounds||seed!=before,"Original select cue/RNG was bypassed");
 if(pad)pad->Set(0,0,false);driver.value.devices.keys.fill(false);
 Check(music==1&&!stack.Entry(token).full_scene_created,"Host cursor fabricated handler readiness");nav.Release();stack.Release();pad.reset();audio->Unload();
}
void SDLReads(SDL_Window* window)
{
 FrontendInput input;VirtualPad pad;Reject([&]{pad.reader.LastSnapshot();});pad.reader.Poll(input,window,.01f);const auto old=pad.reader.LastSnapshot();Check(old.sequence==1&&old.window==SDL_GetWindowID(window)&&old.devices.pads[0].id==pad.id,"Actual SDL snapshot identity differs");Reject([&]{pad.reader.Poll(input,window,-1);});Check(pad.reader.LastSnapshot().sequence==old.sequence,"Rejected FE update published another snapshot");
 pad.Set(1,0,true);pad.reader.Poll(input,window,.01f);Check(pad.reader.LastSnapshot().sequence==2&&pad.reader.LastSnapshot().devices.pads[0].buttons[SDL_GAMEPAD_BUTTON_SOUTH],"SDL snapshot did not preserve actual control state");
 auto frame=EmptyFrame();int ww,wh,pw,ph;SDL_GetWindowSize(window,&ww,&wh);SDL_GetWindowSizeInPixels(window,&pw,&ph);FrontendMenuCursor c;c.Acknowledge(1,frame,{SDL_GetWindowID(window),unsigned(ww),unsigned(wh),unsigned(pw),unsigned(ph),0,0,double(pw),double(ph)});(void)c.Poll(pad.reader.LastSnapshot(),window);Check(SDL_SetWindowSize(window,ww+1,wh+1),SDL_GetError());SDL_PumpEvents();pad.reader.Poll(input,window,.01f);Check(!c.Poll(pad.reader.LastSnapshot(),window),"Unacknowledged resize produced a new cursor projection");
}
struct Host{bool live=false,disc=false;~Host(){if(live)ResetStartupFiles();if(disc)aurora_dvd_close();if(live){ResetStartupMemory();aurora_shutdown();}}};
}
int main(int argc,char** argv)
{
 try
 {
  Check(argc==4,"Supply disc/output/generated or owned");const bool owned=std::string_view(argv[3])=="owned";SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS,"1");SDL_SetHint(SDL_HINT_GAMECONTROLLER_IGNORE_DEVICES_EXCEPT,"0xFFFF/0xFE01");
  const auto folder=(std::filesystem::path(argv[2])/"menu-input-host").string();std::filesystem::create_directories(folder);AuroraConfig config{};config.appName="Charged menu cursor";config.userPath=config.cachePath=folder.c_str();config.resourcesPath=SDL_GetBasePath();config.desiredBackend=BACKEND_NULL;config.windowWidth=320;config.windowHeight=240;config.windowPosX=config.windowPosY=-1;config.mem1Size=MEM1_DEFAULT_SIZE;config.mem2Size=64*1024*1024;config.logLevel=LOG_WARNING;
  Host host;const auto h=aurora_initialize(argc,argv,&config);host.live=true;Check(h.window,"Aurora failed");Check(SDL_InitSubSystem(SDL_INIT_GAMEPAD),SDL_GetError());InitializeStartupOS();nlInitMemory();Check(aurora_dvd_open(argv[1]),"Disc missing");host.disc=true;nlInitFileSystem();const auto a=StandardAllocator.TotalFreeMemory(),b=VirtualAllocator.TotalFreeMemory();Policy();BackPolicy();SDLReads(h.window);for(unsigned i=0;i<4;++i)Menu(owned,i,i%2);Menu(owned,3,false);Check(!nlAsyncReadsPending(nullptr)&&a==StandardAllocator.TotalFreeMemory()&&b==VirtualAllocator.TotalFreeMemory(),"Menu input did not recover arenas/files");std::cout<<checks<<" native menu cursor checks passed\n";
 }
 catch(const std::exception& e){allocation_budget=-1;std::cerr<<"FAILED: "<<e.what()<<" (check "<<checks<<")\n";return 1;}
}
