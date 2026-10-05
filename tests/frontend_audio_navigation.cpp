#include "runtime/frontend_audio_navigation.h"
#include "runtime/frontend_handler.h"
#include "runtime/frontend_music.h"
#include "runtime/frontend_stack.h"
#include "runtime/startup.h"
#include "runtime/startup_files.h"
#include "audio_bank_fixture.h"
#include "frontend_music_fixture.h"
#include "NL/nlFileGC.h"
#include "NL/MemAlloc.h"
#include "Game/FE/FrontendDoneButtonSteps.h"
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
std::shared_ptr<FrontendAudio> Audio(bool owned,AudioCategoryVolumes::Handle volumes)
{
 LoadedAudioBank::Handle loaded;
 if(owned){auto catalog=ReadAudioBankCatalog(Load("/audio/nlxgs.bun"));AudioBankLoad load(catalog,23,21);const auto end=std::chrono::steady_clock::now()+std::chrono::seconds(15);while(load.State()==AudioBankLoadState::Loading){load.Service();Check(std::chrono::steady_clock::now()<end,"Resident audio timed out");SDL_Delay(1);}loaded=load.Result();}
 else
 {
  auto f=Make();const auto chunks=[](Bytes data){std::vector<std::pair<unsigned,Data>> out;for(std::size_t at=0;at<data.size();){const auto id=U32(data,at),n=U32(data,at+4);out.emplace_back(id,Data(data.begin()+at+8,data.begin()+at+8+n));at+=(n+11)&~3u;}return out;};
  constexpr std::array keys{0x96deb5c3u,0x3021a1eeu,0x1f824c84u,0x304fdd1eu,0xf0afd586u,0xaa73ef33u,0x6b0689d4u,0x0a93e9a0u};Data root;
  for(auto [id,data]:chunks(Bytes(f.bytes).subspan(8)))
  {
   if(id==0x80023000){Data map,records;Append(map,0x23001,Words({unsigned(keys.size()),0,0}));for(unsigned i=0;i<keys.size();++i){auto row=Words({keys[i],0,0,0,i});records.insert(records.end(),row.begin(),row.end());}Append(map,0x23003,records);data=std::move(map);}
   if(id==0x80023300){Data graph;bool refs=false;for(auto [kind,part]:chunks(data)){
    if(kind==0x23301)Put(part,8,keys.size());
    if(kind==0x23302){Data rows;for(auto key:keys){auto row=Data(part.begin()+40,part.end());Put(row,0,key);rows.insert(rows.end(),row.begin(),row.end());}part=std::move(rows);}
    if(kind==0x23303)Put(part,12,4); // Synthetic real sound source belongs to SFX category.
    if(kind==0x23308){if(!refs){for(unsigned i=0;i<keys.size();++i)Append(graph,kind,Words({0xf0001000,0,Float(255),0,0}));refs=true;}continue;}Append(graph,kind,part);}data=std::move(graph);}
   Append(root,id,data);
  }
  loaded=std::make_shared<const LoadedAudioBank>(LoadedAudioBank{23,21,{23,"FE_GEN_Sfx"},{21,0,1,false},ReadAudioResidentBank(Wrap(0x80000001,root),f.wave)});
 }
 AudioVoicesOptions options;options.category_volumes=volumes;return std::make_shared<FrontendAudio>(loaded,volumes->Initial(),options);
}
auto Session(const char* path="/Art/fe/options_audio_options.fen")
{auto owner=std::make_shared<FrontendSession>();owner->Begin({path,FrontendLanguage::English,FrontendImageProfile::Main,"OPTIONS_IN",true});const auto end=std::chrono::steady_clock::now()+std::chrono::seconds(15);while(owner->State()==FrontendSessionState::Loading){owner->Service();Check(std::chrono::steady_clock::now()<end,"Audio options resources timed out");SDL_Delay(1);}owner->Result();return owner;}
FrontendStackRequest StackRequest(){FrontendStackRequest r;r.scene=14;r.initial_slide="OPTIONS_IN";return r;}
void StackPump(FrontendSceneStack& stack,std::uint64_t token)
{const auto end=std::chrono::steady_clock::now()+std::chrono::seconds(20);while(stack.Entry(token).state==FrontendStackState::Queued||stack.Entry(token).state==FrontendStackState::Loading){stack.Service();Check(std::chrono::steady_clock::now()<end,"Audio stack resources timed out");SDL_Delay(1);}stack.RethrowFailure(token);}
void Publish(FrontendSceneStack& stack,std::uint64_t token,FrontendAudioOptions& owner)
{auto frame=stack.Entry(token).prepared;stack.Publish(token,frame);owner.Acknowledge(frame,{1,640,480,640,480,0,0,640,480});}
std::array<float,2> Center(FrontendPointerBounds b){return{(b.min_x+b.max_x)/2,(b.min_y+b.max_y)/2};}
void PreferencesPump(NativePreferences& p)
{const auto end=std::chrono::steady_clock::now()+std::chrono::seconds(10);while(p.Status().host_pending){p.Poll();Check(std::chrono::steady_clock::now()<end,"Native preferences timed out");SDL_Delay(1);}p.RethrowFailure();}
auto Preferences(const std::filesystem::path& folder,unsigned index)
{
 auto values=DefaultNativePreferences();values.audio={5,5,5};values.audio_defaults={9,8,7};values.auto_zoom=false;values.camera_zoom=.75f;
 const auto path=std::filesystem::absolute(folder/("audio-preferences"+std::to_string(index)+".bin"));auto bytes=EncodeNativePreferences(values);SaveFile(path,Data(bytes.begin(),bytes.end()));auto preferences=std::make_shared<NativePreferences>(path);preferences->StartLoad();PreferencesPump(*preferences);Check(*preferences->Current()==values,"Preferences seed differs");return preferences;
}
void Pump(FrontendSession& session)
{const auto until=std::chrono::steady_clock::now()+std::chrono::seconds(15);while(session.State()==FrontendSessionState::Loading){session.Service();Check(std::chrono::steady_clock::now()<until,"NAV resource timeout");SDL_Delay(1);}session.Result();}
std::string DoneSlide(const FrontendSession::Handle& frame,std::uint32_t component)
{
 const auto& graph=frame->graph;auto n=std::find_if(graph.instances.begin(),graph.instances.end(),[&](auto& x){return x.offset==component;});Check(n!=graph.instances.end()&&n->library,"Done component missing");
 auto library=std::find_if(graph.library.begin(),graph.library.end(),[&](auto& l){return l.offset==*n->library;});Check(library!=graph.library.end()&&library->active_slide,"Done active slide missing");
 auto slide=std::find_if(graph.slides.begin(),graph.slides.end(),[&](auto& x){return x.offset==*library->active_slide;});Check(slide!=graph.slides.end(),"Done slide missing");return slide->name;
}
void Lifecycle(bool owned,const std::filesystem::path& folder,unsigned iteration,bool back,SDL_Window* window,bool save_fail=false)
{
 FrontendInput input;std::array<FrontendPadSample,4> pads{};for(auto& p:pads)p.connected=true;input.Update(pads,0);
 auto preferences=Preferences(folder,iteration);auto volumes=std::make_shared<AudioCategoryVolumes>(ReadAudioVolumeProfile(owned?Load("/audio/nlxgs.bun"):Calculation()),preferences->Current()->audio);auto audio=Audio(owned,volumes);unsigned seed=41;
 FrontendSceneStack stack(input,[]{});auto request=StackRequest();request.resources_mode=FrontendSessionResourcesMode::PermanentMain;
 auto token=stack.QueuePush(request);std::shared_ptr<FrontendAudioOptions> menu;stack.BindVisual(token,[&](auto c){menu=std::make_shared<FrontendAudioOptions>(c.session,input,audio,volumes,seed,c.handler);return menu;});StackPump(stack,token);
 auto nav_session=std::make_shared<FrontendSession>();nav_session->BeginShared({"/Art/fe/fe_overlay.fen",FrontendLanguage::English,FrontendImageProfile::Main,"Slide1",true},stack.Resources(token));Pump(*nav_session);
 auto nav=std::make_shared<FrontendNavigation>(nav_session,input,audio,seed);FrontendAudioNavigation control(menu,nav,input,audio,seed,preferences);Check(nav->Status().visible_buttons==0,"Initial Audio Hide did not hide NAV");
 FrontendSession::Handle shown,nav_shown;
 const auto publish=[&]{shown=stack.Entry(token).prepared;stack.Publish(token,shown);menu->Acknowledge(shown,{SDL_GetWindowID(window),640,480,640,480,0,0,640,480});nav_shown=nav->Current();nav->Acknowledge(nav_shown,{SDL_GetWindowID(window),640,480,640,480,0,0,640,480});control.Acknowledge(nav_shown);};
 const auto step=[&](const std::function<void()>& work={}){stack.Update(1.f/60,[&](auto,const auto& frame){Check(frame==shown,"Stack supplied wrong presented Audio");control.ApplyCommands();if(work)work();control.ApplyCommands();});stack.RethrowFailure(token);publish();};
 publish();bool intro_input=false;for(unsigned i=0;menu->Status().state==0&&i<100;++i)step([&]{if(menu->Status().state==1){intro_input=true;Check(!nav->DoneButton(nav_shown).visible,"Old hidden NAV was already visibly published");control.Deliver(shown,nav_shown,{0,{0,-210},true});Check(!menu->Status().native_save_admitted&&!control.DoneStatus().pressed&&control.DoneStatus().hover_feedback_requests==0,"Unpublished Done candidate accepted input");}});Check(intro_input,"First interactive publication boundary was not exercised");Check(menu->Status().state==1&&nav->Status().visible_buttons==0x24,"Source intro did not expose Back+Done");
 const auto binding=nav->DoneButton(nav_shown);auto bounds=control.DoneBounds();Check(bounds.min_x==-84&&bounds.max_x==84&&bounds.min_y==-259&&bounds.max_y==-165&&bounds.rotation==0&&bounds.pivot==std::array<float,2>{},"Original absolute Done bounds differ");
 const auto component=binding.component;Check(DoneSlide(nav_shown,component)=="off","Done intro feedback differs");
 unsigned labels=0;for(const auto& n:nav_shown->graph.instances)if(n.type==3&&n.name=="done"&&n.localization_hash==FrontendLowerHash("OPTIONS_ACCEPT"))++labels;
 Check(labels>=3,"Original OPTIONS_ACCEPT labels were not set in all three states");
 auto old=nav->Current();control.ApplyCommands();Check(nav->Current()==old,"Audio NAV commands replayed");
 Reject([&]{nav->SetDoneButtonText(nav->Current(),2);});Reject([&]{nav->SetDoneButtonSlide(nav->Current(),FrontendNavigationDoneSlide(99));});
 const auto point=Center(bounds);
 // Region identity is retained while source constants bypass text/quad sizing.
 {auto bad=bounds;bad.max_x=INFINITY;Reject([&]{FrontendPointerRegion p(input,nav_shown,component,bad);});Reject([&]{FrontendPointerRegion p(input,nav_shown,0,bounds);});FrontendPointerRegion p(input,nav_shown,component,bounds);Check(p.Contains(point)&&!p.Contains({84.01f,-210}),"Authored absolute listener changed original edge test");p.Release();unsigned enters=0;FrontendPointerRegion failure(input,nav_shown,component,bounds,[&](auto kind,unsigned,const auto&){if(kind==FrontendPointerCallback::Enter&&++enters==1)throw std::runtime_error("Real callback failure");});Reject([&]{failure.Deliver({0,point});});failure.Deliver({0,point});Check(enters==2,"Failed callback committed original previous pointer event");failure.Release();}
 step([&]{auto d=control.Deliver(shown,nav_shown,{0,point});Check(!d.back_pressed&&!d.pointer.active,"Explicit Done event fabricated desktop admission");});
 Check(control.DoneStatus().pointer_states[0]==1&&control.DoneStatus().hover_feedback_requests==1&&DoneSlide(nav_shown,component)=="over"&&audio->ActiveCount(0xaa73ef33)==1,"Original Done Enter did not set real hover/cue");
 step([&]{control.Deliver(shown,nav_shown,{1,point});});Check(audio->ActiveCount(0xaa73ef33)==1&&control.DoneStatus().pointer_states[1]==1,"Second pointer replayed shared Done hover cue");
 step([&]{control.Deliver(shown,nav_shown,{0,{-300,100}});});Check(DoneSlide(nav_shown,component)=="over","First Leave cleared second pointer hover");
 step([&]{control.Deliver(shown,nav_shown,{1,{-300,100}});});Check(DoneSlide(nav_shown,component)=="off","Final Leave did not restore Done off");
 // Host focus/capture/resize neutral policy must reach this real Done listener
 // through the SAME event as Back and volume controls, with no stale click.
 FrontendPointerDesktopSample sample{SDL_GetWindowID(window),640,480,640,480,1,73,320,452,true,true,false,false};
 const auto host=[&]{step([&]{auto result=control.Route(shown,nav_shown,sample);Check(!result.back_pressed&&!control.DoneStatus().pressed&&!preferences->Status().host_pending,"Suppressed desktop sample clicked Done");});++sample.sequence;};
 host();host();sample.captured=true;sample.primary_down=true;host();sample.captured=false;host();sample.primary_down=false;host();sample.focused=false;sample.primary_down=true;host();sample.focused=true;host();sample.primary_down=false;host();sample.x=-1;sample.primary_down=true;host();
 Check(SDL_SetWindowSize(window,800,600),"Actual host resize failed");step([&]{auto d=control.Poll(shown,nav_shown,window);Check(!d.pointer.active&&!d.pointer.event.pressed&&!control.DoneStatus().pressed,"Unacknowledged actual resize clicked Done");});
 // No second base/action30 query can occur while the source pre-base lock holds.
 int lock=0;input.PushFocus(&lock);old=menu->Current();stack.Update(.25f,[&](auto,const auto&){throw std::runtime_error("Locked Audio input ran");});stack.RethrowFailure(token);Check(menu->Current()==old,"Locked Audio advanced");auto quiet=control.Deliver(shown,nav_shown,{0,point,true});Check(!quiet.back_pressed&&!control.DoneStatus().pressed,"Locked Audio admitted Done");input.PopFocus(&lock);
 if(back)
 {
  const auto down=Center(menu->Bounds()[0]);step([&]{control.Deliver(shown,nav_shown,{0,down,true});});Check(menu->Status().settings[0]==4,"Audio decrement before Back failed");
  const auto hit=Center(nav->Bounds());step([&]{auto d=control.Deliver(shown,nav_shown,{0,hit,true});Check(d.back_pressed,"Actual Back listener did not win");});
  Check(volumes->Snapshot().settings==std::array{5,5,5}&&menu->Status().state==3&&!preferences->Status().host_pending&&!control.DoneStatus().pressed,"Back did not restore audio or incorrectly saved Done");
 }
 else if(save_fail)
 {
  preferences->Cancel();const auto published=shown;
  stack.Update(0,[&](auto,const auto&){control.ApplyCommands();control.Deliver(shown,nav_shown,{0,point,true});});
  Reject([&]{stack.RethrowFailure(token);});Check(control.Failed()&&stack.Entry(token).published==published&&!menu->Status().native_save_admitted&&!preferences->Status().host_pending,"Rejected real save provider published readiness or retired old frame");
  Reject([&]{control.ApplyCommands();});Reject([&]{control.Deliver(shown,nav_shown,{0,point,true});});
 }
 else
 {
  // Generated first volume-up overlaps the actual source Done rectangle.
  // One unchanged event must run all six menu listeners BEFORE saved values.
  step([&]{auto d=control.Deliver(shown,nav_shown,{0,point,true});Check(!d.back_pressed,"Done point unexpectedly hit Back");});
  const auto expected=owned?5:6;Check(control.DoneStatus().pressed&&menu->Status().native_save_admitted&&menu->Status().settings[0]==expected&&preferences->Status().host_pending,"Done did not admit real save after all menu listeners");
  Check(nav->Status().visible_buttons==0&&DoneSlide(nav_shown,component)=="down","Source reset/hide then down feedback differs");
  Check(!preferences->Status().full_save_complete&&!preferences->Status().original_normal_save_loaded,"Native Done claimed original SaveLoad readiness");
 }
 auto retained=shown;control.Release();stack.QueuePop(token);stack.Poll();menu.reset();nav->Release();nav.reset();nav_session.reset();stack.Release();
 if(!back&&!save_fail){PreferencesPump(*preferences);Check(preferences->Current()->audio[0]==(owned?5:6)&&preferences->Current()->camera_zoom==.75f&&preferences->Current()->audio_defaults==std::array{9,8,7},"Retained post-pop save lost menu ordering or visual/default fields");}
 Check(retained->visuals&&retained->images&&audio->Handles().empty(),"Audio/NAV teardown lost retained frame or live cues");audio->Unload();
}
struct Host{bool live=false,disc=false;~Host(){if(live)ResetStartupFiles();if(disc)aurora_dvd_close();if(live){ResetStartupMemory();aurora_shutdown();}}};
}
int main(int argc,char** argv)
{
 try{Check(argc==4,"Supply disc/output/generated or owned");const bool owned=std::string_view(argv[3])=="owned";auto folder=std::filesystem::absolute(std::filesystem::path(argv[2])/"audio-navigation-host");std::filesystem::create_directories(folder);const auto path=folder.string();AuroraConfig config{};config.appName="Charged Audio NAV Done";config.userPath=config.cachePath=path.c_str();config.resourcesPath=SDL_GetBasePath();config.desiredBackend=BACKEND_NULL;config.windowWidth=320;config.windowHeight=240;config.windowPosX=config.windowPosY=-1;config.mem1Size=MEM1_DEFAULT_SIZE;config.mem2Size=64*1024*1024;config.logLevel=LOG_WARNING;
 Host host;auto result=aurora_initialize(argc,argv,&config);host.live=true;Check(result.window,"Aurora failed");InitializeStartupOS();nlInitMemory();Check(aurora_dvd_open(argv[1]),"Audio NAV disc missing");host.disc=true;nlInitFileSystem();
 for(unsigned i=0;i<3;++i){const auto a=StandardAllocator.TotalFreeMemory(),b=VirtualAllocator.TotalFreeMemory();Lifecycle(owned,folder,i,i==1,result.window,i==2);Check(a==StandardAllocator.TotalFreeMemory()&&b==VirtualAllocator.TotalFreeMemory()&&!nlAsyncReadsPending(nullptr),"Audio/NAV did not recover arenas and reads");}std::cout<<checks<<" Audio/NAV Done ownership checks passed\n";
 }catch(const std::exception& e){allocation_budget=-1;std::cerr<<"FAILED: "<<e.what()<<" (check "<<checks<<")\n";return 1;}
}
