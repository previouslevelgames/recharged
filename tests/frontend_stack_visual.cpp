#include "runtime/frontend_main_menu.h"
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
template<class F>void Reject(F f,std::source_location at=std::source_location::current()){++checks;try{f();}catch(const std::exception&){return;}throw std::runtime_error("Invalid NAV operation accepted at "+std::to_string(at.line()));}
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
 const std::array keys{0x6b0689d4u,0x0a93e9a0u,0xf0afd586u,0x304fdd1eu,0xf6eb899eu,0x4430b152u};Data root;
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
void Proofs()
{
 FrontendInput input;Input(input);auto session=Session();FrontendHandler a(session,input),b(session,input);auto old=session->Current();
 auto proof=a.UpdateOnce(old,.125f);Near(proof.Delta(),.125f);Near(proof.After()->graph.presentation_time-old->graph.presentation_time,.125f);
 Reject([&]{b.ConsumeUpdate(std::move(proof),old);});Check(proof.Before()==old,"Failed foreign consume lost proof");
 auto moved=std::move(proof);Reject([&]{a.ConsumeUpdate(std::move(proof),old);});
 auto current=a.ConsumeUpdate(std::move(moved),old);Check(current==session->Current(),"Base receipt lost exact output");Reject([&]{a.ConsumeUpdate(std::move(moved),old);});
 old=current;auto stale=a.UpdateOnce(old,.1f);auto newer=a.UpdateOnce(session->Current(),.2f);Reject([&]{a.ConsumeUpdate(std::move(stale),old);});
 auto before=newer.Before();Reject([&]{a.ConsumeUpdate(std::move(newer),old);});a.ConsumeUpdate(std::move(newer),before);
 old=session->Current();auto altered=a.UpdateOnce(old,0);session->Advance(0);Reject([&]{a.ConsumeUpdate(std::move(altered),old);});
 old=session->Current();auto released=a.UpdateOnce(old,0);a.Release();Reject([&]{a.ConsumeUpdate(std::move(released),old);});b.Release();
}
void Lifecycle(bool owned)
{
 FrontendInput input;Input(input);auto audio=Audio(owned);unsigned seed=987,drains=0;FrontendSceneStack stack(input,[&]{++drains;});
 std::shared_ptr<FrontendMainMenu> main;std::shared_ptr<FrontendHandler> main_base;std::shared_ptr<FrontendSession> main_session;
 auto request=Request(1);request.resources_mode=FrontendSessionResourcesMode::PermanentMain;auto token=stack.QueuePush(request);Reject([&]{stack.Resources(token);});stack.BindVisual(token,[&](auto context){main_base=context.handler;main_session=context.session;main=std::make_shared<FrontendMainMenu>(context.session,input,audio,seed,context.handler,false);return main;});Pump(stack,token);
 auto shared=stack.Resources(token);Check(bool(shared),"Permanent stack did not expose checked resources");auto entry=stack.Entry(token);Check(entry.handler_scope==FrontendStackHandlerScope::SelectedVisual&&entry.subhandlers==FrontendStackSubhandlers::SourceEmpty&&!entry.full_scene_created,"Selected visuals invented full handler readiness");
 Check(main->Current()==entry.prepared&&entry.state==FrontendStackState::AwaitingPublication,"Concrete factory frame was not retained");
 stack.Publish(token,entry.prepared);main->Acknowledge(entry.prepared,Viewport());const auto original=entry.prepared;
 Reject([&]{main->AdvanceVisual(main->Current(),.1f);});Reject([&]{main_base->Update(main->Current(),.1f);});Reject([&]{main->Release();});Reject([&]{main_base->Release();});
 unsigned calls=0;stack.Update(.125f,[&](auto id,const auto& published){++calls;Check(id==token&&published==original,"Input did not receive last rendered frame");Near(main->Current()->graph.presentation_time-original->graph.presentation_time,.125f);Reject([&]{main->AdvanceVisual(main->Current(),.125f);});Reject([&]{main_base->Update(main->Current(),.125f);});Reject([&]{stack.Poll();});Reject([&]{main->Acknowledge(main->Current(),Viewport());});});stack.RethrowFailure(token);
 Check(calls==1&&stack.Entry(token).published==original&&main->Current()==stack.Entry(token).prepared,"Update lost exact publication barrier");auto pending=main->Current();stack.Update(1);Check(main->Current()==pending,"Unpublished visual advanced twice");
 stack.Publish(token,pending);main->Acknowledge(pending,Viewport());
 unsigned updates=0;while(!main->Status().interactive&&updates++<1000){stack.Update(.125f);stack.RethrowFailure(token);auto frame=stack.Entry(token).prepared;stack.Publish(token,frame);main->Acknowledge(frame,Viewport());}
 Check(main->Status().interactive,"Main intro did not finish");auto visible=main->Current();const auto bounds=main->Bounds();
 Reject([&]{main->DeliverPointer(visible,{0,Center(bounds[6])});});
 stack.Update(.125f,[&](auto,const auto& presented){Check(presented==visible,"Pointer read candidate geometry");main->DeliverPointer(presented,{0,Center(bounds[6]),false});});stack.RethrowFailure(token);
 Check(main->Status().highlighted[0]>0&&main->Status().pointer_states[6][0]==1,"Post-base original Enter did not mutate current visual");Near(main->Current()->graph.presentation_time,ExpectedTime(visible,.125f));Check(stack.Entry(token).published==visible,"Pointer mutation silently acknowledged unrendered frame");
 auto hovered=main->Current();stack.Publish(token,hovered);main->Acknowledge(hovered,Viewport());
 stack.Update(0,[&](auto,const auto& presented){main->DeliverPointer(presented,{0,Center(bounds[6]),true});});stack.RethrowFailure(token);
 Check(main->Status().selection&&main->Status().selection->item==6&&main->Status().selection->source==hovered,"Options request lost exact presented source");
 stack.Publish(token,stack.Entry(token).prepared);main->Acknowledge(main->Current(),Viewport());
 // Independent Options owner shares its stack base, not Main's base or session.
 std::shared_ptr<FrontendOptions> options;auto option_request=Request(13);option_request.shared_resources=shared;auto options_token=stack.QueuePush(option_request);stack.BindVisual(options_token,[&](auto context){options=std::make_shared<FrontendOptions>(context.session,input,audio,seed,context.handler,0);return options;});Pump(stack,options_token);Check(stack.Resources(options_token)==shared&&options->Current()->visuals==main->Current()->visuals&&options->Current()->images==main->Current()->images&&options->Current()->image_completed_files==0,"Stack replacement duplicated shared assets or reads");stack.Publish(options_token,stack.Entry(options_token).prepared);options->Acknowledge(options->Current(),Viewport());
 unsigned steps=0;while(!options->Status().initialized&&steps++<1000)
 {
  auto old=options->Current();stack.Update(.125f);stack.RethrowFailure(options_token);Near(options->Current()->graph.presentation_time,ExpectedTime(old,.125f));
  for(const auto& e:stack.Entries())if(e.state==FrontendStackState::AwaitingPublication)stack.Publish(e.token,e.prepared);
  options->Acknowledge(options->Current(),Viewport());main->Acknowledge(main->Current(),Viewport());
 }
 Check(options->Status().state==1&&options->Status().initialized,"Options intro state differs");const auto options_visible=options->Current();const auto point=Center(options->Bounds()[0]);
 stack.Update(0,[&](auto id,const auto& presented){if(id==options_token){Check(presented==options_visible,"Options input frame differs");options->DeliverPointer(presented,{0,point,true});}});stack.RethrowFailure(options_token);
 Check(options->Status().state==2&&options->Status().next_scene==15&&!options->Status().transition,"Options press skipped original out gate");
 for(const auto& e:stack.Entries())if(e.state==FrontendStackState::AwaitingPublication)stack.Publish(e.token,e.prepared);
 options->Acknowledge(options->Current(),Viewport());main->Acknowledge(main->Current(),Viewport());
 steps=0;while(!options->Status().transition&&steps++<1000)
 {stack.Update(.125f);stack.RethrowFailure(options_token);for(const auto& e:stack.Entries())if(e.state==FrontendStackState::AwaitingPublication)stack.Publish(e.token,e.prepared);options->Acknowledge(options->Current(),Viewport());main->Acknowledge(main->Current(),Viewport());}
 Check(options->Status().transition&&options->Status().transition->scene==15,"Options did not stop at actual scene15 request");
 auto retained=options->Current();stack.QueuePop(options_token);stack.Poll();Reject([&]{options->Current();});Check(retained->visuals&&retained->images,"Stack pop invalidated retained resources");
 stack.QueuePop(token);stack.Poll();Reject([&]{main->Current();});Reject([&]{main_base->Current();});Check(stack.AllReady()&&drains>2,"Stack teardown left resources/commands");
 auto reuse=Request(1);reuse.shared_resources=shared;auto retained_token=stack.QueuePush(reuse);Pump(stack,retained_token);Check(stack.Resources(retained_token)==shared&&stack.Entry(retained_token).prepared->visuals==retained->visuals,"Shared owners did not survive donor pop");stack.Cancel(retained_token);
 stack.Release();options.reset();main.reset();main_base.reset();main_session.reset();Check(audio->Handles().empty(),"Popped visual left active cue references");audio->Unload();
}
void Failures(bool owned)
{
 FrontendInput input;Input(input);auto audio=Audio(owned);unsigned seed=123;FrontendSceneStack stack(input,[]{});
 auto cancelled=stack.QueuePush(Request(1));Check(!stack.Resources(cancelled),"Ordinary load invented a permanent token");bool created=false;stack.BindVisual(cancelled,[&](auto context){created=true;return std::make_shared<FrontendMainMenu>(context.session,input,audio,seed,context.handler,false);});stack.Cancel(cancelled);stack.Poll();Check(!created,"Cancelled factory ran");
 auto bad=stack.QueuePush(Request(1));stack.BindVisual(bad,[](auto)->std::shared_ptr<FrontendStackVisual>{throw std::runtime_error("real creation failure");});
 Reject([&]{Pump(stack,bad);});Check(stack.Entry(bad).state==FrontendStackState::Failed,"Factory failure manufactured readiness");stack.QueuePop(bad);stack.Poll();
 std::shared_ptr<FrontendMainMenu> main;auto token=stack.QueuePush(Request(1));stack.BindVisual(token,[&](auto context){main=std::make_shared<FrontendMainMenu>(context.session,input,audio,seed,context.handler,false);return main;});Pump(stack,token);stack.Publish(token,stack.Entry(token).prepared);main->Acknowledge(main->Current(),Viewport());auto visible=main->Current();
 stack.Update(0,[&](auto,const auto&){throw std::runtime_error("real input callback failure");});Reject([&]{stack.RethrowFailure(token);});Check(stack.Entry(token).state==FrontendStackState::Failed&&stack.Entry(token).published==visible,"Input failure discarded actual visible generation");stack.QueuePop(token);stack.Poll();Reject([&]{main->Current();});stack.Release();main.reset();audio->Unload();
}
void AllocationFailures()
{
 FrontendInput input;Input(input);auto audio=Audio(false);unsigned seed=43,failures=0;bool succeeded=false;
 for(long budget:{0L,1L,8L,64L,256L,1024L,4096L,16384L,65536L})
 {
  FrontendSceneStack stack(input,[]{});auto token=stack.QueuePush(Request(1));Pump(stack,token);auto resources=stack.Entry(token).prepared;
  stack.BindVisual(token,[&](auto context){return std::make_shared<FrontendMainMenu>(context.session,input,audio,seed,context.handler,false);});
  allocation_budget=budget;stack.Poll();allocation_budget=-1;
  if(stack.Entry(token).state==FrontendStackState::Failed)
  {++failures;Reject([&]{stack.RethrowFailure(token);});Check(!stack.Entry(token).published&&!stack.AllReady(),"Failed visual construction published readiness");}
  else {succeeded=true;Check(stack.Entry(token).state==FrontendStackState::AwaitingPublication,"Successful creation lost publication gate");}
  stack.Cancel(token);stack.Release();Check(resources->visuals&&resources->images,"Failed creation invalidated retained inputs");if(succeeded)break;
 }
 Check(succeeded&&failures>=3,"Concrete factory allocation rollback was not exercised");audio->Unload();
}
struct Host{bool live=false,disc=false;~Host(){if(live)ResetStartupFiles();if(disc)aurora_dvd_close();if(live){ResetStartupMemory();aurora_shutdown();}}};
}
int main(int argc,char** argv)
{
 try
 {
  Check(argc==4,"Supply disc/output/generated or owned");const bool owned=std::string_view(argv[3])=="owned";const auto folder=(std::filesystem::path(argv[2])/"stack-visual-host").string();std::filesystem::create_directories(folder);AuroraConfig config{};
  config.appName="Charged selected visual stack";config.userPath=config.cachePath=folder.c_str();config.resourcesPath=SDL_GetBasePath();config.desiredBackend=BACKEND_NULL;config.windowWidth=320;config.windowHeight=240;config.windowPosX=config.windowPosY=-1;config.mem1Size=MEM1_DEFAULT_SIZE;config.mem2Size=64*1024*1024;config.logLevel=LOG_WARNING;
  Host host;const auto h=aurora_initialize(argc,argv,&config);host.live=true;Check(h.window,"Aurora failed");InitializeStartupOS();nlInitMemory();Check(aurora_dvd_open(argv[1]),"Disc missing");host.disc=true;nlInitFileSystem();
  for(unsigned i=0;i<3;++i){const auto a=StandardAllocator.TotalFreeMemory(),b=VirtualAllocator.TotalFreeMemory();Proofs();Lifecycle(owned);if(i==0){Failures(owned);if(!owned)AllocationFailures();}Check(!nlAsyncReadsPending(nullptr)&&a==StandardAllocator.TotalFreeMemory()&&b==VirtualAllocator.TotalFreeMemory(),"Selected stack teardown did not restore arenas/files");}
  std::cout<<checks<<" selected concrete visual stack checks passed; full SceneCreated remains false\n";
 }
 catch(const std::exception& e){allocation_budget=-1;std::cerr<<"FAILED: "<<e.what()<<" (check "<<checks<<")\n";return 1;}
}
