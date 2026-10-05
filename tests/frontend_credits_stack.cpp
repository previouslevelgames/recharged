#include "runtime/frontend_credits.h"
#include "resources/credits_text.h"
#include "Game/FE/FrontendCreditsSteps.h"
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
 const std::array keys{0xf394c076u,0xbb142b94u};Data root;
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

std::shared_ptr<FrontendSession> Session(const char* path="/Art/fe/credits.fen")
{
 auto s=std::make_shared<FrontendSession>();s->Begin({path,FrontendLanguage::English,FrontendImageProfile::Main,"NINTENDO",true});
 const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(20);while(s->State()==FrontendSessionState::Loading){s->Service();Check(std::chrono::steady_clock::now()<deadline,"Credits NL load timed out");SDL_Delay(1);}s->Result();return s;
}
std::string Active(const FrontendSession::Handle& f)
{for(const auto& p:f->graph.slides)if(p.offset==f->graph.active_slide)return p.name;throw std::logic_error("No presentation");}
std::string Fade(const FrontendSession::Handle& f)
{
 const std::array<std::string_view,2> names{"Layer","WHITE FADE"};auto node=FindFrontendNode(f->graph,{FrontendNodeKind::Slide,*f->graph.active_slide},FrontendNamedPath(names));Check(bool(node),"Actual whitefade absent");
 const FrontendInstance* i=nullptr;for(const auto& n:f->graph.instances)if(n.offset==node->id)i=&n;Check(i&&i->type==4&&i->library,"Actual whitefade is not component");
 for(const auto& l:f->graph.library)if(l.offset==*i->library)for(const auto& s:f->graph.slides)if(s.offset==l.active_slide)return s.name;throw std::logic_error("Fade active slide absent");
}
struct Controls
{
 bool pointer=true,stadium=true;unsigned music_stops=0;std::vector<FrontendCreditsCommand> commands;
 int fail=-1;FrontendCredits* owner=nullptr;bool try_reentry=false;
 void Apply(FrontendCreditsCommand c)
 {
  if(fail==0){fail=-1;throw std::runtime_error("Explicit host-service failure");}if(fail>0)--fail;
  commands.push_back(c);if(try_reentry&&owner)Reject([&]{owner->Release();});
  switch(c.kind){case FrontendCreditsCommandKind::PointerEnabled:pointer=c.argument;break;case FrontendCreditsCommandKind::StadiumRendering:stadium=c.argument;break;case FrontendCreditsCommandKind::StopMusic:++music_stops;break;default:throw std::logic_error("No movie-return provider installed");}
 }
};
FrontendStackRequest Request(){FrontendStackRequest r;r.scene=23;r.initial_slide="NINTENDO";r.resources_mode=FrontendSessionResourcesMode::PermanentMain;return r;}
void Pump(FrontendSceneStack& stack,std::uint64_t id)
{const auto end=std::chrono::steady_clock::now()+std::chrono::seconds(20);while(stack.Entry(id).state==FrontendStackState::Queued||stack.Entry(id).state==FrontendStackState::Loading){stack.Service();Check(std::chrono::steady_clock::now()<end,"Credits stack resource load timed out");SDL_Delay(1);}stack.RethrowFailure(id);}
void Input(FrontendInput& i,unsigned button=0){std::array<FrontendPadSample,4> s{};for(auto& p:s)p.connected=true;s[0].buttons=button;i.Update(s,0);}
float TimeAfter(const FrontendSession::Handle& f,float delta)
{
 const auto s=std::find_if(f->graph.slides.begin(),f->graph.slides.end(),[&](const auto& v){return v.offset==f->graph.active_slide;});Check(s!=f->graph.slides.end(),"Clock oracle active slide absent");
 float time=f->graph.presentation_time+delta;const float end=s->start+s->duration;
 if(s->play_mode==0)time=std::min(time,end);else if(s->play_mode==1&&time>end)time-=end;return time;
}
void Proofs()
{
 FrontendInput input;Input(input);auto session=Session();FrontendHandler first(session,input),other(session,input);
 auto before=session->Current();auto proof=first.UpdateOnce(before,.125f);Near(proof.Delta(),.125f);Near(proof.After()->graph.presentation_time,TimeAfter(before,.125f));
 Reject([&]{other.ConsumeUpdate(std::move(proof),before);});Check(proof.Before()==before,"Foreign handler consumed Credits proof");auto moved=std::move(proof);Reject([&]{first.ConsumeUpdate(std::move(proof),before);});first.ConsumeUpdate(std::move(moved),before);Reject([&]{first.ConsumeUpdate(std::move(moved),before);});
}
void Lifecycle(bool owned)
{
 FrontendInput input;Input(input);auto audio=Audio(owned);unsigned seed=811,drains=0;bool reject_drain=false;
 FrontendSceneStack stack(input,[&]{++drains;if(reject_drain){reject_drain=false;throw std::runtime_error("Actual graphics drain unavailable");}});
 Controls controls;std::shared_ptr<FrontendCredits> credits;std::shared_ptr<FrontendHandler> handler;std::shared_ptr<FrontendSession> session;
 auto id=stack.QueuePush(Request());stack.BindVisual(id,[&](auto c){session=c.session;handler=c.handler;credits=std::make_shared<FrontendCredits>(c.session,input,audio,seed,[&](auto command){controls.Apply(command);},c.handler);return credits;});Pump(stack,id);
 auto e=stack.Entry(id);Check(e.state==FrontendStackState::AwaitingPublication&&e.handler_scope==FrontendStackHandlerScope::SelectedVisual&&e.subhandlers==FrontendStackSubhandlers::SourceEmpty&&!e.full_scene_created&&!stack.AllReady(),"Credits stack fabricated full handler readiness");
 const auto initial=credits->Current();unsigned calls=0;stack.Update(1,[&](auto,const auto&){++calls;});Check(!calls&&credits->Current()==initial,"Unpresented Credits advanced");stack.Publish(id,initial);
 Reject([&]{credits->Update(initial,0);});Reject([&]{credits->Release();});Reject([&]{handler->UpdateOnce(initial,0);});Reject([&]{credits->Button(initial,FrontendAction::Accept,FrontendButtonQuery::Held);});
 // Credits source differs from Audio14/Visual15: locks suppress queries only,
 // not the original base/timing update itself.
 FrontendHandler lock(session,input);lock.SetExclusiveInput(true);Input(input,0x100);const auto old_seed=seed;const auto old_sounds=audio->Handles().size();
 stack.Update(.125f,[&](auto token,const auto& shown){++calls;Check(token==id&&shown==initial,"Credits input did not retain actual prior publication");Check(!credits->Button(shown,FrontendAction::Accept,FrontendButtonQuery::Held),"Locked Credits received FEInput");Reject([&]{credits->Button(credits->Current(),FrontendAction::Accept,FrontendButtonQuery::Held);});});stack.RethrowFailure(id);
 Check(calls==1&&credits->Current()!=initial&&credits->Status().elapsed==.125f&&seed==old_seed&&audio->Handles().size()==old_sounds&&stack.Entry(id).published==initial,"Credits input lock incorrectly froze clocks/proof or mutated audio");Near(credits->Current()->graph.presentation_time,TimeAfter(initial,.125f));lock.SetExclusiveInput(false);lock.Release();
 stack.Publish(id,credits->Current());auto shown=credits->Current();Input(input);Input(input,0x100);
 stack.Update(.125f,[&](auto,const auto& frame){++calls;Check(frame==shown&&credits->Button(frame,FrontendAction::Accept,FrontendButtonQuery::Pressed),"Original Credits focus/button delivery failed");Reject([&]{credits->Update(frame,0);});Reject([&]{credits->Release();});Reject([&]{handler->UpdateOnce(credits->Current(),0);});Reject([&]{stack.Update(0);});});stack.RethrowFailure(id);Check(credits->Status().elapsed==.25f,"Credits source update ran more than once");Near(credits->Current()->graph.presentation_time,TimeAfter(shown,.125f));
 Reject([&]{stack.Publish(id,shown);});stack.Publish(id,credits->Current());shown=credits->Current();Input(input);
 stack.Update(2.75f);stack.RethrowFailure(id);Check(credits->Status().phase==0&&credits->Status().fade_started&&(owned?credits->Status().default_fade:Fade(credits->Current())=="FADEIN"),"Credits stack changed exact source fade endpoint");stack.Publish(id,credits->Current());
 stack.Update(0);stack.RethrowFailure(id);Check(credits->Status().phase==1&&credits->Status().movie&&credits->Status().boundary==FrontendCreditsBoundary::MoviePlayback,"Credits source did not request real movie boundary");stack.Publish(id,credits->Current());
 const auto movie=credits->Current();const auto movie_status=credits->Status();const auto movie_seed=seed;calls=0;
 stack.Update(1,[&](auto,const auto&){++calls;});stack.RethrowFailure(id);Check(!calls&&credits->Current()==movie&&handler->Current()==movie&&seed==movie_seed&&credits->Status().elapsed==movie_status.elapsed&&stack.Entry(id).state==FrontendStackState::Published,"Missing movie provider consumed base/input or invented completion");
 std::weak_ptr<FrontendSession> life=session;session.reset();reject_drain=true;Reject([&]{stack.Release();});Check(!controls.pointer&&!controls.stadium&&credits->Current()==movie&&!life.expired(),"Failed graphics drain released Credits resources/services");
 stack.Release();Check(controls.pointer&&controls.stadium&&audio->Handles().empty()&&life.expired()&&movie->images&&movie->visuals,"Credits stack teardown lost restoration/retention");Reject([&]{credits->Current();});Reject([&]{handler->Current();});Check(drains>3,"Credits publications did not drain");audio->Unload();
}
void Failures(bool owned)
{
 FrontendInput input;Input(input);auto audio=Audio(owned);unsigned seed=29;FrontendSceneStack stack(input,[]{});Controls c;
 bool created=false;auto cancel=stack.QueuePush(Request());stack.BindVisual(cancel,[&](auto)->std::shared_ptr<FrontendStackVisual>{created=true;throw std::runtime_error("Cancelled factory");});stack.Poll();stack.Cancel(cancel);Check(!created&&!nlAsyncReadsPending(nullptr),"Cancelled Credits ran handler or retained NL workers");
 for(unsigned mode=0;mode<2;++mode)
 {
  std::shared_ptr<FrontendCredits> owner;std::shared_ptr<FrontendSession> session;
  auto id=stack.QueuePush(Request());stack.BindVisual(id,[&](auto v){session=v.session;owner=std::make_shared<FrontendCredits>(v.session,input,audio,seed,[&](auto command){c.Apply(command);},v.handler);return owner;});Pump(stack,id);stack.Publish(id,owner->Current());auto old=owner->Current();
  stack.Update(.125f,[&](auto,const auto&){if(mode==0)throw std::runtime_error("Genuine input callback failure");session->Advance(.125f);});Reject([&]{stack.RethrowFailure(id);});Check(stack.Entry(id).state==FrontendStackState::Failed&&stack.Entry(id).published==old&&owner->Status().failed,"Failed or unauthorized input published Credits candidate");stack.QueuePop(id);stack.Poll();Check(c.pointer&&c.stadium&&audio->Handles().empty(),"Failed Credits stack cleanup missed restoration/audio");session.reset();
 }
 stack.Release();audio->Unload();
}
void Missing()
{
 FrontendInput input;Input(input);FrontendSceneStack stack(input,[]{});auto id=stack.QueuePush(Request());bool made=false;stack.BindVisual(id,[&](auto)->std::shared_ptr<FrontendStackVisual>{made=true;throw std::logic_error("Missing source ran constructor");});Reject([&]{Pump(stack,id);});const auto e=stack.Entry(id);Check(!made&&e.state==FrontendStackState::Failed&&!e.prepared&&!e.published&&!stack.AllReady(),"Missing Credits read fabricated source initialization");stack.QueuePop(id);stack.Poll();stack.Release();
}
struct Host{bool live=false,disc=false;~Host(){if(live)ResetStartupFiles();if(disc)aurora_dvd_close();if(live){ResetStartupMemory();aurora_shutdown();}}};
}
int main(int argc,char** argv)
{
 try
 {
  Check(argc==4,"Supply disc/output/generated or owned or missing");const bool owned=std::string_view(argv[3])=="owned",missing=std::string_view(argv[3])=="missing";const auto output=std::filesystem::absolute(argv[2]);const auto folder=(output/"credits-stack-host").string();std::filesystem::create_directories(folder);
  AuroraConfig config{};config.appName="Charged retained Credits stack";config.userPath=config.cachePath=folder.c_str();config.resourcesPath=SDL_GetBasePath();config.desiredBackend=BACKEND_NULL;config.windowWidth=320;config.windowHeight=240;config.windowPosX=config.windowPosY=-1;config.mem1Size=MEM1_DEFAULT_SIZE;config.mem2Size=64*1024*1024;config.logLevel=LOG_WARNING;
  Host host;auto result=aurora_initialize(argc,argv,&config);host.live=true;Check(result.window,"Aurora init failed");InitializeStartupOS();nlInitMemory();Check(aurora_dvd_open(argv[1]),"Credits disc absent");host.disc=true;nlInitFileSystem();
  for(unsigned i=0;i<(missing?1u:2u);++i){const auto a=StandardAllocator.TotalFreeMemory(),b=VirtualAllocator.TotalFreeMemory();if(missing)Missing();else{Proofs();Lifecycle(owned);if(!i)Failures(owned);}Check(!nlAsyncReadsPending(nullptr)&&a==StandardAllocator.TotalFreeMemory()&&b==VirtualAllocator.TotalFreeMemory(),"Credits stack resources did not recover native arenas");}
  std::cout<<checks<<" retained Credits stack checks passed\n";
 }
 catch(const std::exception& e){allocation_budget=-1;std::cerr<<"FAILED: "<<e.what()<<" (check "<<checks<<")\n";return 1;}
}
