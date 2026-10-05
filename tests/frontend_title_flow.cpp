#include "runtime/frontend_menu_scenes.h"
#include "runtime/frontend_camera_assets.h"
#include "runtime/frontend_input_sdl.h"
#include "runtime/frontend_stack.h"
#include "runtime/startup.h"
#include "runtime/startup_files.h"
#include "frontend_music_fixture.h"
#include "NL/MemAlloc.h"
#include "NL/nlFileGC.h"
#include <aurora/aurora.h>
#include <aurora/dvd.h>
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
#include <tuple>
thread_local long allocation_budget=-1;
void* operator new(std::size_t n){if(allocation_budget==0)throw std::bad_alloc();if(allocation_budget>0)--allocation_budget;if(auto p=std::malloc(n?n:1))return p;throw std::bad_alloc();}
void* operator new[](std::size_t n){return ::operator new(n);}
void operator delete(void* p)noexcept{std::free(p);}void operator delete[](void* p)noexcept{std::free(p);}
void operator delete(void* p,std::size_t)noexcept{std::free(p);}void operator delete[](void* p,std::size_t)noexcept{std::free(p);}
using namespace mscharged;using namespace mscharged::resources;
namespace
{
using audio_bank_fixture::Data;
unsigned checks=0;
void Check(bool value,const char* message){++checks;if(!value)throw std::runtime_error(message);}
template<class F>void Reject(F f,std::source_location p=std::source_location::current())
{++checks;try{f();}catch(const std::exception&){return;}throw std::runtime_error("Invalid Title operation accepted at "+std::to_string(p.line()));}
void Near(float a,float b){Check(std::isfinite(a)&&std::abs(a-b)<.001f,"Independent Title clock/bounds differ");}
std::vector<std::uint8_t> Load(const char* path)
{
 std::unique_ptr<nlFile> f(nlOpen(path));Check(bool(f),"Title NL input absent");const auto size=nlFileSize(f.get(),nullptr);std::vector<std::uint8_t> b(size);nlRead(f.get(),b.data(),size,size);return b;
}
void Save(const std::filesystem::path& path,const std::vector<std::uint8_t>& b)
{std::ofstream f(path,std::ios::binary);f.write(reinterpret_cast<const char*>(b.data()),b.size());Check(bool(f),"Cannot save independent music fixture");}
void Pads(FrontendInput& input,unsigned buttons=0,unsigned controller=0)
{std::array<FrontendPadSample,4> p{};for(auto& pad:p)pad.connected=true;p.at(controller).buttons=buttons;input.Update(p,0);}
std::shared_ptr<FrontendSession> Session(const char* path="/art/fe/sms2_start.fen")
{
 auto s=std::make_shared<FrontendSession>();s->Begin({path,FrontendLanguage::English,FrontendImageProfile::Main,"regular",true});
 const auto end=std::chrono::steady_clock::now()+std::chrono::seconds(20);while(s->State()==FrontendSessionState::Loading){s->Service();Check(std::chrono::steady_clock::now()<end,"Title scene read timed out");SDL_Delay(1);}s->Result();return s;
}
std::shared_ptr<FrontendAudio> Audio(bool owned)
{
 using namespace audio_bank_fixture;
 if(owned)
 {
  auto global=Load("audio/nlxgs.bun");auto catalog=ReadAudioBankCatalog(global);AudioBankLoad owner(catalog,23,21);
  const auto end=std::chrono::steady_clock::now()+std::chrono::seconds(20);while(owner.State()==AudioBankLoadState::Loading){owner.Service();Check(std::chrono::steady_clock::now()<end,"Title sound bank read timed out");SDL_Delay(1);}
  return std::make_shared<FrontendAudio>(owner.Result(),ReadAudioCalculationInitial(global),AudioVoicesOptions{32});
 }
 auto f=Make();const auto word=[](const Data& d,std::size_t at){return (std::uint32_t(d.at(at))<<24)|(std::uint32_t(d.at(at+1))<<16)|(std::uint32_t(d.at(at+2))<<8)|d.at(at+3);};
 const auto chunks=[&](const Data& d){std::vector<std::pair<unsigned,Data>> out;for(std::size_t at=0;at<d.size();){const auto id=word(d,at),n=word(d,at+4);out.emplace_back(id,Data(d.begin()+at+8,d.begin()+at+8+n));at+=(n+11)&~3u;}return out;};
 constexpr std::array keys{0x26894c84u,0xaa73ef32u,0x55c84a9du,0x80060b2du,0x96deb5c3u,0x3021a1eeu,0x1f824c84u,0x304fdd1eu,0xf0afd586u,0xaa73ef33u,0x6b0689d4u,0x0a93e9a0u,0xf6eb899eu,0x4430b152u,0xaccdca48u,0x6f6a3a07u,0xb19dbc20u,0x362f2841u};Data root;
 for(auto [id,d]:chunks(Data(f.bytes.begin()+8,f.bytes.end())))
 {
  if(id==0x80023000)
  {Data map,records;Append(map,0x23001,Words({unsigned(keys.size()),0,0}));for(unsigned i=0;i<keys.size();++i){auto b=Words({keys[i],0,0,0,i});records.insert(records.end(),b.begin(),b.end());}Append(map,0x23003,records);d=std::move(map);}
  if(id==0x80023300)
  {
   Data graph;bool refs=false;
   for(auto [kind,part]:chunks(d))
   {
    if(kind==0x23301)Put(part,8,unsigned(keys.size()));
    if(kind==0x23302){Data rows;for(auto key:keys){auto row=Data(part.begin()+40,part.end());Put(row,0,key);rows.insert(rows.end(),row.begin(),row.end());}part=std::move(rows);}
    if(kind==0x23308){if(!refs){for(unsigned i=0;i<keys.size();++i)Append(graph,kind,Words({0xf0001000,0,Float(255),0,0}));refs=true;}continue;}
    Append(graph,kind,part);
   }
   d=std::move(graph);
  }
  Append(root,id,d);
 }
 auto bank=std::make_shared<const LoadedAudioBank>(LoadedAudioBank{23,21,{23,"FE_GEN_Sfx"},{21,0,1,false},ReadAudioResidentBank(Wrap(0x80000001,root),f.wave)});
 return std::make_shared<FrontendAudio>(bank,ReadAudioCalculationInitial(frontend_music_fixture::Calculation()),AudioVoicesOptions{32});
}
std::shared_ptr<FrontendMusic> Music(bool owned)
{
 AudioBankCatalog::Handle catalog;AudioCalculationInitial::Handle calculation;
 if(owned){auto global=Load("audio/nlxgs.bun");catalog=ReadAudioBankCatalog(global);calculation=ReadAudioCalculationInitial(global);}
 else{auto c=std::make_shared<AudioBankCatalog>();c->names.resize(27);c->slots.resize(23);c->names[26]={26,"FE_GEN_Music"};c->slots[22]={22,0,0,true};catalog=c;calculation=ReadAudioCalculationInitial(Load("audio/calculation.bun"));}
 return std::make_shared<FrontendMusic>(catalog,calculation);
}
void PumpMusic(FrontendMusic& m)
{const auto end=std::chrono::steady_clock::now()+std::chrono::seconds(20);while(m.Status().load==FrontendMusicLoadState::Loading){m.Service();Check(std::chrono::steady_clock::now()<end,"Title music read timed out");SDL_Delay(1);}m.Check();}
FrontendPointerViewport Viewport(){return {1,960,720,1920,1440,0,0,1920,1440};}
void Ack(FrontendTitle& t){t.Acknowledge(t.Current(),Viewport());}
std::string Active(const FrontendSession::Handle& f)
{for(const auto& s:f->graph.slides)if(s.offset==f->graph.active_slide)return s.name;throw std::logic_error("Title presentation absent");}
std::string Feedback(const FrontendSession::Handle& f)
{
 const std::array<std::string_view,2> path{"Layer2","Component2"};auto node=FindFrontendNode(f->graph,{FrontendNodeKind::Slide,*f->graph.active_slide},FrontendNamedPath(path),FrontendNodeType::Component);Check(bool(node),"Title component absent");
 auto i=std::find_if(f->graph.instances.begin(),f->graph.instances.end(),[&](const auto& x){return x.offset==node->id;});Check(i!=f->graph.instances.end()&&i->library,"Title library absent");
 auto lib=std::find_if(f->graph.library.begin(),f->graph.library.end(),[&](const auto& x){return x.offset==i->library;});Check(lib!=f->graph.library.end(),"Title component library absent");
 for(const auto& s:f->graph.slides)if(s.offset==lib->active_slide)return s.name;throw std::logic_error("Title feedback slide absent");
}
void Created(const FrontendTitleStatus& s)
{
 Check(!s.full_scene_created&&!s.wii_motion_available&&!s.icons_loaded&&!s.departure,"Selected Title fabricated complete constructor services");
 Check(s.commands.size()==7&&s.commands[0].kind==FrontendTitleCommandKind::Dimming&&s.commands[0].argument==2,"Title dimming creation order differs");
 for(unsigned i=0;i<4;++i)Check(s.commands[i+1].kind==FrontendTitleCommandKind::PointerWaiting&&s.commands[i+1].argument==i,"Title creation pointer order differs");
 Check(s.commands[5].kind==FrontendTitleCommandKind::PointerEnabled&&!s.commands[5].argument&&s.commands[6].kind==FrontendTitleCommandKind::ResetNavigation,"Title source initial NAV/pointer requests differ");
}
void Departed(const FrontendTitleStatus& s,const FrontendSession::Handle& shown,bool source_input=false,bool initialize=false,unsigned controller=0)
{
 Check(s.departure==FrontendTitleCommandKind::TransitionTitleToMain&&s.source==shown,"Title departure lost exact source publication");
 std::vector<FrontendTitleCommand> expected;
 const auto press=[&]{expected.push_back({FrontendTitleCommandKind::PointerEnabled,1});expected.push_back({FrontendTitleCommandKind::PopScene,0});expected.push_back({FrontendTitleCommandKind::Dimming,0});for(unsigned i=0;i<4;++i)expected.push_back({FrontendTitleCommandKind::PointerWaiting,i});expected.push_back({FrontendTitleCommandKind::TransitionTitleToMain,0});};
 if(initialize)for(unsigned i=0;i<4;++i)expected.push_back({FrontendTitleCommandKind::PointerCursor,i});
 if(source_input)for(unsigned i=0;i<4;++i){expected.push_back({i==controller?FrontendTitleCommandKind::PointerAccept:FrontendTitleCommandKind::PointerWaiting,i});if(i==controller)press();}else press();
 Check(s.commands.size()==expected.size(),"Title press command count differs from source route");
 for(unsigned i=0;i<expected.size();++i)Check(s.commands[i].kind==expected[i].kind&&s.commands[i].argument==expected[i].argument,"Title original pointer/pop/dimming/transition order differs");
}

void Catalog(CameraAssetLibrary& library)
{
 auto batch=LoadFrontendCameraAssets(library);const auto end=std::chrono::steady_clock::now()+std::chrono::seconds(20);
 while(batch.State()==CameraBatchState::Loading){batch.Service();Check(std::chrono::steady_clock::now()<end,"Title flow camera assets timed out");SDL_Delay(1);}Check(batch.State()==CameraBatchState::Ready&&batch.Progress().succeeded==37,"Original camera catalog is incomplete");batch.Publish();
}
std::shared_ptr<NativePreferences> Preferences(const std::filesystem::path& path)
{
 auto p=std::make_shared<NativePreferences>(std::filesystem::absolute(path));p->StartLoad();
 const auto end=std::chrono::steady_clock::now()+std::chrono::seconds(20);while(p->Status().host_pending){p->Poll();Check(std::chrono::steady_clock::now()<end,"Title flow native preferences timed out");SDL_Delay(1);}p->RethrowFailure();return p;
}
void DimmingAuthority()
{
 FrontendTitleDimming absent;Check(!absent.Admit(2)&&!absent.Status().armed&&absent.Status().pending==2,"Missing dimming was admitted");Reject([&]{absent.Admit(1);});
 unsigned mode=0;bool available=false;std::vector<unsigned> requests;
 FrontendTitleDimming real([&](unsigned value){requests.push_back(value);if(!available)return false;mode=value;return true;});
 Check(!real.Admit(2)&&mode==0&&!real.Status().armed,"Pending dimming mutated source guard");available=true;Check(real.Admit(2)&&mode==2&&real.Status().armed,"Actual dimming admission did not set source guard");
 const auto admitted=requests.size();Check(real.Admit(2)&&requests.size()==admitted,"Repeated Title constructor replayed source dimming2");
 available=false;Check(!real.Admit(0)&&real.Status().armed&&mode==2,"Pending dimming0 cleared source guard");Reject([&]{real.Admit(2);});Check(real.Status().pending==0&&real.Status().armed,"Conflicting Title request erased pending source dimming0");available=true;Check(real.Admit(0)&&!real.Status().armed&&mode==0,"Actual dimming0 did not restore default/clear source guard");
 Check(real.Admit(2)&&mode==2&&requests.back()==2,"New Title did not reapply dimming2 after source Press0");
 bool foreign=false;std::thread thread([&]{try{real.Status();}catch(const std::logic_error&){foreign=true;}});thread.join();Check(foreign,"Foreign dimming thread accepted");
}
struct Presentation
{
 FrontendMenuScenes& menus;
 FrontendMenuScenesFrame shown;
 void Ack(){shown=menus.Current();if(shown.navigation)menus.Acknowledge(shown,Viewport());}
 void Step(FrontendInput& input,float dt,const std::function<void()>& command={})
 {Pads(input);menus.Update(dt,command);Ack();}
 void Load()
 {const auto end=std::chrono::steady_clock::now()+std::chrono::seconds(20);while(!menus.Current().navigation||!menus.Current().menu){menus.Service();Check(std::chrono::steady_clock::now()<end,"Title flow initial FEN/NAV timed out");SDL_Delay(1);}Ack();}
};
void MissingServices(bool owned,const Data& script,CameraAssetLibrary& library,const std::filesystem::path& root)
{
 FrontendInput input;Pads(input);unsigned seed=70;OriginalCameras core;FrontendCameras cameras(core,library);cameras.Push("startidle");auto audio=Audio(owned);auto music=Music(owned);auto prefs=Preferences(root/"title-missing-preferences");
 Reject([&]{FrontendMenuScenes invalid(input,audio,seed,cameras,script,prefs,[&](unsigned i){music->BeginSelect(i,seed);},{},[]{},0);});
 auto missing=std::make_shared<FrontendTitleDimming>();FrontendMenuScenes menus(input,audio,seed,cameras,script,prefs,[&](unsigned i){music->BeginSelect(i,seed);},{},[]{},0,FrontendLanguage::English,{},{},{},FrontendMenuTitleServices{music,missing});
 Presentation view{menus};view.Load();const auto initial=view.shown.menu;const auto status=menus.Status();Check(status.title&&status.pending_title_service==FrontendTitleCommandKind::Dimming&&!status.interactive&&!status.full_scene_created&&music->Status().load==FrontendMusicLoadState::Idle&&audio->Handles().empty(),"Missing real service allowed partial music/cue/source advancement");
 for(unsigned i=0;i<8;++i){view.Step(input,.5f);Check(view.shown.menu==initial&&menus.Status().title->elapsed==0&&menus.Status().title->admitted_operations==0,"Missing dimming replayed/advanced Title source");}
 const auto retained=view.shown;menus.Release();Check(retained.menu->images&&retained.menu->visuals&&audio->Handles().empty(),"Missing-service teardown lost retained frame or audio ownership");music->Unload();audio->Unload();cameras.Release();core.Release();
}
void Flow(bool owned,const Data& script,CameraAssetLibrary& library,const std::filesystem::path& root)
{
 FrontendInput input;Pads(input);unsigned seed=710,drains=0;OriginalCameras core;FrontendCameras cameras(core,library);cameras.Push("startidle");auto audio=Audio(owned);auto music=Music(owned);auto prefs=Preferences(root/"title-flow-preferences");
 bool allow_initial=false,allow_restore=false,fail_drain=false;unsigned idle_seconds=300;std::vector<unsigned> modes;FrontendMenuScenes* current=nullptr;
 auto dimming=std::make_shared<FrontendTitleDimming>([&](unsigned mode){
   modes.push_back(mode);
   if(mode==2){Check(audio->ActiveCount(0x26894c84)==0&&music->Status().load==FrontendMusicLoadState::Idle,"Creation cue/music preceded dimming admission");if(!allow_initial)return false;idle_seconds=900;return true;}
   Check(current&&current->Status().pointer_enabled&&!current->Current().menu,"Source Pop/pointerEnable did not precede dimming0");
   Check(audio->ActiveCount(0x55c84a9d)>0&&audio->ActiveCount(0x80060b2d)==0&&cameras.ActiveAlias()=="startidle","Original cue/dimming/script order was rearranged");
   if(!allow_restore)return false;idle_seconds=300;return true;
 });
 FrontendMenuScenes menus(input,audio,seed,cameras,script,prefs,[&](unsigned i){music->BeginSelect(i,seed);},{},[&]{++drains;if(fail_drain){fail_drain=false;throw std::runtime_error("Actual renderer drain failed");}},0,FrontendLanguage::English,{},{},{},FrontendMenuTitleServices{music,dimming});current=&menus;
 Presentation view{menus};view.Load();Check(menus.Status().pending_title_service==FrontendTitleCommandKind::Dimming&&idle_seconds==300,"Title did not retain missing creation admission");const auto first=view.shown;
 allow_initial=true;menus.Service();view.Ack();PumpMusic(*music);Check(idle_seconds==900&&dimming->Status().armed&&menus.Status().title->admitted_operations==menus.Status().title->operations.size()&&!menus.Status().pointer_enabled,"Creation journal did not admit real services in source order");
 const auto initial=menus.Current();menus.Update(.5f);Check(menus.Status().title->elapsed==.5f,"Published Title failed original base update");const auto unshown=menus.Current();menus.Update(.5f);Check(menus.Current().menu==unshown.menu&&menus.Status().title->elapsed==.5f,"Unpresented candidate advanced Title clock");Reject([&]{menus.Acknowledge(initial,Viewport());});view.Ack();
 view.Step(input,1);Check(menus.Status().interactive&&menus.Status().title->initialized&&menus.Bounds().size()==1,"Title1.5s gate failed in central graph");const auto title_token=view.shown.token;const auto published=view.shown;const auto images=published.menu->images;const auto visuals=published.menu->visuals;
 const auto b=menus.Bounds()[0];const std::array<float,2> center{(b.min_x+b.max_x)/2,(b.min_y+b.max_y)/2};
 view.Step(input,0,[&]{Check(!menus.BackShortcut(),"Title admitted NAV Back instead of original local input");menus.DeliverPointer({0,center,true});});
 const auto waiting=menus.Status();Check(waiting.title&&waiting.pending_title_service==FrontendTitleCommandKind::Dimming&&waiting.title->source==published.menu&&!waiting.interactive&&idle_seconds==900&&dimming->Status().armed,"Pending source Press0 discarded exact presented owner");
 Check(!view.shown.menu&&!view.shown.token&&cameras.ActiveAlias()=="startidle","Pending dimming started script or fabricated another menu");const auto sounds=audio->Handles();const auto cursor=waiting.title->admitted_operations;
 for(unsigned i=0;i<3;++i){view.Step(input,.25f);Check(menus.Status().title&&menus.Status().title->admitted_operations==cursor&&audio->Handles()==sounds&&!nlGetCurrentAsyncRead(),"Pending post-Pop admission replayed audio or discarded Title owner");}
 allow_restore=true;menus.Service();view.Ack();Check(!menus.Status().title&&idle_seconds==300&&!dimming->Status().armed&&cameras.ActiveAlias()=="startmainmenumove"&&audio->ActiveCount(0x80060b2d)>0,"Actual dimming/script/transition cue admission or persistent audio transfer failed");
 Check(menus.Status().title_transition->state==FrontendTransitionState::Running&&!view.shown.menu,"Original camera endpoint was skipped");
 for(unsigned i=0;i<1200&&!(menus.Status().scene==1&&menus.Status().interactive);++i){music->Service();view.Step(input,1.f/60);SDL_Delay(1);}
 const auto main=menus.Status();Check(main.scene==1&&main.interactive&&main.title_transition->state==FrontendTransitionState::SceneQueued&&!main.full_scene_created&&!main.pending_scene&&!main.pending_main_music,"Original VM endpoint failed to queue genuine Main selected owner");
 Check(view.shown.token!=title_token&&view.shown.menu&&view.shown.menu->images==images&&view.shown.menu->visuals==visuals&&view.shown.menu->image_completed_files==0&&menus.Bounds().size()==7,"Title Pop/Main Push lost FIFO identity or permanent resources");PumpMusic(*music);Check(music->Status().cue==0x445abf3a&&music->Status().submitted_frames>0,"Selected Main arrival did not execute actual music1");
 Reject([&]{menus.Acknowledge(first,Viewport());});const auto retained=view.shown;fail_drain=true;Reject([&]{menus.Release();});Check(retained.menu->images&&retained.navigation->images,"Failed drain discarded immutable Title/Main resources");menus.Release();menus.Release();Check(audio->Handles().empty()&&drains>0,"Title/Main coordinator leaked retained audio after retry teardown");music->Unload();audio->Unload();cameras.Release();core.Release();
}
void IdleAndRepeated(bool owned,const Data& script,CameraAssetLibrary& library,const std::filesystem::path& root)
{
 FrontendInput input;Pads(input);unsigned seed=300;auto audio=Audio(owned);auto music=Music(owned);auto prefs=Preferences(root/"title-idle-preferences");unsigned mode=0,changes=0;
 auto dimming=std::make_shared<FrontendTitleDimming>([&](unsigned value){mode=value;++changes;return true;});
 for(unsigned repeat=0;repeat<2;++repeat)
 {
  OriginalCameras core;FrontendCameras cameras(core,library);cameras.Push("startidle");FrontendMenuScenes menus(input,audio,seed,cameras,script,prefs,[&](unsigned i){music->BeginSelect(i,seed);},{},[]{},0,FrontendLanguage::English,{},{},{},FrontendMenuTitleServices{music,dimming});Presentation view{menus};view.Load();PumpMusic(*music);
  Check(mode==2&&changes==1&&dimming->Status().armed,"Repeated Title destruction replayed/cleared source-global dimming guard");
  if(repeat==0)
  {
   for(unsigned i=0;i<160;++i)view.Step(input,1);Check(menus.Status().title->elapsed==160&&!menus.Status().pending_scene,"Idle Intro22 fired at160");view.Step(input,.01f);
   const auto stopped=menus.Status();Check(stopped.pending_scene==22&&stopped.pending_title_service==FrontendTitleCommandKind::IntroMovie&&stopped.title&&stopped.title->started_demo&&stopped.title->elapsed==0&&!stopped.pointer_enabled&&music->Status().source_state==6,"Original idle StopMusic/pointer-disable/Intro22 pending order differs");
   const auto frame=view.shown;const auto reads=music->Status().requested_reads;for(unsigned i=0;i<5;++i){view.Step(input,1);Check(view.shown.menu==frame.menu&&menus.Status().title->elapsed==0&&music->Status().requested_reads==reads,"Unavailable Intro22 replayed source/music or fabricated progress");}
  }
  menus.Release();music->CancelPending();cameras.Release();core.Release();Check(audio->Handles().empty(),"Repeated Title scope leaked cues");
 }
 Check(dimming->Admit(0)&&mode==0&&!dimming->Status().armed&&changes==2,"Explicit real host restore failed after Title scope teardown");music->Unload();audio->Unload();
}
void PendingMusic(bool owned,const Data& script,CameraAssetLibrary& library,const std::filesystem::path& root)
{
 FrontendInput input;Pads(input);unsigned seed=337;OriginalCameras core;FrontendCameras cameras(core,library);cameras.Push("startidle");auto audio=Audio(owned);auto music=Music(owned);auto prefs=Preferences(root/"title-pending-music-preferences");
 music->BeginSelect(0,seed);const auto initial=music->Status();Check(initial.load==FrontendMusicLoadState::Loading&&initial.requested_reads>0,"Initial actual music request did not remain Loading");
 unsigned mode=0;auto dimming=std::make_shared<FrontendTitleDimming>([&](unsigned value){mode=value;return true;});
 FrontendMenuScenes menus(input,audio,seed,cameras,script,prefs,[&](unsigned i){music->BeginSelect(i,seed);},{},[]{},0,FrontendLanguage::English,{},{},{},FrontendMenuTitleServices{music,dimming});Presentation view{menus};view.Load();
 Check(mode==2&&music->Status().load==FrontendMusicLoadState::Loading&&music->Status().requested_reads==initial.requested_reads&&menus.Status().title->admitted_operations==menus.Status().title->operations.size(),"Integrated Title replayed or claimed readiness for the same pending music0 selection");
 view.Step(input,.75f);view.Step(input,.75f);Pads(input);Pads(input,0x100);menus.Update(0);view.Ack();
 for(unsigned i=0;i<1200&&menus.Status().scene!=1;++i){view.Step(input,1.f/60);SDL_Delay(1);}
 Check(menus.Status().scene==1&&menus.Status().pending_main_music&&!menus.Status().interactive&&music->Status().load==FrontendMusicLoadState::Loading&&music->Status().requested_reads==initial.requested_reads,"Main replaced another pending cue or fabricated music admission");
 const auto arrived=view.shown.menu;PumpMusic(*music);menus.Service();view.Ack();Check(!menus.Status().pending_main_music&&music->Status().load==FrontendMusicLoadState::Loading&&view.shown.menu==arrived,"Real pending source completion did not admit original Main music1");PumpMusic(*music);
 Check(music->Status().cue==0x445abf3a&&music->Status().submitted_frames>0,"Main pending music release missed actual output");menus.Release();music->Unload();audio->Unload();cameras.Release();core.Release();
}
struct Host{bool live=false,disc=false;~Host(){if(live)ResetStartupFiles();if(disc)aurora_dvd_close();if(live){ResetStartupMemory();aurora_shutdown();}}};
}
int main(int argc,char** argv)
{
 try
 {
  if(argc==3&&std::string_view(argv[1])=="--fixture"){std::filesystem::create_directories(argv[2]);auto f=frontend_music_fixture::Make();Save(std::filesystem::path(argv[2])/"music.resbun",f.metadata);Save(std::filesystem::path(argv[2])/"music.nlxwb",f.wave);Save(std::filesystem::path(argv[2])/"calculation.bun",frontend_music_fixture::Calculation());return 0;}
  Check(argc==4,"Supply Title flow disc/output/generated|owned");const auto mode=std::string_view(argv[3]);const bool owned=mode=="owned";Check(owned||mode=="generated","Unknown Title flow mode");const auto root=std::filesystem::absolute(argv[2]);std::filesystem::create_directories(root);
  const auto folder=(root/"host").string();AuroraConfig config{};config.appName="Charged actual selected Title/Main flow";config.userPath=config.cachePath=folder.c_str();config.resourcesPath=SDL_GetBasePath();config.desiredBackend=BACKEND_NULL;config.windowWidth=320;config.windowHeight=240;config.windowPosX=config.windowPosY=-1;config.mem1Size=MEM1_DEFAULT_SIZE;config.mem2Size=64*1024*1024;config.logLevel=LOG_WARNING;Host host;const auto state=aurora_initialize(argc,argv,&config);host.live=true;Check(state.window,"Aurora core failed");InitializeStartupOS();nlInitMemory();Check(aurora_dvd_open(argv[1]),"Title flow disc unavailable");host.disc=true;nlInitFileSystem();
  const auto a=StandardAllocator.TotalFreeMemory(),b=VirtualAllocator.TotalFreeMemory();{
   CameraAssetLibrary library;Catalog(library);const auto script=Load("/Art/scripts/fe_presentation.byte_code");DimmingAuthority();MissingServices(owned,script,library,root);Flow(owned,script,library,root);IdleAndRepeated(owned,script,library,root);PendingMusic(owned,script,library,root);
  }Check(!nlAsyncReadsPending(nullptr)&&a==StandardAllocator.TotalFreeMemory()&&b==VirtualAllocator.TotalFreeMemory(),"Title flow NL work/native arenas did not recover");std::cout<<checks<<" selected Title/Main source-order/VM/queue/resource checks passed; actual dimming provider/Intro22/fullSceneCreated remain explicit\n";return 0;
 }
 catch(const std::exception& e){allocation_budget=-1;std::cerr<<"FAILED: "<<e.what()<<" (check "<<checks<<")\n";return 1;}
}
