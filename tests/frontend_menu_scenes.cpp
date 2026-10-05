#include "runtime/frontend_menu_scenes.h"
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
#include <fstream>
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
Data Calculation()
{
 Data header,records(120);Append(header,0x23401,Words({5,0xf1000100,0}));
 for(unsigned i=0;i<5;++i){Put(records,i*24,i);Put(records,i*24+4,0x1234+i);Put(records,i*24+12,i?0xf1000100:0);}Append(header,0x23402,records);return Wrap(0x80000001,Wrap(0x80023400,header));
}
Data MenuData(const char* path){std::unique_ptr<nlFile> f(nlOpen(path));Check(bool(f),"Required owned/synthetic NL file is absent");const auto n=nlFileSize(f.get(),nullptr);Data b(n);nlRead(f.get(),b.data(),n,n);return b;}
void SaveFile(const std::filesystem::path& p,const Data& b){std::ofstream f(p,std::ios::binary);f.write(reinterpret_cast<const char*>(b.data()),b.size());Check(bool(f),"Cannot write synthetic fixture");}
std::shared_ptr<FrontendAudio> Audio(bool owned,AudioCategoryVolumes::Handle volumes)
{
 LoadedAudioBank::Handle loaded;
 if(owned){auto catalog=ReadAudioBankCatalog(MenuData("/audio/nlxgs.bun"));AudioBankLoad load(catalog,23,21);const auto end=std::chrono::steady_clock::now()+std::chrono::seconds(15);while(load.State()==AudioBankLoadState::Loading){load.Service();Check(std::chrono::steady_clock::now()<end,"Resident audio timed out");SDL_Delay(1);}loaded=load.Result();}
 else
 {
  auto f=Make();const auto chunks=[](Bytes data){std::vector<std::pair<unsigned,Data>> out;for(std::size_t at=0;at<data.size();){const auto id=U32(data,at),n=U32(data,at+4);out.emplace_back(id,Data(data.begin()+at+8,data.begin()+at+8+n));at+=(n+11)&~3u;}return out;};
  constexpr std::array keys{0x96deb5c3u,0x3021a1eeu,0x1f824c84u,0x304fdd1eu,0xf0afd586u,0xaa73ef33u,0x6b0689d4u,0x0a93e9a0u,0xf6eb899eu,0x4430b152u,0xaccdca48u,0x6f6a3a07u,0xb19dbc20u,0x362f2841u};Data root;
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

void Flow(bool owned,const std::vector<std::uint8_t>& script,CameraAssetLibrary& library,const std::filesystem::path& folder)
{
 FrontendInput input;unsigned seed=0x194fe921;unsigned drains=0,effects=0,music=0;
 OriginalCameras core;FrontendCameras cameras(core,library);cameras.Push("startidle");
 auto path=std::filesystem::absolute(folder/"native-preferences");auto defaults=DefaultNativePreferences();
 defaults.audio={5,5,5};defaults.audio_defaults={9,8,7};defaults.auto_zoom=false;defaults.camera_zoom=.61f;
 auto bytes=EncodeNativePreferences(defaults);SaveFile(path,Data(bytes.begin(),bytes.end()));
 auto preferences=std::make_shared<NativePreferences>(path);
 preferences->StartLoad();while(preferences->Status().host_pending){preferences->Poll();SDL_Delay(1);}preferences->RethrowFailure();
 auto volumes=std::make_shared<AudioCategoryVolumes>(ReadAudioVolumeProfile(owned?Load("/audio/nlxgs.bun"):Calculation()),preferences->Current()->audio);
 auto audio=Audio(owned,volumes);auto visual_settings=std::make_shared<FrontendVisualSettings>(preferences->Current()->auto_zoom,preferences->Current()->camera_zoom);
 FrontendMenuScenes menus(input,audio,seed,cameras,script,preferences,
     [&](unsigned index){Check(index==1,"Source requested another music profile");++music;},
     [&](unsigned kind){Check(kind==95,"Source requested another stadium effect");++effects;return true;},[&]{++drains;},1,FrontendLanguage::English,volumes,visual_settings);
 Reject([&]{menus.DeliverPointer({});});Reject([&]{menus.Update(-1);});
 const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(15);
 while(!menus.Current().menu||!menus.Current().navigation){menus.Service();Check(std::chrono::steady_clock::now()<deadline,"Initial menus timed out");SDL_Delay(1);}
 auto shown=menus.Current();menus.Acknowledge(shown,Viewport());const auto images=shown.menu->images;const auto visuals=shown.menu->visuals;
 const auto step=[&](const std::function<void()>& action={}){
  Input(input);volumes->Update(volumes->Snapshot().frame+1,1.f/60);audio->ServiceAudio();audio->Update(1.f/60);menus.Update(1.f/60,action);shown=menus.Current();
  if(shown.navigation){Check(!shown.menu||(shown.menu->images==images&&shown.menu->visuals==visuals),"Transition replaced permanent resources");menus.Acknowledge(shown,Viewport());}
 };
 const auto until=[&](unsigned scene,std::source_location at=std::source_location::current()){
  for(unsigned i=0;i<2400&&!(menus.Status().scene==scene&&menus.Status().interactive);++i)step();
  if(menus.Status().scene!=scene||!menus.Status().interactive)std::cerr<<"Menu wait at line "<<at.line()<<": expected scene"<<scene<<", actual scene"<<menus.Status().scene<<" state"<<menus.Status().state<<" pending"<<menus.Status().pending_scene.value_or(0)<<" transition"<<unsigned(menus.Status().transition.state)<<"\n";
  Check(menus.Status().scene==scene&&menus.Status().interactive,"Source menu did not become interactive");
 };
 until(1);Check(menus.Bounds().size()==7&&!menus.Status().full_scene_created,"Main selected scope differs");
 const auto main_token=shown.token;
 step([&]{menus.DeliverPointer({0,Center(menus.Bounds()[6]),true});});
 Check(!shown.menu&&!shown.token&&!menus.Status().full_scene_created,"Departing Main remained published");
 // Actual source camera end, NAV completion and bytecode select Options.
 until(13);Check(shown.token!=main_token&&shown.menu->image_completed_files==0,"Options reread shared image bundles");
 Check(effects==1&&music>=1&&cameras.ActiveAlias()=="fechoosecaptains","Original departure hosts/camera differ");
 const auto options_token=shown.token;
 step([&]{menus.DeliverPointer({0,Center(menus.BackBounds()),true});});
 Check(menus.Status().state==3&&!menus.Status().interactive,"Real NAV Back did not start source Options outro");
 until(1);Check(shown.token!=main_token&&shown.token!=options_token&&shown.menu->image_completed_files==0,"Returning Main reused old handler or reread resources");
 Check(cameras.ActiveAlias()=="startidle"&&effects==1&&music>=2,"Original return camera/music hosts differ");
 // A second loop opens both original submenus and replaces Options anew.
 step([&]{menus.DeliverPointer({0,Center(menus.Bounds()[6]),true});});until(13);
 const auto child_options_token=shown.token;
 step([&]{menus.DeliverPointer({0,Center(menus.Bounds()[1]),true});});until(14);
 Check(menus.Bounds().size()==6&&shown.token!=child_options_token,"Audio did not replace original Options");
 step([&]{menus.DeliverPointer({0,Center(menus.Bounds()[0]),true});});
 Check(volumes->Snapshot().settings==std::array{4,5,5},"Original Audio decrement did not reach live category authority");
 const auto audio_token=shown.token;
 step([&]{menus.DeliverPointer({0,Center(menus.DoneBounds()),true});});
 Check(preferences->Status().host_pending&&menus.Status().state==3,"Done did not start actual native preferences worker");
 until(13);Check(shown.token!=audio_token&&shown.token!=child_options_token&&preferences->Current()->audio==std::array{4,5,5},"Audio save/return reused old Options or lost preferences");
 Check(preferences->Current()->audio_defaults==defaults.audio_defaults&&preferences->Current()->camera_zoom==defaults.camera_zoom,"Audio save changed unrelated native fields");
 step([&]{menus.DeliverPointer({0,Center(menus.Bounds()[1]),true});});until(14);
 step([&]{menus.DeliverPointer({0,Center(menus.Bounds()[0]),true});});Check(volumes->Snapshot().settings[0]==3,"Second Audio edit failed");
 step([&]{menus.DeliverPointer({0,Center(menus.BackBounds()),true});});until(13);
 Check(volumes->Snapshot().settings==std::array{4,5,5}&&preferences->Current()->audio[0]==4,"Audio Back failed to restore original backup");
 step([&]{menus.DeliverPointer({0,Center(menus.Bounds()[0]),true});});until(15);
 Check(menus.Bounds().size()==7,"Visual source pointer listener count differs");
 step([&]{menus.DeliverPointer({0,Center(menus.Bounds()[4]),true});});
 Check(visual_settings->Snapshot().zoom==1,"Visual level press did not reach desired settings");
 step([&]{menus.DeliverPointer({0,Center(menus.BackBounds()),true});});until(13);
 Check(!visual_settings->Snapshot().auto_zoom&&visual_settings->Snapshot().zoom==.5f&&preferences->Current()->camera_zoom==.61f,"Visual Back did not preserve source quantization/native file distinction");
 step([&]{menus.DeliverPointer({0,Center(menus.Bounds()[0]),true});});until(15);
 step([&]{menus.DeliverPointer({0,Center(menus.Bounds()[3]),true});});
 step([&]{menus.DeliverPointer({0,Center(menus.Bounds()[5]),true});});
 Check(visual_settings->Snapshot().zoom==.75f&&visual_settings->Snapshot().auto_zoom,"Visual level/mode source changes differ");
 step([&]{menus.DeliverPointer({0,Center(menus.DoneBounds()),true});});until(13);
 Check(preferences->Current()->camera_zoom==.75f&&preferences->Current()->auto_zoom&&preferences->Current()->audio==std::array{4,5,5}&&preferences->Current()->audio_defaults==defaults.audio_defaults,"Visual native save lost audio/default fields");
 step([&]{menus.DeliverPointer({0,Center(menus.BackBounds()),true});});until(1);
 Check(effects==2&&!preferences->Status().original_normal_save_loaded&&!preferences->Status().full_save_complete,"Selected menus manufactured original save readiness");
 const auto retained=shown;menus.Release();Reject([&]{menus.Update(0);});
 Check(retained.menu->images==images&&retained.navigation->visuals==visuals&&drains>8,"Menu teardown lost retained data or missed drains");
}
struct Host {bool live=false,disc=false;~Host(){if(live)ResetStartupFiles();if(disc)aurora_dvd_close();if(live){ResetStartupMemory();aurora_shutdown();}}};
}
int main(int argc,char** argv)
{
 try{
  Check(argc==4,"Supply disc/output/generated or owned");const bool owned=std::string_view(argv[3])=="owned";const auto folder=(std::filesystem::path(argv[2])/"menu-scenes-host").string();std::filesystem::create_directories(folder);AuroraConfig config{};
  config.appName="Charged original menu flow";config.userPath=config.cachePath=folder.c_str();config.resourcesPath=SDL_GetBasePath();config.desiredBackend=BACKEND_NULL;config.windowWidth=320;config.windowHeight=240;config.windowPosX=config.windowPosY=-1;config.mem1Size=MEM1_DEFAULT_SIZE;config.mem2Size=64*1024*1024;config.logLevel=LOG_WARNING;
  Host host;const auto h=aurora_initialize(argc,argv,&config);host.live=true;Check(h.window,"Aurora failed");InitializeStartupOS();nlInitMemory();Check(aurora_dvd_open(argv[1]),"Disc missing");host.disc=true;nlInitFileSystem();
  const auto before1=StandardAllocator.TotalFreeMemory(),before2=VirtualAllocator.TotalFreeMemory();CameraAssetLibrary library;Catalog(library);const auto script=Load("/Art/scripts/fe_presentation.byte_code");
  Flow(owned,script,library,folder);library.Clear();Check(!nlAsyncReadsPending(nullptr)&&before1==StandardAllocator.TotalFreeMemory()&&before2==VirtualAllocator.TotalFreeMemory(),"Menu flow did not restore arenas/files");
  std::cout<<checks<<" original Main/Options/Audio/Visual menu lifecycle checks passed; full SceneCreated remains unavailable\n";
 }catch(const std::exception&e){allocation_budget=-1;std::cerr<<"FAILED: "<<e.what()<<" (check "<<checks<<")\n";return 1;}
}
