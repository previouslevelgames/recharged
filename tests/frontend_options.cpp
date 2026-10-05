#include "runtime/frontend_options.h"
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
template<class F>void Reject(F f,std::source_location at=std::source_location::current()){++checks;try{f();}catch(const std::exception&){return;}throw std::runtime_error("Invalid Options operation accepted at "+std::to_string(at.line()));}
void Near(float a,float b){Check(std::isfinite(a)&&std::abs(a-b)<.0001f,"Independent Options bounds differ");}
std::vector<std::uint8_t> Load(const char* path)
{std::unique_ptr<nlFile> file(nlOpen(path));Check(bool(file),"Missing Options input");const auto n=nlFileSize(file.get(),nullptr);std::vector<std::uint8_t> out(n);nlRead(file.get(),out.data(),n,n);return out;}
void Pump(FrontendSession& session)
{const auto end=std::chrono::steady_clock::now()+std::chrono::seconds(15);while(session.State()==FrontendSessionState::Loading){session.Service();Check(std::chrono::steady_clock::now()<end,"Options assets timed out");SDL_Delay(1);}session.Result();}
auto Session(const char* path="/Art/fe/options_main_menu.fen")
{auto owner=std::make_shared<FrontendSession>();owner->Begin({path,FrontendLanguage::English,FrontendImageProfile::Main,"in",true});Pump(*owner);return owner;}
using namespace audio_bank_fixture;
std::shared_ptr<FrontendAudio> Audio(bool owned,unsigned device=0)
{
 if(owned)
 {
  auto global=Load("/audio/nlxgs.bun");auto catalog=ReadAudioBankCatalog(global);AudioBankLoad owner(catalog,23,21);
  const auto end=std::chrono::steady_clock::now()+std::chrono::seconds(10);while(owner.State()==AudioBankLoadState::Loading){owner.Service();Check(std::chrono::steady_clock::now()<end,"Options sound bank timed out");SDL_Delay(1);}
  return std::make_shared<FrontendAudio>(owner.Result(),ReadAudioCalculationInitial(global),AudioVoicesOptions{32,64*1024*1024,device});
 }
 auto f=Make();const auto word=[](const Data& d,std::size_t at){return (std::uint32_t(d.at(at))<<24)|(std::uint32_t(d.at(at+1))<<16)|(std::uint32_t(d.at(at+2))<<8)|d.at(at+3);};
 const auto chunks=[&](const Data& data){std::vector<std::pair<unsigned,Data>> out;for(std::size_t at=0;at<data.size();){const auto id=word(data,at),n=word(data,at+4);out.emplace_back(id,Data(data.begin()+at+8,data.begin()+at+8+n));at+=(n+11)&~3u;}return out;};
 const std::array keys{0xf6eb899eu,0xf0afd586u,0x304fdd1eu,0x4430b152u};Data root;
 for(auto [id,data]:chunks(Data(f.bytes.begin()+8,f.bytes.end())))
 {
  if(id==0x80023000)
  {
   Data map,records;Append(map,0x23001,Words({4,0,0}));for(unsigned i=0;i<keys.size();++i){auto b=Words({keys[i],0,0,0,i});records.insert(records.end(),b.begin(),b.end());}Append(map,0x23003,records);data=std::move(map);
  }
  if(id==0x80023300)
  {
   Data graph;bool refs=false;
   for(auto [kind,part]:chunks(data))
   {
    if(kind==0x23301)Put(part,8,4);
    if(kind==0x23302){Data rows;for(auto key:keys){auto row=Data(part.begin()+40,part.end());Put(row,0,key);rows.insert(rows.end(),row.begin(),row.end());}part=std::move(rows);}
    if(kind==0x23308){if(!refs){for(unsigned i=0;i<4;++i)Append(graph,kind,Words({0xf0001000,0,Float(255),0,0}));refs=true;}continue;}
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
unsigned NextSeed(unsigned v){const auto a=v^0x1d872b41u,b=a^(a>>5);return b^a^(b<<27);}
struct CallbackProbe {FrontendOptions& owner;bool called=false;};
void Callback(nlFile*,void*,unsigned,nlFileAsyncParam value)
{
 auto& probe=*reinterpret_cast<CallbackProbe*>(value);auto frame=probe.owner.Current();probe.called=true;
 Reject([&]{probe.owner.AdvanceVisual(frame,0);});Reject([&]{probe.owner.NotifyBackButton(frame);});
 Reject([&]{probe.owner.DeliverPointer(frame,{0,{0,0}});});Reject([&]{probe.owner.Release();});
 Check(probe.owner.Current()==frame,"NL callback rejection changed Options frame");
}
void Input(FrontendInput& input,bool pressed=false){std::array<FrontendPadSample,4> pads{};for(auto& p:pads)p.connected=true;pads[0].buttons=pressed?0x100:0;input.Update(pads,1.f/60);}
FrontendPointerViewport Viewport(){return {1,960,720,1920,1440,0,0,1920,1440};}
void Ack(FrontendOptions& owner){owner.Acknowledge(owner.Current(),Viewport());}
std::array<float,2> Center(FrontendPointerBounds b){return {(b.min_x+b.max_x)/2,(b.min_y+b.max_y)/2};}
const FrontendInstance& Instance(const FrontendSession::Handle& frame,std::uint32_t id)
{auto it=std::find_if(frame->graph.instances.begin(),frame->graph.instances.end(),[&](const auto& v){return v.offset==id;});Check(it!=frame->graph.instances.end(),"Options instance absent");return *it;}
const FrontendLibraryObject& Library(const FrontendSession::Handle& frame,std::uint32_t id)
{auto it=std::find_if(frame->graph.library.begin(),frame->graph.library.end(),[&](const auto& v){return v.offset==id;});Check(it!=frame->graph.library.end(),"Options library absent");return *it;}
std::uint32_t Button(const FrontendSession::Handle& frame,unsigned item)
{const auto name="BTN_"+std::to_string(item);const std::array<std::string_view,4> path{"in","Layer","options_list",name};const auto node=FindFrontendNode(frame->graph,{},FrontendNamedPath(path),FrontendNodeType::Component);Check(bool(node),"Options button lookup absent");return node->id;}
std::string Feedback(const FrontendSession::Handle& frame,unsigned item)
{const auto& lib=Library(frame,*Instance(frame,Button(frame,item)).library);auto it=std::find_if(frame->graph.slides.begin(),frame->graph.slides.end(),[&](const auto& v){return v.offset==lib.active_slide;});Check(it!=frame->graph.slides.end(),"Options feedback absent");return it->name;}
unsigned EndUpdates(bool owned)
{
 // FEPresentation advances its own clock, assigns it to TLSlide, and the
 // original TLSlide then adds the same delta once. The next update starts from
 // the presentation clock, not the previously twice-advanced slide clock.
 const float duration=owned?.4f:.25f,delta=1.f/60;float presentation=0;
 for(unsigned n=1;n<100;++n){presentation=std::min(presentation+delta,duration);const float slide=std::min(presentation+delta,duration);if(slide>=duration)return n;}
 throw std::logic_error("Invalid independent clock oracle");
}
void Intro(FrontendOptions& owner,bool owned)
{
 const auto first=owner.Current();unsigned updates=0;while(owner.Status().state==0&&updates<100){owner.AdvanceVisual(owner.Current(),1.f/60);++updates;}
 Check(owner.Status().state==1&&owner.Status().initialized,"Options intro did not finish");Check(updates==EndUpdates(owned),"Original in duration/update order differs");Check(owner.Current()!=first,"Options base update did not publish");Ack(owner);
 const auto commands=owner.Status().commands;Check(commands.size()==5&&commands[0].kind==FrontendOptionsCommandKind::ShowNavigationBack&&commands[0].argument==4,"Options intro NAV boundary/order differs");
 for(unsigned i=0;i<4;++i)Check(commands[i+1].kind==(i==0?FrontendOptionsCommandKind::PointerCursor:FrontendOptionsCommandKind::PointerWaiting)&&commands[i+1].argument==i,"Options controller pointer request differs");
}
void BoundOracle(FrontendOptions& owner,bool owned,bool text)
{
 const auto frame=owner.Current();const auto bounds=owner.Bounds();
 for(unsigned i=0;i<3;++i)
 {
  const auto& button=Instance(frame,Button(frame,i));const std::array<std::string_view,3> path{"off","BUTTON_0","list_back_480x70 "};
  const auto hit=FindFrontendNode(frame->graph,{FrontendNodeKind::Instance,button.offset},FrontendNamedPath(path),FrontendNodeType::Any);Check(bool(hit),"Options hit lookup absent");const auto& instance=Instance(frame,hit->id);
  Check(instance.type==(text?3u:2u),"Options FindChecked did not preserve actual stored type");
  float w=0,h=0;
  if(text){w=22;h=12;} // independent synthetic A(10)+B(12), one original row.
  else{const auto& lib=Library(frame,*instance.library);const auto scale=instance.overload_flags&4?instance.attributes.scale:lib.attributes.scale;w=scale[0]*100;h=scale[1]*100;}
  const float x=instance.attributes.position[0]+button.attributes.position[0],y=instance.attributes.position[1]+button.attributes.position[1];
  Near(bounds[i].min_x,x-w/2);Near(bounds[i].max_x,x+w/2);Near(bounds[i].min_y,y-h/4);Near(bounds[i].max_y,y+h/4);
  if(owned){Check(hit->id==std::array{15600u,18800u,21660u}[i],"Owned actual image ID differs");std::cout<<"Owned Options hit"<<i<<" id="<<hit->id<<" bounds="<<bounds[i].min_x<<','<<bounds[i].max_x<<','<<bounds[i].min_y<<','<<bounds[i].max_y<<'\n';}
 }
}
void Lifecycle(bool owned,bool text=false)
{
 FrontendInput input;Input(input);auto audio=Audio(owned);auto session=Session(text?"/Art/fe/options-text.fen":"/Art/fe/options_main_menu.fen");unsigned seed=17;
 FrontendOptions owner(session,input,audio,seed);auto status=owner.Status();Check(status.state==0&&!status.initialized&&!status.transition,"Options initial state differs");
 Check(status.commands.size()==7&&status.commands[0].kind==FrontendOptionsCommandKind::HideNavigation&&status.commands[1].kind==FrontendOptionsCommandKind::BindNavigationBack&&status.commands[6].kind==FrontendOptionsCommandKind::SelectMusic,"Options SceneCreated dependencies differ");
 Reject([&]{owner.Bounds();});Ack(owner);owner.DeliverPointer(owner.Current(),{0,{0,0},true});Check(!owner.Status().transition,"Options intro accepted input");Intro(owner,owned);BoundOracle(owner,owned,text);const auto bounds=owner.Bounds();
 for(unsigned i=0;i<3;++i)
 {
  const auto old=owner.Current();const auto before=audio->ActiveCount(0xf6eb899e);const auto prior_seed=seed;owner.DeliverPointer(old,{0,Center(bounds[i])});
  Check(Feedback(owner.Current(),i)=="over"&&Feedback(old,i)=="off"&&owner.Status().pointer_states[i][0]==1,"Original Options Enter or retention differs");
  Check(audio->ActiveCount(0xf6eb899e)==before+1&&seed==(owned?prior_seed:NextSeed(prior_seed)),"Options Enter cue/RNG contract differs");audio->Update(0);audio->ServiceAudio();audio->Update(0);
  Reject([&]{owner.Acknowledge(old,Viewport());});Ack(owner);Near(owner.Bounds()[i].min_x,bounds[i].min_x);
  owner.DeliverPointer(owner.Current(),{0,{-999,-999}});Check(Feedback(owner.Current(),i)=="off"&&owner.Status().pointer_states[i][0]==0,"Original Options Leave differs");Ack(owner);
 }
 owner.DeliverPointer(owner.Current(),{0,Center(bounds[0])});Ack(owner);const auto before=audio->ActiveCount(0xf6eb899e);
 owner.DeliverPointer(owner.Current(),{1,Center(bounds[0])});Ack(owner);Check(owner.Status().pointer_states[0][1]==0&&audio->ActiveCount(0xf6eb899e)==before,"Source second-pointer quirk was changed");
 owner.DeliverPointer(owner.Current(),{0,{-999,-999}});Ack(owner);Check(Feedback(owner.Current(),0)=="off","Source first pointer Leave was rewritten");owner.DeliverPointer(owner.Current(),{1,{-999,-999}});Ack(owner);
 if(!owned&&!text)
 {
  auto retained=owner.Current();Reject([&]{session->Begin({"/Art/fe/absent-options.fen",FrontendLanguage::English,FrontendImageProfile::Main,"in",true});Pump(*session);});
  Check(session->Current()==retained,"Failed replacement lost current Options scene");session->Cancel();Ack(owner);
  std::unique_ptr<nlFile> file(nlOpen("/Art/fe/english.loc"));std::array<std::uint8_t,16> bytes{};CallbackProbe probe{owner};
  nlReadAsync(file.get(),bytes.data(),bytes.size(),Callback,reinterpret_cast<nlFileAsyncParam>(&probe),bytes.size());
  const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(3);
  while(nlAsyncReadsPending(file.get())){nlServiceFileSystem();Check(std::chrono::steady_clock::now()<deadline,"Options callback check timed out");SDL_Delay(1);}
  Check(probe.called&&owner.Current()==retained,"Real NL callback ownership differed");
  FrontendInstanceChange change;change.instance=Button(retained,0);change.flag=false;session->Apply(retained,std::span(&change,1));
  Reject([&]{owner.AdvanceVisual(retained,0);});Reject([&]{owner.Acknowledge(retained,Viewport());});
 }
 auto retained=owner.Current();owner.Release();owner.Release();Check(audio->Handles().empty()&&retained->images&&retained->visuals,"Options release lost resources or kept its sounds");Reject([&]{owner.Current();});audio->Unload();
}
void Selection(bool owned,unsigned item,bool back=false)
{
 FrontendInput input;Input(input);auto audio=Audio(owned);auto session=Session();unsigned seed=29;FrontendOptions owner(session,input,audio,seed);Intro(owner,owned);
 if(back)owner.NotifyBackButton(owner.Current());else owner.DeliverPointer(owner.Current(),{0,Center(owner.Bounds()[item]),true});
 Check(owner.Status().state==(back?3:2)&&!owner.Status().transition,"Options press skipped authored out");
 if(!back){Check(owner.Status().next_scene==std::array{15,14,23}[item],"Original item scene mapping differs");Check(audio->ActiveCount(0xf0afd586)==1&&audio->ActiveCount(0x304fdd1e)==unsigned(item<2),"Options Press cue order/count differs");}
 auto source=owner.Current();Ack(owner);owner.DeliverPointer(owner.Current(),{0,{0,0},true});Check(owner.Current()==source,"Options out accepted another pointer action");
 unsigned updates=0;while(!owner.Status().transition&&updates<100){owner.AdvanceVisual(owner.Current(),1.f/60);++updates;}
 Check(updates==EndUpdates(owned),"Options out clock differs");const auto terminal=owner.Status().transition;Check(bool(terminal),"Options terminal request absent");
 if(back){Check(terminal->kind==FrontendOptionsCommandKind::TransitionOptionsToMainMenu&&audio->ActiveCount(0x4430b152)==1,"Options return script/audio boundary differs");const auto& commands=owner.Status().commands;Check(commands.size()==2&&commands[0].kind==FrontendOptionsCommandKind::TransitionOptionsToMainMenu&&commands[1].kind==FrontendOptionsCommandKind::PopScene,"Return script then Pop source ordering differs");}
 else Check(terminal->kind==FrontendOptionsCommandKind::PushScene&&terminal->scene==std::array{15,14,23}[item],"Options terminal scene request differs");
 source=owner.Current();owner.AdvanceVisual(source,1);Check(owner.Current()==source,"Pending service request advanced/repeated source side effects");
 bool rejected=false;std::thread wrong([&]{try{owner.Status();}catch(const std::exception&){rejected=true;}});wrong.join();Check(rejected,"Foreign Options owner thread accepted");owner.Release();Check(audio->Handles().empty(),"Options terminal release retained audio");audio->Unload();
}
void FailureAndHost()
{
 FrontendInput input;Input(input);auto audio=Audio(false);auto session=Session();unsigned seed=47;
 const auto before=session->Current();unsigned failures=0;bool complete=false;
 for(long budget=0;budget<3000&&!complete;budget+=17)
 {
  allocation_budget=budget;std::unique_ptr<FrontendOptions> owner;bool failed=false;try{owner=std::make_unique<FrontendOptions>(session,input,audio,seed);}catch(const std::bad_alloc&){failed=true;}allocation_budget=-1;
  if(failed){++failures;Check(session->Current()==before&&audio->Handles().empty()&&seed==47,"Failed Options constructor published effects");}else {complete=true;owner->Release();}
 }
 Check(complete&&failures>=4,"Options allocation sweep missed constructor rollback");
 auto missing=Session("/Art/fe/options-missing.fen");const auto old=missing->Current();Reject([&]{FrontendOptions bad(missing,input,audio,seed);});Check(missing->Current()==old,"Missing Options path partially published");
 session=Session();FrontendOptions owner(session,input,audio,seed);Intro(owner,false);const auto center=Center(owner.Bounds()[0]);const auto v=Viewport();
 FrontendPointerDesktopSample sample{v.window,v.window_width,v.window_height,v.pixel_width,v.pixel_height,0,42,(center[0]/640+.5f)*v.window_width,(.5f-center[1]/480)*v.window_height,true,true};
 const auto route=[&]{++sample.sequence;auto out=owner.Route(sample);Ack(owner);return out;};
 Check(!route().active&&route().active&&Feedback(owner.Current(),0)=="over","Options host neutral/hover failed");sample.focused=false;sample.primary_down=true;Check(!route().active&&Feedback(owner.Current(),0)=="off","Options focus loss retained hover");sample.focused=true;Check(!route().active,"Options focus recovery accepted held input");sample.primary_down=false;Check(!route().active&&route().active,"Options neutral rearm differs");Input(input,true);Check(route().event.pressed&&owner.Status().state==2,"Original action30 did not select Options");owner.Release();audio->Unload();
 session=Session();audio=Audio(false,0x1234567);FrontendOptions bad(session,input,audio,seed);Intro(bad,false);const auto saved=bad.Current();const auto rng=seed;Reject([&]{bad.DeliverPointer(saved,{0,Center(bad.Bounds()[0])});});Check(bad.Status().failed&&session->Current()==saved&&seed==rng&&audio->Handles().empty(),"Failed SDL admission published Options feedback");bad.Release();audio->Unload();
}
struct Host {bool live=false,disc=false;~Host(){if(live)ResetStartupFiles();if(disc)aurora_dvd_close();if(live){ResetStartupMemory();aurora_shutdown();}}};
}
int main(int argc,char** argv)
{
 try
 {
  Check(argc==4,"Supply disc/output/generated or owned");const bool owned=std::string_view(argv[3])=="owned";
  const auto folder=(std::filesystem::path(argv[2])/"options-host").string();std::filesystem::create_directories(folder);AuroraConfig config{};
  config.appName="Charged original Options callbacks";config.userPath=config.cachePath=folder.c_str();config.resourcesPath=SDL_GetBasePath();config.desiredBackend=BACKEND_NULL;config.windowWidth=320;config.windowHeight=240;config.windowPosX=config.windowPosY=-1;config.mem1Size=MEM1_DEFAULT_SIZE;config.mem2Size=64*1024*1024;config.logLevel=LOG_WARNING;
  Host host;const auto h=aurora_initialize(argc,argv,&config);host.live=true;Check(h.window,"Aurora failed");InitializeStartupOS();nlInitMemory();Check(aurora_dvd_open(argv[1]),"Options disc missing");host.disc=true;nlInitFileSystem();
  for(unsigned i=0;i<3;++i){const auto a=StandardAllocator.TotalFreeMemory(),b=VirtualAllocator.TotalFreeMemory();Lifecycle(owned);for(unsigned item=0;item<3;++item)Selection(owned,item);Selection(owned,0,true);if(!owned&&i==0){Lifecycle(false,true);FailureAndHost();}Check(!nlAsyncReadsPending(nullptr)&&a==StandardAllocator.TotalFreeMemory()&&b==VirtualAllocator.TotalFreeMemory(),"Options teardown did not recover files/arenas");}
  std::cout<<checks<<" original Options checks passed; NAV/global services remain explicit requests\n";
 }
 catch(const std::exception& e){allocation_budget=-1;std::cerr<<"FAILED: "<<e.what()<<" (check "<<checks<<")\n";return 1;}
}
