#include "runtime/frontend_audio_options.h"
#include "runtime/frontend_handler.h"
#include "runtime/frontend_music.h"
#include "runtime/frontend_stack.h"
#include "runtime/startup.h"
#include "runtime/startup_files.h"
#include "audio_bank_fixture.h"
#include "frontend_music_fixture.h"
#include "NL/nlFileGC.h"
#include "NL/MemAlloc.h"
#include "Game/FE/FrontendAudioOptionsSteps.h"
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
  constexpr std::array keys{0x96deb5c3u,0x3021a1eeu,0x1f824c84u,0x304fdd1eu,0xf0afd586u};Data root;
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
void SourceSaveOrder()
{
 // Independent event oracle for the selected original prefix; the actual save
 // service is intentionally outside this helper and cannot be reported here.
 std::vector<unsigned> events;
 struct Field{std::vector<unsigned>& events;unsigned expected,event;void operator=(unsigned value){Check(value==expected,"Original Save assignment differs");events.push_back(event);}};
 struct Slide{std::vector<unsigned>& events;void SetActiveSlide(const char* name,bool reset){Check(std::string_view(name)=="OPTIONS_OUT"&&reset,"Original Save slide differs");events.push_back(3);}} slide{events};
 struct Button{std::vector<unsigned>& events;void SetActiveSlide(const char* name,bool reset,bool preserve){Check(std::string_view(name)=="down"&&reset&&!preserve,"Original Save feedback differs");events.push_back(5);}} button{events};
 struct Navigation{std::vector<unsigned>& events;void SetButtons(unsigned mask,bool reset){Check(mask==0&&reset,"Original Save NAV differs");events.push_back(2);}} navigation{events};
 struct Scene{Field mState;Slide* mPresentation;Field mSaveStarted;Button* mSaveButton;}scene{{events,3,1},&slide,{events,1,4},&button};
 FrontendAudioOptionsSaveVisual(scene,[&]{return &navigation;},[&](unsigned long cue,const char* name,void* context,bool restart){Check(!name&&!context&&restart,"Original Save cue contract differs");events.push_back(cue);});
 Check(events==std::vector<unsigned>{1,2,3,4,5,0xf0afd586,0x304fdd1e},"Original Save event order differs");
}
void SelectedStack(bool owned,const std::filesystem::path& folder,unsigned index)
{
 FrontendInput input;std::array<FrontendPadSample,4> pads{};pads[0].connected=true;input.Update(pads,0);
 auto preferences=Preferences(folder,index);auto volumes=std::make_shared<AudioCategoryVolumes>(ReadAudioVolumeProfile(owned?Load("/audio/nlxgs.bun"):Calculation()),preferences->Current()->audio);auto audio=Audio(owned,volumes);unsigned seed=71,drains=0;
 FrontendSceneStack stack(input,[&]{++drains;});std::shared_ptr<FrontendAudioOptions> owner;std::shared_ptr<FrontendHandler> base;std::shared_ptr<FrontendSession> session;
 auto token=stack.QueuePush(StackRequest());stack.BindVisual(token,[&](auto context){session=context.session;base=context.handler;owner=std::make_shared<FrontendAudioOptions>(session,input,audio,volumes,seed,base,0);return owner;});StackPump(stack,token);
 std::thread foreign([&]{Reject([&]{owner->Current();});Reject([&]{stack.Update(0);});});foreign.join();
 auto entry=stack.Entry(token);Check(entry.handler_scope==FrontendStackHandlerScope::SelectedVisual&&entry.subhandlers==FrontendStackSubhandlers::SourceEmpty&&!entry.full_scene_created,"Audio visual manufactured complete SceneCreated");Publish(stack,token,*owner);auto visible=owner->Current();
 Reject([&]{owner->AdvanceVisual(visible,0);});Reject([&]{base->Update(visible,0);});Reject([&]{owner->Release();});Reject([&]{base->Release();});
 // A real unrelated focus lock must block before clock/focus/proof/input work.
 FrontendHandler lock(session,input);lock.SetExclusiveInput(true);unsigned calls=0;const auto before_time=visible->graph.presentation_time;
 stack.Update(.125f,[&](auto,const auto&){++calls;});stack.RethrowFailure(token);Check(calls==0&&owner->Current()==visible&&stack.Entry(token).state==FrontendStackState::Published&&owner->Current()->graph.presentation_time==before_time,"Audio input lock ran base or post-base work");lock.SetExclusiveInput(false);lock.Release();
 stack.Update(.125f,[&](auto id,const auto& frame){++calls;Check(id==token&&frame==visible&&owner->Current()!=visible,"Audio stack did not provide old presented frame after base");Reject([&]{base->Update(owner->Current(),0);});Reject([&]{owner->AdvanceVisual(owner->Current(),0);});Reject([&]{owner->Acknowledge(owner->Current(),{1,640,480,640,480,0,0,640,480});});Reject([&]{stack.Poll();});});stack.RethrowFailure(token);
 Check(calls==1&&owner->Current()->graph.presentation_time==before_time+.125f&&stack.Entry(token).published==visible,"Audio base advanced more than once or published early");auto pending=owner->Current();stack.Update(1);Check(owner->Current()==pending,"Unpublished Audio candidate advanced");Publish(stack,token,*owner);
 for(unsigned n=0;owner->Status().state==0&&n<100;++n){stack.Update(1.f/60);stack.RethrowFailure(token);Publish(stack,token,*owner);}Check(owner->Status().state==1&&owner->Status().initialized,"Audio authored intro never became interactive");
 visible=owner->Current();const auto point=Center(owner->Bounds()[1]);Reject([&]{owner->DeliverPointer(visible,{0,point,true});});
 stack.Update(0,[&](auto,const auto& frame){Check(frame==visible,"Input used unpresented candidate");owner->DeliverPointer(frame,{0,point,true});});stack.RethrowFailure(token);Check(owner->Status().settings==std::array{6,5,5}&&volumes->Snapshot().settings==std::array{6,5,5}&&stack.Entry(token).published==visible,"Audio stack press lost current mutation or publication barrier");Publish(stack,token,*owner);volumes->Update(1,0);audio->Update(0);audio->ServiceAudio();
 visible=owner->Current();auto previous=*preferences->Current();
 // Missing save authority and still-unobserved storage never mutate the scene.
 stack.Update(0,[&](auto,const auto& frame){auto unobserved=std::make_shared<NativePreferences>(std::filesystem::absolute(folder/"unobserved.bin"));Reject([&]{owner->SaveNativePreferences(frame,unobserved);});Reject([&]{owner->SaveNativePreferences(frame,{});});Reject([&]{owner->Save(frame);});Check(!owner->Status().native_save_admitted,"Rejected host/original save admitted a request");});stack.RethrowFailure(token);Publish(stack,token,*owner);
 visible=owner->Current();stack.Update(0,[&](auto,const auto& frame){owner->SaveNativePreferences(frame,preferences);});stack.RethrowFailure(token);
 Check(owner->Status().native_save_admitted&&owner->Status().state==3&&preferences->Status().host_pending&&*preferences->Current()==previous,"Native save did not retain pending host ownership before visual publication");
 const auto commands=owner->Status().commands;Check(commands.size()==2&&commands[0].kind==FrontendAudioOptionsCommandKind::ResetNavigation&&commands[1].kind==FrontendAudioOptionsCommandKind::DoneDown,"Source save NAV command order differs");Check(audio->ActiveCount(0xf0afd586)==1&&audio->ActiveCount(0x304fdd1e)==1,"Source save cue ordering/count differs");Check(stack.Entry(token).published==visible,"Save silently presented candidate");
 Publish(stack,token,*owner);PreferencesPump(*preferences);auto saved=*preferences->Current();Check(saved.audio==std::array{6,5,5}&&saved.audio_defaults==previous.audio_defaults&&saved.auto_zoom==previous.auto_zoom&&saved.camera_zoom==previous.camera_zoom&&!preferences->Status().full_save_complete&&!preferences->Status().original_normal_save_loaded,"Audio native save lost visual/default fields or claimed original save completion");
 bool push=false;for(unsigned n=0;n<100&&!push;++n){stack.Update(1.f/60);stack.RethrowFailure(token);for(auto c:owner->Status().commands)if(c.kind==FrontendAudioOptionsCommandKind::PushOptions){Check(c.argument==13,"Audio out requested wrong scene");push=true;}Publish(stack,token,*owner);}Check(push,"Audio out did not request Options13");
 auto retained=owner->Current();std::weak_ptr<FrontendSession> lifetime=session;session.reset();stack.QueuePop(token);stack.Poll();Reject([&]{owner->Current();});Reject([&]{base->Current();});Check(lifetime.expired()&&retained->visuals&&retained->images&&audio->Handles().empty()&&drains>1,"Popped Audio owner kept base/source or lost retained output");stack.Release();audio->Unload();
}
void Failures(bool owned,const std::filesystem::path& folder)
{
 FrontendInput input;std::array<FrontendPadSample,4> pads{};pads[0].connected=true;input.Update(pads,0);auto volumes=std::make_shared<AudioCategoryVolumes>(ReadAudioVolumeProfile(owned?Load("/audio/nlxgs.bun"):Calculation()),std::array{5,5,5});auto audio=Audio(owned,volumes);unsigned seed=1;
 FrontendSceneStack stack(input,[]{});auto cancelled=stack.QueuePush(StackRequest());bool called=false;stack.BindVisual(cancelled,[&](auto c){called=true;return std::make_shared<FrontendAudioOptions>(c.session,input,audio,volumes,seed,c.handler);});stack.Cancel(cancelled);stack.Poll();Check(!called,"Cancelled Audio factory ran");
 std::shared_ptr<FrontendAudioOptions> owner;auto token=stack.QueuePush(StackRequest());stack.BindVisual(token,[&](auto c){owner=std::make_shared<FrontendAudioOptions>(c.session,input,audio,volumes,seed,c.handler);return owner;});StackPump(stack,token);Publish(stack,token,*owner);for(unsigned n=0;owner->Status().state==0&&n<100;++n){stack.Update(.1f);stack.RethrowFailure(token);Publish(stack,token,*owner);}
 auto preferences=Preferences(folder,55);auto visible=owner->Current();audio->Enable(false);stack.Update(0,[&](auto,const auto& frame){owner->SaveNativePreferences(frame,preferences);});Reject([&]{stack.RethrowFailure(token);});Check(stack.Entry(token).published==visible&&stack.Entry(token).state==FrontendStackState::Failed&&!preferences->Status().host_pending&&!owner->Status().native_save_admitted,"Failed real save cue published scene or storage success");stack.QueuePop(token);stack.Poll();audio->Enable(true);
 // A real post-base callback exception retains the already presented generation.
 token=stack.QueuePush(StackRequest());stack.BindVisual(token,[&](auto c){owner=std::make_shared<FrontendAudioOptions>(c.session,input,audio,volumes,seed,c.handler);return owner;});StackPump(stack,token);Publish(stack,token,*owner);visible=owner->Current();
 stack.Update(0,[&](auto,const auto&){throw std::runtime_error("Actual caller input failure");});Reject([&]{stack.RethrowFailure(token);});Check(stack.Entry(token).published==visible&&stack.Entry(token).state==FrontendStackState::Failed,"Callback failure retired visible Audio frame");stack.QueuePop(token);stack.Poll();
 // Save admission is distinct from worker completion: an external edit must
 // fail the actual optimistic host-file guard, never overwrite the edit.
 token=stack.QueuePush(StackRequest());stack.BindVisual(token,[&](auto c){owner=std::make_shared<FrontendAudioOptions>(c.session,input,audio,volumes,seed,c.handler);return owner;});StackPump(stack,token);Publish(stack,token,*owner);for(unsigned n=0;owner->Status().state==0&&n<100;++n){stack.Update(.1f);stack.RethrowFailure(token);Publish(stack,token,*owner);}
 preferences=Preferences(folder,56);const auto original=preferences->Current();auto edited=*original;edited.camera_zoom=.25f;const auto bytes=EncodeNativePreferences(edited);SaveFile(folder/"audio-preferences56.bin",Data(bytes.begin(),bytes.end()));
 stack.Update(0,[&](auto,const auto& f){owner->SaveNativePreferences(f,preferences);});stack.RethrowFailure(token);Check(preferences->Status().host_pending&&owner->Status().native_save_admitted,"Real host save was not admitted");Publish(stack,token,*owner);Reject([&]{PreferencesPump(*preferences);});
 Check(preferences->Status().state==NativePreferencesState::Failed&&preferences->Current()==original&&preferences->Status().in_operation&&!preferences->Status().full_save_complete,"Failed host save published values or finished the operation");Reject([&]{preferences->DepartureBlocked(NativePreferencesScope::NativePreferences);});
 const auto path=folder/"audio-preferences56.bin";std::ifstream file(path,std::ios::binary);Data actual((std::istreambuf_iterator<char>(file)),{});Check(actual==Data(bytes.begin(),bytes.end()),"Audio save overwrote a concurrent host edit");
 std::weak_ptr<NativePreferences> retained=preferences;preferences.reset();Check(!retained.expired(),"Audio scene lost pending/failed host owner");stack.QueuePop(token);stack.Poll();Check(retained.expired(),"Removed Audio scene retained save owner");stack.Release();audio->Unload();
}
struct Host{bool live=false,disc=false;~Host(){if(live)ResetStartupFiles();if(disc)aurora_dvd_close();if(live){ResetStartupMemory();aurora_shutdown();}}};
}
int main(int argc,char** argv)
{
 try{SourceSaveOrder();Check(argc==4,"Supply disc/output/generated or owned");const bool owned=std::string_view(argv[3])=="owned";auto folder=std::filesystem::absolute(std::filesystem::path(argv[2])/"audio-stack-host");std::filesystem::create_directories(folder);const auto path=folder.string();AuroraConfig config{};config.appName="Charged retained Audio stack";config.userPath=config.cachePath=path.c_str();config.resourcesPath=SDL_GetBasePath();config.desiredBackend=BACKEND_NULL;config.windowWidth=320;config.windowHeight=240;config.windowPosX=config.windowPosY=-1;config.mem1Size=MEM1_DEFAULT_SIZE;config.mem2Size=64*1024*1024;config.logLevel=LOG_WARNING;
 Host host;auto result=aurora_initialize(argc,argv,&config);host.live=true;Check(result.window,"Aurora failed");InitializeStartupOS();nlInitMemory();Check(aurora_dvd_open(argv[1]),"Audio stack disc missing");host.disc=true;nlInitFileSystem();
 for(unsigned i=0;i<2;++i){const auto a=StandardAllocator.TotalFreeMemory(),b=VirtualAllocator.TotalFreeMemory();SelectedStack(owned,folder,i);if(i==0)Failures(owned,folder);Check(a==StandardAllocator.TotalFreeMemory()&&b==VirtualAllocator.TotalFreeMemory()&&!nlAsyncReadsPending(nullptr),"Audio stack did not recover arenas/reads");}std::cout<<checks<<" retained Audio stack/native preferences checks passed\n";
 }catch(const std::exception& e){allocation_budget=-1;std::cerr<<"FAILED: "<<e.what()<<" (check "<<checks<<")\n";return 1;}
}
