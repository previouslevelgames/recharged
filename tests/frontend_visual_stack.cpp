#include "runtime/frontend_visual_options.h"
#include "runtime/frontend_handler.h"
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
 const std::array keys{0x96deb5c3u,0x3021a1eeu,0xf6eb899eu,0x362f2841u,0x304fdd1eu,0xf0afd586u};Data root;
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

FrontendStackRequest Request(){FrontendStackRequest r;r.scene=15;r.initial_slide="OPTIONS_IN";return r;}
void Pump(FrontendSceneStack& stack,FrontendSceneStack::Token token)
{
 const auto end=std::chrono::steady_clock::now()+std::chrono::seconds(15);
 while(stack.Entry(token).state==FrontendStackState::Queued||stack.Entry(token).state==FrontendStackState::Loading)
 {stack.Service();Check(std::chrono::steady_clock::now()<end,"Visual stack real resource reads timed out");SDL_Delay(1);}stack.RethrowFailure(token);
}
FrontendPointerViewport Viewport(SDL_Window* window)
{int x,y,px,py;Check(SDL_GetWindowSize(window,&x,&y)&&SDL_GetWindowSizeInPixels(window,&px,&py),"SDL window query failed");return{SDL_GetWindowID(window),unsigned(x),unsigned(y),unsigned(px),unsigned(py),0,0,double(px),double(py)};}
void Publish(FrontendSceneStack& stack,std::uint64_t token,FrontendVisualOptions& owner,SDL_Window* window)
{auto f=stack.Entry(token).prepared;stack.Publish(token,f);owner.Acknowledge(f,Viewport(window));}
void Input(FrontendInput& input){std::array<FrontendPadSample,4> p{};for(auto& v:p)v.connected=true;input.Update(p,0);}
float TimeAfter(const FrontendSession::Handle& f,float delta)
{
 const auto s=std::find_if(f->graph.slides.begin(),f->graph.slides.end(),[&](const auto& v){return v.offset==f->graph.active_slide;});Check(s!=f->graph.slides.end(),"Clock oracle missing active slide");
 float time=f->graph.presentation_time+delta;const float end=s->start+s->duration;
 if(s->play_mode==0)time=std::min(time,end);else if(s->play_mode==1&&time>end)time-=end;return time;
}
auto Preferences(const std::filesystem::path& folder,unsigned index)
{
 const auto p=std::filesystem::absolute(folder/("visual-stack-prefs"+std::to_string(index)+".bin"));auto data=DefaultNativePreferences();data.audio={2,4,6};data.audio_defaults={9,8,7};data.auto_zoom=true;data.camera_zoom=.61f;
 const auto b=EncodeNativePreferences(data);{std::ofstream f(p,std::ios::binary);f.write(reinterpret_cast<const char*>(b.data()),b.size());Check(bool(f),"Synthetic preference write failed");}
 auto result=std::make_shared<NativePreferences>(p);result->StartLoad();const auto end=std::chrono::steady_clock::now()+std::chrono::seconds(10);while(result->Status().host_pending){result->Poll();Check(std::chrono::steady_clock::now()<end,"Preference load timed out");SDL_Delay(1);}result->RethrowFailure();return result;
}
void Proofs()
{
 FrontendInput input;Input(input);auto s=Session();FrontendHandler first(s,input),second(s,input);auto before=s->Current();auto proof=first.UpdateOnce(before,.125f);
 Near(proof.Delta(),.125f);Near(proof.After()->graph.presentation_time,TimeAfter(before,.125f));Reject([&]{second.ConsumeUpdate(std::move(proof),before);});Check(proof.Before()==before,"Foreign proof consumer stole receipt");
 auto moved=std::move(proof);Reject([&]{first.ConsumeUpdate(std::move(proof),before);});first.ConsumeUpdate(std::move(moved),before);Reject([&]{first.ConsumeUpdate(std::move(moved),before);});
 first.Release();second.Release();
}
void Lifecycle(bool owned,const std::filesystem::path& folder,SDL_Window* window,unsigned iteration)
{
 FrontendInput input;Input(input);auto audio=Audio(owned);auto settings=std::make_shared<FrontendVisualSettings>(true,.61f);unsigned seed=197,drains=0;FrontendSceneStack stack(input,[&]{++drains;});
 std::shared_ptr<FrontendVisualOptions> owner;std::shared_ptr<FrontendHandler> handler;std::shared_ptr<FrontendSession> session;
 auto token=stack.QueuePush(Request());stack.BindVisual(token,[&](auto c){session=c.session;handler=c.handler;owner=std::make_shared<FrontendVisualOptions>(session,input,audio,settings,seed,handler);return owner;});Pump(stack,token);
 auto entry=stack.Entry(token);Check(entry.handler_scope==FrontendStackHandlerScope::SelectedVisual&&entry.subhandlers==FrontendStackSubhandlers::SourceEmpty&&!entry.full_scene_created,"Visual scope fabricated full SceneCreated");Publish(stack,token,*owner,window);auto visible=owner->Current();
 Reject([&]{owner->AdvanceVisual(visible,0);});Reject([&]{handler->UpdateOnce(visible,0);});Reject([&]{owner->Release();});Reject([&]{handler->Release();});
 FrontendHandler lock(session,input);lock.SetExclusiveInput(true);unsigned calls=0;const auto original_seed=seed;const auto original_handles=audio->Handles().size();const auto original=settings->Snapshot();
 for(unsigned i=0;i<3;++i){stack.Update(.125f,[&](auto,const auto&){++calls;});stack.RethrowFailure(token);}
 Check(!calls&&owner->Current()==visible&&stack.Entry(token).state==FrontendStackState::Published&&handler->Current()==visible&&seed==original_seed&&audio->Handles().size()==original_handles&&settings->Snapshot()==original,"Pre-base lock changed clock/proof/input/settings/RNG/cues");lock.SetExclusiveInput(false);lock.Release();
 stack.Update(.125f,[&](auto id,const auto& frame){++calls;Check(id==token&&frame==visible&&owner->Current()!=visible,"Input did not follow exactly one base update");Near(owner->Current()->graph.presentation_time,TimeAfter(visible,.125f));Reject([&]{handler->UpdateOnce(owner->Current(),0);});Reject([&]{owner->AdvanceVisual(owner->Current(),0);});Reject([&]{owner->Acknowledge(owner->Current(),Viewport(window));});Reject([&]{stack.Poll();});});stack.RethrowFailure(token);
 Check(calls==1&&stack.Entry(token).published==visible,"Update silently acknowledged candidate");auto pending=owner->Current();stack.Update(.5f);Check(owner->Current()==pending,"Unpresented frame consumed a second base proof");Publish(stack,token,*owner,window);
 for(unsigned n=0;owner->Status().state==0&&n<100;++n){visible=owner->Current();stack.Update(.125f);stack.RethrowFailure(token);Near(owner->Current()->graph.presentation_time,TimeAfter(visible,.125f));Publish(stack,token,*owner,window);}
 Check(owner->Status().state==1&&owner->Status().initialized,"Visual authored intro did not complete");const auto bounds=owner->Bounds();visible=owner->Current();
 Reject([&]{owner->DeliverPointer(visible,{0,Center(bounds[0]),true});});
 // A pointer callback reads the old acknowledged bounds and edits only the proven new frame.
 stack.Update(0,[&](auto,const auto& frame){Check(frame==visible,"Pointer received candidate instead of presented geometry");owner->DeliverPointer(frame,{2,Center(bounds[4]),true});});stack.RethrowFailure(token);
 Check(settings->Snapshot().auto_zoom&&settings->Snapshot().zoom==1&&owner->Status().settings[1]==4&&stack.Entry(token).published==visible,"Visual pointer lost original setting or publication boundary");Check(owner->Current()!=visible,"Visual pointer reused the displayed generation");Label(owner->Current(),5);Label(visible,3);Publish(stack,token,*owner,window);
 // Lock also blocks an already active source state without consuming queued UI work.
 visible=owner->Current();const auto active_seed=seed;const auto active_settings=settings->Snapshot();const auto active_handles=audio->Handles().size();FrontendHandler active_lock(session,input);active_lock.SetExclusiveInput(true);
 stack.Update(1,[&](auto,const auto&){++calls;});stack.RethrowFailure(token);Check(owner->Current()==visible&&seed==active_seed&&settings->Snapshot()==active_settings&&audio->Handles().size()==active_handles,"Interactive lock admitted side effects");active_lock.SetExclusiveInput(false);active_lock.Release();
 // Real host focus policy clears hover and requires a neutral observation.
 std::uint64_t sequence=0;const auto viewport=Viewport(window);const auto point=Center(bounds[0]);FrontendPointerDesktopSample sample{viewport.window,viewport.window_width,viewport.window_height,viewport.pixel_width,viewport.pixel_height,0,7,(point[0]+320)*viewport.window_width/640,(240-point[1])*viewport.window_height/480,true,true,false,false};
 auto route=[&](bool focused,bool pressed){sample.sequence=++sequence;sample.focused=focused;sample.primary_down=pressed;FrontendPointerDispatch result;stack.Update(0,[&](auto,const auto&){result=owner->Route(sample);});stack.RethrowFailure(token);Publish(stack,token,*owner,window);return result;};
 Check(!route(true,false).active,"Initial host neutral observation became active");Check(route(true,false).active,"Focused neutral host did not become active");Check(!route(false,true).active,"Lost focus admitted pointer press");const auto focus_settings=settings->Snapshot();Check(!route(true,true).active&&settings->Snapshot()==focus_settings,"Held focus recovery changed settings");Check(!route(true,false).active&&route(true,false).active,"Focus recovery skipped its neutral gate");
 // Resize invalidates acknowledged extents; actual SDL Poll delivers inactive Leave.
 Check(SDL_SetWindowSize(window,int(viewport.window_width)+16,int(viewport.window_height)+8),"SDL resize failed");FrontendPointerDispatch resized;stack.Update(0,[&](auto,const auto&){resized=owner->Poll(window);});stack.RethrowFailure(token);Check(!resized.active&&resized.event.index==0&&resized.listeners==7&&resized.event.position[0]==-9999.9f&&resized.event.position[1]==-9999.9f,"Resize did not deliver inactive original offscreen position");Publish(stack,token,*owner,window);
 // Real Back restores original constructor quantization before the out gate.
 visible=owner->Current();stack.Update(0,[&](auto,const auto& frame){owner->NotifyBackButton(frame);});stack.RethrowFailure(token);Check(settings->Snapshot().auto_zoom&&settings->Snapshot().zoom==.5f&&owner->Status().state==3&&stack.Entry(token).published==visible,"Source Back restoration/publication differs");Publish(stack,token,*owner,window);
 bool request=false;for(unsigned n=0;n<100&&!request;++n){stack.Update(.125f);stack.RethrowFailure(token);for(auto c:owner->Status().commands)if(c.kind==FrontendVisualOptionsCommandKind::PushOptions){Check(c.argument==13,"Source visual return scene differs");request=true;}Publish(stack,token,*owner,window);}Check(request,"Visual out did not request fresh Options13 replacement");
 auto retained=owner->Current();std::weak_ptr<FrontendSession> lifetime=session;session.reset();stack.QueuePop(token);stack.Poll();Reject([&]{owner->Current();});Reject([&]{handler->Current();});Check(lifetime.expired()&&retained->visuals&&retained->images&&audio->Handles().empty()&&drains>2,"Visual stack removal lost ownership/drain semantics");
 // New source15 owner starts fresh. Native persistence remains explicit and preserves audio/default fields.
 auto prefs=Preferences(folder,iteration);const auto prior=*prefs->Current();auto save_settings=std::make_shared<FrontendVisualSettings>(prior.auto_zoom,prior.camera_zoom);auto next=stack.QueuePush(Request());
 stack.BindVisual(next,[&](auto c){owner=std::make_shared<FrontendVisualOptions>(c.session,input,audio,save_settings,seed,c.handler);return owner;});Pump(stack,next);Publish(stack,next,*owner,window);
 for(unsigned n=0;owner->Status().state==0&&n<100;++n){stack.Update(.125f);stack.RethrowFailure(next);Publish(stack,next,*owner,window);}
 stack.Update(0,[&](auto,const auto& frame){owner->DeliverPointer(frame,{0,Center(owner->Bounds()[1]),true});});stack.RethrowFailure(next);Publish(stack,next,*owner,window);
 visible=owner->Current();stack.Update(0,[&](auto,const auto& frame){
  bool unsupported=false;try{owner->Save(frame);}catch(const UnsupportedResource& e){unsupported=std::string_view(e.what()).find("full game-save service")!=std::string_view::npos;}Check(unsupported,"Original game-save request lost explicit unsupported boundary");
  owner->SaveNativePreferences(frame,prefs);});stack.RethrowFailure(next);Check(owner->Status().native_save_admitted&&prefs->Status().host_pending&&stack.Entry(next).published==visible,"Native save lost admitted unpublished ownership");Publish(stack,next,*owner,window);stack.QueuePop(next);stack.Poll();
 const auto end=std::chrono::steady_clock::now()+std::chrono::seconds(10);while(prefs->Status().host_pending){prefs->Poll();Check(std::chrono::steady_clock::now()<end,"Visual save did not complete after pop");SDL_Delay(1);}prefs->RethrowFailure();const auto saved=*prefs->Current();Check(saved.auto_zoom&&saved.camera_zoom==.25f&&saved.audio==prior.audio&&saved.audio_defaults==prior.audio_defaults&&!prefs->Status().original_normal_save_loaded&&!prefs->Status().full_save_complete,"Native visual save lost scope or untouched fields");stack.Release();audio->Unload();
}
void Failures(bool owned,SDL_Window* window)
{
 FrontendInput input;Input(input);auto audio=Audio(owned);auto settings=std::make_shared<FrontendVisualSettings>(true,.5f);unsigned seed=7;FrontendSceneStack stack(input,[]{});
 auto cancelled=stack.QueuePush(Request());bool created=false;stack.BindVisual(cancelled,[&](auto c){created=true;return std::make_shared<FrontendVisualOptions>(c.session,input,audio,settings,seed,c.handler);});stack.Cancel(cancelled);stack.Poll();Check(!created,"Cancelled Visual factory ran");
 std::shared_ptr<FrontendVisualOptions> owner;auto token=stack.QueuePush(Request());stack.BindVisual(token,[&](auto c){owner=std::make_shared<FrontendVisualOptions>(c.session,input,audio,settings,seed,c.handler);return owner;});Pump(stack,token);Publish(stack,token,*owner,window);auto old=owner->Current();
 stack.Update(0,[](auto,const auto&){throw std::runtime_error("genuine input callback failure");});Reject([&]{stack.RethrowFailure(token);});Check(stack.Entry(token).published==old&&stack.Entry(token).state==FrontendStackState::Failed,"Failed callback exposed unrendered candidate");stack.QueuePop(token);stack.Poll();Reject([&]{owner->Current();});
 token=stack.QueuePush(Request());stack.BindVisual(token,[&](auto c){owner=std::make_shared<FrontendVisualOptions>(c.session,input,audio,settings,seed,c.handler);return owner;});Pump(stack,token);Publish(stack,token,*owner,window);old=owner->Current();const auto before=old->graph.presentation_time;settings->Set(settings->Snapshot(),false,1);
 stack.Update(.5f);Reject([&]{stack.RethrowFailure(token);});Check(stack.Entry(token).published==old&&owner->Current()->graph.presentation_time==before,"Foreign settings mutation ran base before admission failure");stack.QueuePop(token);stack.Poll();stack.Release();audio->Unload();
}
void ReadFailure()
{
 FrontendInput input;Input(input);FrontendSceneStack stack(input,[]{});auto token=stack.QueuePush(Request());bool created=false;
 stack.BindVisual(token,[&](auto)->std::shared_ptr<FrontendStackVisual>{created=true;throw std::runtime_error("Factory must not run");});Reject([&]{Pump(stack,token);});
 Check(!created&&stack.Entry(token).state==FrontendStackState::Failed&&!stack.Entry(token).prepared&&!stack.Entry(token).published&&!stack.AllReady(),"Missing source read invented handler publication");stack.QueuePop(token);stack.Poll();stack.Release();
}
struct Host{bool live=false,disc=false;~Host(){if(live)ResetStartupFiles();if(disc)aurora_dvd_close();if(live){ResetStartupMemory();aurora_shutdown();}}};
}
int main(int argc,char** argv)
{
 try
 {
  Check(argc==4,"Supply disc/output/generated or owned or missing");const bool owned=std::string_view(argv[3])=="owned",missing=std::string_view(argv[3])=="missing";const auto output=std::filesystem::absolute(argv[2]);const auto folder=(output/"visual-stack-host").string();std::filesystem::create_directories(folder);
  AuroraConfig config{};config.appName="Charged retained Visual stack";config.userPath=config.cachePath=folder.c_str();config.resourcesPath=SDL_GetBasePath();config.desiredBackend=BACKEND_NULL;config.windowWidth=320;config.windowHeight=240;config.windowPosX=config.windowPosY=-1;config.mem1Size=MEM1_DEFAULT_SIZE;config.mem2Size=64*1024*1024;config.logLevel=LOG_WARNING;
  Host host;auto result=aurora_initialize(argc,argv,&config);host.live=true;Check(result.window,"Aurora init failed");InitializeStartupOS();nlInitMemory();Check(aurora_dvd_open(argv[1]),"Visual stack disc absent");host.disc=true;nlInitFileSystem();
  for(unsigned i=0;i<(missing?1u:2u);++i){const auto a=StandardAllocator.TotalFreeMemory(),b=VirtualAllocator.TotalFreeMemory();if(missing)ReadFailure();else{Proofs();Lifecycle(owned,output,result.window,i);if(!i)Failures(owned,result.window);}Check(!nlAsyncReadsPending(nullptr)&&a==StandardAllocator.TotalFreeMemory()&&b==VirtualAllocator.TotalFreeMemory(),"Visual stack did not recover native reads/arenas");}
  std::cout<<checks<<" retained Visual stack checks passed; full SceneCreated remains false\n";
 }
 catch(const std::exception& e){allocation_budget=-1;std::cerr<<"FAILED: "<<e.what()<<" (check "<<checks<<")\n";return 1;}
}
