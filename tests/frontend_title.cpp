#include "runtime/frontend_title.h"
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
 constexpr std::array keys{0x26894c84u,0xaa73ef32u,0x55c84a9du,0x80060b2du};Data root;
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

void Lifecycle(bool owned,bool wide,unsigned movement,unsigned controller=0)
{
 FrontendInput input;Pads(input);auto session=Session();auto audio=Audio(owned);auto music=Music(owned);unsigned seed=811;
 FrontendTitle title(session,input,audio,music,seed,{controller,movement,wide});Created(title.Status());
 Check(Active(title.Current())==(wide?"widescreen":"regular")&&Feedback(title.Current())=="off","Title source selected wrong authored presentation");
 Check(audio->ActiveCount(0x26894c84)==(movement==2?0u:1u),"Title SCREEN_BACK initial cue differs");PumpMusic(*music);Check(music->Status().cue==0xe326f931&&music->Status().submitted_frames>0&&music->Status().completed_reads>=3,"Title original music0 did not reach actual NL/SDL playback");
 const auto initial=title.Current();Reject([&]{title.Bounds();});Ack(title);title.DeliverPointer(initial,{controller,{0,0},true});Check(!title.Status().departure,"Uninitialized Title accepted pointer press");
 Pads(input,0x100,controller);title.AdvanceVisual(title.Current(),1.499f);Check(!title.Status().initialized&&title.Status().elapsed<1.5f&&!title.Status().departure,"Title bypassed original1.5s gate");Ack(title);
 Pads(input);title.AdvanceVisual(title.Current(),.001f);Check(title.Status().initialized,"Title did not initialize at1.5s");Ack(title);const auto bounds=title.Bounds();
 if(!owned){const float x=wide?-180.f:130.f,y=wide?50.f:-70.f;Near(bounds.min_x,x-20);Near(bounds.max_x,x+20);Near(bounds.min_y,y-20);Near(bounds.max_y,y+20);}
 const std::array<float,2> center{(bounds.min_x+bounds.max_x)/2,(bounds.min_y+bounds.max_y)/2};const auto before=title.Current();
 title.DeliverPointer(before,{controller,center});Check(title.Status().highlighted&&title.Status().pointer_states[controller]==1&&Feedback(title.Current())=="over"&&Feedback(before)=="off","Title enter mutated previous frame or missed actual feedback");
 Check(audio->ActiveCount(0xaa73ef32)>0,"Title hover did not play actual resident cue");Reject([&]{title.Acknowledge(before,Viewport());});Ack(title);Near(title.Bounds().max_x,bounds.max_x);
 const auto hover=title.Current();title.DeliverPointer(hover,{controller,{9999,9999}});Check(!title.Status().highlighted&&Feedback(title.Current())=="off"&&title.Status().pointer_states[controller]==0,"Title leave failed");Ack(title);
 const auto shown=title.Current();title.DeliverPointer(shown,{controller,center,true});Departed(title.Status(),shown);Check(Feedback(title.Current())=="down"&&title.Status().pointer_states[controller]==2,"Title press feedback differs");
 Check(audio->ActiveCount(0x55c84a9d)>0&&audio->ActiveCount(0x80060b2d)>0,"Title select/transition cues did not admit real resident handles");Ack(title);Near(title.Bounds().min_x,bounds.min_x);Near(title.Bounds().max_y,bounds.max_y);
 const auto selected=title.Current();const auto count=audio->Handles().size();title.AdvanceVisual(selected,1);title.DeliverPointer(selected,{controller,center,true});Check(title.Current()==selected&&audio->Handles().size()==count,"Pending external Title departure replayed source or audio");
 bool foreign=false;std::thread other([&]{try{title.Status();}catch(const std::logic_error&){foreign=true;}});other.join();Check(foreign,"Foreign Title thread accepted");
 title.Release();title.Release();Reject([&]{title.Current();});Check(audio->Handles().empty()&&selected->images&&selected->visuals&&Feedback(selected)=="down","Title release leaked cues or immutable presentation");music->Unload();audio->Unload();
}
void Sequence(bool owned)
{
 FrontendInput input;Pads(input);auto session=Session();auto audio=Audio(owned);auto music=Music(owned);unsigned seed=31;FrontendTitle title(session,input,audio,music,seed);PumpMusic(*music);
 title.AdvanceVisual(title.Current(),1.5f);Ack(title);const std::array<unsigned,6> directions{8,4,8,4,1,2};const std::array<unsigned,6> resets{0x800,0x400,0x40,0x20,0x20,0x40};
 for(unsigned reset:resets)
 {
  for(unsigned index=0;index<directions.size();++index)
  {
   Pads(input);Pads(input,directions[index]);title.AdvanceVisual(title.Current(),0);Ack(title);
   const auto state=title.Status();for(unsigned j=0;j<6;++j)Check(state.sequence[j]==(j<=index),"Title original6-key order/reset contract differs");
  }
  const auto completed=title.Status();Check(std::none_of(completed.sequence.begin()+6,completed.sequence.end(),[](bool v){return v;}),"Desktop Title fabricated Wii accelerometer unlock steps");
  Pads(input);Pads(input,reset);title.AdvanceVisual(title.Current(),0);Ack(title);const auto state=title.Status();Check(std::none_of(state.sequence.begin(),state.sequence.end(),[](bool v){return v;}),"Actual auxiliary action did not reset Title sequence");
 }
 title.Release();music->Unload();audio->Unload();
}
void Mapping()
{
 for(const auto [physical,action,mask]:std::array{
  std::tuple{SDL_GAMEPAD_BUTTON_NORTH,FrontendAction::TitleReset40,0x800u},std::tuple{SDL_GAMEPAD_BUTTON_WEST,FrontendAction::TitleReset41,0x400u},
  std::tuple{SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER,FrontendAction::TitleReset44,0x40u},std::tuple{SDL_GAMEPAD_BUTTON_LEFT_SHOULDER,FrontendAction::TitleReset45,0x20u},
  std::tuple{SDL_GAMEPAD_BUTTON_LEFT_SHOULDER,FrontendAction::TitleReset48,0x20u},std::tuple{SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER,FrontendAction::TitleReset49,0x40u}})
 {
  FrontendInput input;FrontendInputMap map;FrontendInputDevices devices;devices.pads[0].id=71;
  map.Update(input,devices,{},0);devices.pads[0].buttons[physical]=true;const auto sample=map.Update(input,devices,{},0);
  Check((sample[0].buttons&mask)&&input.Button(action,FrontendButtonQuery::Pressed,0),"SDL projection lost original Title auxiliary mask");
  auto capture=FrontendInputCapture{false,false,false};map.Update(input,devices,capture,0);Check(!input.Button(action,FrontendButtonQuery::Pressed,0),"Unfocused auxiliary key leaked original input");
  capture.focused=true;map.Update(input,devices,capture,0);Check(!input.Button(action,FrontendButtonQuery::Pressed,0),"Held auxiliary focus recovery bypassed neutral gate");
  devices.pads[0].buttons[physical]=false;map.Update(input,devices,capture,0);devices.pads[0].buttons[physical]=true;map.Update(input,devices,capture,0);Check(input.Button(action,FrontendButtonQuery::Pressed,0),"Neutral auxiliary key did not recover");
  devices.pads[0].id=72;map.Update(input,devices,capture,0);Check(!input.Button(action,FrontendButtonQuery::Pressed,0),"New held auxiliary device bypassed hotplug gate");
 }
}
void EarlyAccept(bool owned)
{
 FrontendInput input;Pads(input);auto session=Session();auto audio=Audio(owned);auto music=Music(owned);unsigned seed=4;FrontendTitle title(session,input,audio,music,seed);PumpMusic(*music);Ack(title);
 const auto shown=title.Current();Pads(input);Pads(input,0x100);title.AdvanceVisual(shown,1.5f);Check(title.Status().initialized&&title.Status().departure.has_value(),"Accept during exact initialization was lost");Departed(title.Status(),shown,true,true);Ack(title);const auto bound=title.Bounds();
 if(!owned){Near(bound.min_x,110);Near(bound.max_x,150);Near(bound.min_y,-90);Near(bound.max_y,-50);Check(Feedback(title.Current())=="down","Early source press not applied");}
 title.Release();music->Unload();audio->Unload();
}
FrontendStackRequest Request(){FrontendStackRequest r;r.scene=0;r.initial_slide="regular";r.resources_mode=FrontendSessionResourcesMode::PermanentMain;return r;}
void Pump(FrontendSceneStack& stack,std::uint64_t id)
{const auto end=std::chrono::steady_clock::now()+std::chrono::seconds(20);while(stack.Entry(id).state==FrontendStackState::Queued||stack.Entry(id).state==FrontendStackState::Loading){stack.Service();Check(std::chrono::steady_clock::now()<end,"Title stack read timed out");SDL_Delay(1);}stack.RethrowFailure(id);}
void Stack(bool owned,bool idle)
{
 FrontendInput input;Pads(input);auto audio=Audio(owned);auto music=Music(owned);unsigned seed=910,drains=0;bool fail_drain=false;
 FrontendSceneStack stack(input,[&]{++drains;if(fail_drain){fail_drain=false;throw std::runtime_error("Actual renderer drain failed");}});std::shared_ptr<FrontendTitle> title;std::shared_ptr<FrontendSession> session;std::shared_ptr<FrontendHandler> handler;
 const auto token=stack.QueuePush(Request());stack.BindVisual(token,[&](auto c){session=c.session;handler=c.handler;title=std::make_shared<FrontendTitle>(c.session,input,audio,music,seed,c.handler,FrontendTitleOptions{0,c.movement,false});return title;});Pump(stack,token);PumpMusic(*music);
 const auto entry=stack.Entry(token);Check(entry.state==FrontendStackState::AwaitingPublication&&entry.handler_scope==FrontendStackHandlerScope::SelectedVisual&&entry.subhandlers==FrontendStackSubhandlers::SourceEmpty&&!entry.full_scene_created&&!stack.AllReady(),"Title stack invented full SceneCreated/state6");
 const auto first=title->Current();unsigned calls=0;stack.Update(1,[&](auto,const auto&){++calls;});Check(title->Current()==first&&!calls,"Unpresented Title advanced original base");stack.Publish(token,first);Ack(*title);
 Reject([&]{title->AdvanceVisual(first,0);});Reject([&]{title->Release();});Reject([&]{handler->UpdateOnce(first,0);});
 FrontendHandler lock(session,input);lock.SetExclusiveInput(true);Pads(input,0x100);
 stack.Update(.5f,[&](auto t,const auto& shown){++calls;Check(t==token&&shown==first,"Title stack lost input source");Reject([&]{stack.Update(0);});});stack.RethrowFailure(token);Near(title->Status().elapsed,.5f);Check(!title->Status().departure&&title->Current()!=first&&calls==1,"Input lock incorrectly froze original Title clock or delivered Accept");
 stack.Publish(token,title->Current());Ack(*title);Pads(input);Pads(input,0x100);stack.Update(1);stack.RethrowFailure(token);Check(title->Status().initialized&&!title->Status().departure,"Title input lock froze its clock or admitted initialized Accept");Near(title->Status().elapsed,1.5f);
 lock.SetExclusiveInput(false);lock.Release();stack.Publish(token,title->Current());Ack(*title);Pads(input);stack.Update(0);stack.RethrowFailure(token);stack.Publish(token,title->Current());Ack(*title);
 if(idle)
 {
  for(float dt:{50.f,50.f,58.5f}){Pads(input);stack.Update(dt);stack.RethrowFailure(token);stack.Publish(token,title->Current());Ack(*title);}
  Near(title->Status().elapsed,160);Check(!title->Status().started_demo&&!title->Status().departure,"Title Intro fired at160 rather than strictlygreater");
  stack.Update(.0001f);stack.RethrowFailure(token);const auto s=title->Status();Check(s.started_demo&&s.elapsed==0&&s.departure==FrontendTitleCommandKind::IntroMovie&&s.commands.size()==2&&s.commands[0].kind==FrontendTitleCommandKind::PointerEnabled&&!s.commands[0].argument&&s.commands[1].kind==FrontendTitleCommandKind::IntroMovie&&s.commands[1].argument==22,"Title exact idle stop/intro22 order differs");
  Check(music->Status().queued_input_bytes==0&&music->Status().source_state==6,"Title idle did not stop actual music output");
 }
 else
 {
  const auto shown=title->Current();Pads(input);Pads(input,0x100);stack.Update(0);stack.RethrowFailure(token);Departed(title->Status(),shown,true);
 }
 stack.Publish(token,title->Current());Ack(*title);const auto stopped=title->Current();calls=0;stack.Update(1,[&](auto,const auto&){++calls;});stack.RethrowFailure(token);Check(!calls&&title->Current()==stopped,"Pending external Title boundary consumed another base/input proof");
 std::weak_ptr<FrontendSession> life=session;session.reset();fail_drain=true;Reject([&]{stack.Release();});Check(!life.expired()&&title->Current()==stopped,"Failed graphics drain discarded Title owners");stack.Release();Check(life.expired()&&audio->Handles().empty()&&stopped->visuals&&stopped->images&&drains>=5,"Title stack release lost immutable resources or cue cleanup");Reject([&]{title->Current();});music->Unload();audio->Unload();
}
void Failures(bool owned)
{
 FrontendInput input;Pads(input);auto audio=Audio(owned);auto music=Music(owned);unsigned seed=12;
 if(!owned)
 {
  auto missing=Session("/Art/fe/title-missing.fen");const auto before=missing->Current();Reject([&]{FrontendTitle invalid(missing,input,audio,music,seed);});Check(missing->Current()==before&&audio->Handles().empty()&&music->Status().load==FrontendMusicLoadState::Idle,"Failed Title lookup published partial frame or music/cue services");
 }
 auto session=Session();const auto initial=session->Current();for(auto o:{FrontendTitleOptions{4,0,false},FrontendTitleOptions{0,3,false}})Reject([&]{FrontendTitle invalid(session,input,audio,music,seed,o);});Check(session->Current()==initial,"Invalid Title profile mutated immutable frame");
 unsigned failures=0;for(long budget:{0L,8L,40L,150L,500L})
 {
  auto candidate=Session();const auto before=candidate->Current();bool failed=false;
  try{allocation_budget=budget;FrontendTitle owner(candidate,input,audio,music,seed);allocation_budget=-1;owner.Release();}
  catch(const std::bad_alloc&){allocation_budget=-1;failed=true;++failures;}
  allocation_budget=-1;if(failed)Check(candidate->Current()==before,"Failed Title clone published partial state");music->CancelPending();Check(audio->Handles().empty()&&!nlAsyncReadsPending(nullptr),"Allocation-failed Title constructor leaked cue/read services");
 }
 Check(failures>=3,"Title allocation sweep missed actual constructor ownership");
 FrontendSceneStack stack(input,[]{});std::shared_ptr<FrontendTitle> title;
 auto id=stack.QueuePush(Request());stack.BindVisual(id,[&](auto c){title=std::make_shared<FrontendTitle>(c.session,input,audio,music,seed,c.handler,FrontendTitleOptions{});return title;});Pump(stack,id);PumpMusic(*music);stack.Publish(id,title->Current());Ack(*title);const auto shown=title->Current();
 stack.Update(.125f,[&](auto,const auto&){throw std::runtime_error("Actual input callback failure");});Reject([&]{stack.RethrowFailure(id);});Check(title->Status().failed&&stack.Entry(id).state==FrontendStackState::Failed&&stack.Entry(id).published==shown,"Failed Title source callback published a candidate");stack.QueuePop(id);stack.Poll();stack.Release();music->Unload();audio->Unload();
}
void Missing()
{
 FrontendInput input;Pads(input);FrontendSceneStack stack(input,[]{});bool made=false;const auto id=stack.QueuePush(Request());stack.BindVisual(id,[&](auto)->std::shared_ptr<FrontendStackVisual>{made=true;throw std::logic_error("Missing Title unexpectedly constructed");});Reject([&]{Pump(stack,id);});const auto e=stack.Entry(id);Check(!made&&!e.prepared&&!e.published&&!e.full_scene_created&&e.state==FrontendStackState::Failed&&!stack.AllReady(),"Missing Title fabricated handler readiness");stack.QueuePop(id);stack.Poll();stack.Release();
}
struct Host{bool live=false,disc=false;~Host(){if(live)ResetStartupFiles();if(disc)aurora_dvd_close();if(live){ResetStartupMemory();aurora_shutdown();}}};
}
int main(int argc,char** argv)
{
 try
 {
  if(argc==3&&std::string_view(argv[1])=="--fixture")
  {std::filesystem::create_directories(argv[2]);auto f=frontend_music_fixture::Make();Save(std::filesystem::path(argv[2])/"music.resbun",f.metadata);Save(std::filesystem::path(argv[2])/"music.nlxwb",f.wave);Save(std::filesystem::path(argv[2])/"calculation.bun",frontend_music_fixture::Calculation());return 0;}
  Check(argc==4,"Supply Title disc/output/generated|owned|missing");const auto mode=std::string_view(argv[3]);const bool owned=mode=="owned",missing=mode=="missing";Check(owned||missing||mode=="generated","Unknown Title test mode");
  const auto folder=(std::filesystem::absolute(argv[2])/"title-host").string();std::filesystem::create_directories(folder);AuroraConfig config{};config.appName="Charged selected original Title owner";config.userPath=config.cachePath=folder.c_str();config.resourcesPath=SDL_GetBasePath();config.desiredBackend=BACKEND_NULL;config.windowWidth=320;config.windowHeight=240;config.windowPosX=config.windowPosY=-1;config.mem1Size=MEM1_DEFAULT_SIZE;config.mem2Size=64*1024*1024;config.logLevel=LOG_WARNING;
  Host host;const auto state=aurora_initialize(argc,argv,&config);host.live=true;Check(state.window,"Aurora core initialization failed");InitializeStartupOS();nlInitMemory();Check(aurora_dvd_open(argv[1]),"Title disc unavailable");host.disc=true;nlInitFileSystem();
  const auto a=StandardAllocator.TotalFreeMemory(),b=VirtualAllocator.TotalFreeMemory();if(missing)Missing();else{Mapping();Lifecycle(owned,false,0);Lifecycle(owned,true,2,2);Sequence(owned);EarlyAccept(owned);Stack(owned,false);Stack(owned,true);Failures(owned);}Check(!nlAsyncReadsPending(nullptr)&&a==StandardAllocator.TotalFreeMemory()&&b==VirtualAllocator.TotalFreeMemory(),"Title native arenas or NL workers did not recover");
  std::cout<<checks<<" selected Title source/stack/input checks passed; full SceneCreated/intro/Wii services pending\n";return 0;
 }
 catch(const std::exception& e){allocation_budget=-1;std::cerr<<"FAILED: "<<e.what()<<" (check "<<checks<<")\n";return 1;}
}
