#include "runtime/frontend_navigation.h"
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
void Near(float a,float b){Check(std::isfinite(a)&&std::abs(a-b)<.0001f,"Independent NAV bounds differ");}
std::vector<std::uint8_t> Load(const char* path)
{std::unique_ptr<nlFile> file(nlOpen(path));Check(bool(file),"Missing NAV input");const auto n=nlFileSize(file.get(),nullptr);std::vector<std::uint8_t> out(n);nlRead(file.get(),out.data(),n,n);return out;}
void Pump(FrontendSession& session)
{const auto end=std::chrono::steady_clock::now()+std::chrono::seconds(15);while(session.State()==FrontendSessionState::Loading){session.Service();Check(std::chrono::steady_clock::now()<end,"NAV assets timed out");SDL_Delay(1);}session.Result();}
auto Session(const char* path="/Art/fe/fe_overlay.fen")
{auto owner=std::make_shared<FrontendSession>();owner->Begin({path,FrontendLanguage::English,FrontendImageProfile::Main,"Slide1",true});Pump(*owner);return owner;}
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
 const std::array keys{0xaccdca48u,0x6f6a3a07u,0xaccdca48u+1,0x6f6a3a07u+1};Data root;
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
void Input(FrontendInput& input,bool pressed=false){std::array<FrontendPadSample,4> pads{};for(auto& p:pads)p.connected=true;pads[0].buttons=pressed?0x100:0;input.Update(pads,1.f/60);}
FrontendPointerViewport Viewport(bool wide=false){auto out=FrontendPointerViewport{1,960,720,1920,1440,0,0,1920,1440};out.widescreen=wide;return out;}
void Ack(FrontendNavigation& owner){owner.Acknowledge(owner.Current(),Viewport(owner.Status().widescreen));}
const FrontendInstance& Instance(const FrontendSession::Handle& f,std::uint32_t id)
{auto it=std::find_if(f->graph.instances.begin(),f->graph.instances.end(),[&](const auto& v){return v.offset==id;});Check(it!=f->graph.instances.end(),"NAV instance absent");return *it;}
const FrontendLibraryObject& Library(const FrontendSession::Handle& f,std::uint32_t id)
{auto it=std::find_if(f->graph.library.begin(),f->graph.library.end(),[&](const auto& v){return v.offset==id;});Check(it!=f->graph.library.end(),"NAV library absent");return *it;}
std::uint32_t Find(const FrontendSession::Handle& f,std::initializer_list<std::string_view> path)
{auto node=FindFrontendNode(f->graph,{},FrontendNamedPath(std::span(path.begin(),path.size())));Check(node&&node->kind==FrontendNodeKind::Instance,"NAV source path absent");return node->id;}
std::string Active(const FrontendSession::Handle& f,std::uint32_t id)
{const auto& lib=Library(f,*Instance(f,id).library);const auto it=std::find_if(f->graph.slides.begin(),f->graph.slides.end(),[&](const auto& v){return v.offset==lib.active_slide;});Check(it!=f->graph.slides.end(),"NAV active slide absent");return it->name;}
std::string BackState(const FrontendNavigation& n)
{auto f=n.Current();return Active(f,Find(f,{"Slide1","Layer","back",n.Status().widescreen?"16:9":"4:3","back"}));}
struct CallbackProbe {FrontendNavigation& owner;bool called=false;};
void Callback(nlFile*,void*,unsigned,nlFileAsyncParam value)
{
 auto& p=*reinterpret_cast<CallbackProbe*>(value);auto f=p.owner.Current();p.called=true;
 Reject([&]{p.owner.AdvanceVisual(f,0);});Reject([&]{p.owner.SetButtons(f,4);});Reject([&]{p.owner.DeliverPointer(f,{0,{0,0}});});Reject([&]{p.owner.Release();});Check(p.owner.Current()==f,"Callback rejection changed NAV");
}
void Lifecycle(bool owned,bool wide)
{
 FrontendInput input;Input(input);auto session=Session();auto audio=Audio(owned);unsigned seed=17;
 const auto old=session->Current();FrontendNavigation nav(session,input,audio,seed,wide);Check(nav.Current()!=old&&nav.Status().visible_buttons==0,"NAV visual setup did not publish");
 const auto back=Find(nav.Current(),{"Slide1","Layer","back"});Check(!Instance(nav.Current(),back).visible,"NAV initial button mask differs");
 Check(Active(nav.Current(),back)==(wide?"16:9":"4:3"),"Back aspect selection differs");
 for(unsigned i=0;i<4;++i){const auto name="cursor"+std::to_string(i);Check(Active(nav.Current(),Find(nav.Current(),{"Slide1","Layer",name}))=="waiting","NAV cursor setup differs");}
 Reject([&]{nav.Bounds();});nav.SetButtons(nav.Current(),4,true);Check(nav.Status().visible_buttons==4&&Instance(nav.Current(),back).visible,"NAV SetButtons did not show back");Ack(nav);
 auto f=nav.Current();const auto bounds=nav.Bounds();const auto nested=Find(f,{"Slide1","Layer","back",wide?"16:9":"4:3","back"});
 const std::array<std::string_view,2> hitpath{"over","list_high_250x60"};const auto hit=FindFrontendNode(f->graph,{FrontendNodeKind::Instance,nested},FrontendNamedPath(hitpath));Check(bool(hit),"NAV actual hit missing");
 const auto& image=Instance(f,hit->id);const auto& parent=Instance(f,nested);const auto scale=image.overload_flags&4?image.attributes.scale:Library(f,*image.library).attributes.scale;
 const auto x=image.attributes.position[0]+parent.attributes.position[0],y=image.attributes.position[1]+parent.attributes.position[1];
 Near(bounds.min_x,x-scale[0]*50);Near(bounds.max_x,x+scale[0]*50);Near(bounds.min_y,y-scale[1]*50);Near(bounds.max_y,y+scale[1]*50);
 if(owned){Check(hit->id==5420,"Retail NAV actual hit ID differs");std::cout<<"Owned NAV aspect="<<wide<<" hit="<<hit->id<<" bounds="<<bounds.min_x<<','<<bounds.max_x<<','<<bounds.min_y<<','<<bounds.max_y<<'\n';}
 const std::array<float,2> center{(bounds.min_x+bounds.max_x)/2,(bounds.min_y+bounds.max_y)/2};
 const auto rng=seed;Check(!nav.DeliverPointer(f,{0,center}),"Hover incorrectly completed back");Check(BackState(nav)=="over"&&nav.Status().back_states[0]==1&&nav.Status().back_inside[0],"Original back enter/inside differs");Check(audio->ActiveCount(0xaccdca48)==1,"NAV hover cue absent");
 if(!owned)Check(seed==NextSeed(rng),"Independent NAV hover RNG differs");
 Ack(nav);Check(!nav.DeliverPointer(nav.Current(),{1,center}),"Second hover completed back");Check(nav.Status().back_states[1]==1&&audio->ActiveCount(0xaccdca48)==1,"Back second pointer differs from source");
 Ack(nav);nav.DeliverPointer(nav.Current(),{0,{-999,-999}});Check(BackState(nav)=="over"&&nav.Status().back_states[0]==0,"Back first leave erased other hover");
 Ack(nav);nav.DeliverPointer(nav.Current(),{1,{-999,-999}});Check(BackState(nav)=="off"&&nav.Status().back_states[1]==0,"Back final leave differs");
 Ack(nav);Check(nav.DeliverPointer(nav.Current(),{2,center,true}),"Genuine false-push/false-pop back press did not complete");Check(audio->ActiveCount(0x6f6a3a07)==1,"NAV press cue absent");
 Ack(nav);Check(!nav.DeliverPointer(nav.Current(),{2,center}),"Back repeated consumed press");Ack(nav);Check(!nav.DeliverPointer(nav.Current(),{2,center,false,true}),"Back release completed press");
 audio->Update(0);audio->ServiceAudio();audio->Update(0);
 f=nav.Current();nav.HideButtons(f);Check(nav.Status().visible_buttons==4&&!Instance(nav.Current(),back).visible&&Instance(f,back).visible,"HideButtons reset mask or mutated retained frame");
 nav.SetButtons(nav.Current(),4);Ack(nav);Near(nav.Bounds().min_x,bounds.min_x);
 std::array<FrontendNavigationPointerSample,4> pointers{};pointers[0]={{12,-20},16384,true};pointers[1]={{-1,2},65535,true};nav.UpdatePointers(nav.Current(),pointers);
 for(unsigned i=0;i<4;++i){const auto name="cursor"+std::to_string(i);const auto& v=Instance(nav.Current(),Find(nav.Current(),{"Slide1","Layer",name}));Check(v.visible==pointers[i].valid,"NAV pointer validity differs");if(pointers[i].valid){Near(v.attributes.position[0],pointers[i].position[0]);Near(v.attributes.position[1],pointers[i].position[1]);Near(v.attributes.rotation[2],float(std::uint16_t(-pointers[i].angle))*.0000958738f);}}
 nav.UpdatePointers(nav.Current(),pointers,true);Check(!Instance(nav.Current(),Find(nav.Current(),{"Slide1","Layer","cursor0"})).visible,"Hidden pointer remains visible");
 nav.SetPointerSlide(nav.Current(),0,FrontendNavigationPointer::Cursor);Check(Active(nav.Current(),Find(nav.Current(),{"Slide1","Layer","cursor0"}))=="cursor","NAV cursor request differs");
 f=nav.Current();nav.AdvanceVisual(f,1.f/60);Check(nav.Current()!=f,"NAV base update absent");Reject([&]{nav.Acknowledge(f,Viewport(wide));});Ack(nav);Near(nav.Bounds().min_x,bounds.min_x);
 // Input reads an actually acknowledged frame after the next base/command
 // mutations. No candidate is acknowledged just to make its hit regions usable.
 f=nav.Current();nav.AdvanceVisual(f,.125f);nav.HideButtons(nav.Current());const auto candidate=nav.Current();
 Reject([&]{nav.DeliverPointer(f,{0,center});});
 Reject([&]{nav.WithPresentedInput(candidate,[]{});});
 nav.WithPresentedInput(f,[&]{
  Reject([&]{nav.WithPresentedInput(f,[]{});});
  Reject([&]{nav.AdvanceVisual(nav.Current(),0);});
  Reject([&]{nav.Acknowledge(nav.Current(),Viewport(wide));});
  Reject([&]{nav.Release();});
  Check(nav.DeliverPointer(f,{3,center,true}),"Presented NAV input did not complete source back");
 });
 Check(nav.Current()!=candidate&&!nav.Status().failed&&Instance(f,back).visible&&!Instance(nav.Current(),back).visible,
  "Presented NAV input changed old geometry or lost candidate commands");
 Reject([&]{nav.DeliverPointer(f,{3,center});});Ack(nav);nav.SetButtons(nav.Current(),4);Ack(nav);
 if(!owned&&!wide)
 {
  f=nav.Current();std::unique_ptr<nlFile> file(nlOpen("/Art/fe/english.loc"));std::array<std::uint8_t,16> bytes{};CallbackProbe probe{nav};nlReadAsync(file.get(),bytes.data(),bytes.size(),Callback,reinterpret_cast<nlFileAsyncParam>(&probe),bytes.size());const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(3);
  while(nlAsyncReadsPending(file.get())){nlServiceFileSystem();Check(std::chrono::steady_clock::now()<deadline,"NAV callback timed out");SDL_Delay(1);}Check(probe.called&&nav.Current()==f,"NAV callback altered state");
  Reject([&]{session->Begin({"/Art/fe/absent-nav.fen",FrontendLanguage::English,FrontendImageProfile::Main,"Slide1",true});Pump(*session);});session->Cancel();Check(session->Current()==f,"Failed replacement lost NAV frame");
  FrontendInstanceChange change;change.instance=back;session->Apply(f,std::span(&change,1));Reject([&]{nav.Acknowledge(f,Viewport());});Reject([&]{nav.AdvanceVisual(f,0);});
 }
 bool rejected=false;std::thread foreign([&]{try{nav.Status();}catch(...){rejected=true;}});foreign.join();Check(rejected,"NAV accepted foreign owner thread");f=nav.Current();nav.Release();nav.Release();Check(audio->Handles().empty()&&f->images&&f->visuals,"NAV release lost retained data or active audio");Reject([&]{nav.Current();});audio->Unload();
}
void Failures(SDL_Window* window)
{
 FrontendInput input;Input(input);auto session=Session();auto audio=Audio(false);unsigned seed=91;const auto original=session->Current();bool complete=false;unsigned failures=0;
 for(long budget=0;budget<100000&&!complete;budget+=257)
 {
  allocation_budget=budget;std::unique_ptr<FrontendNavigation> nav;bool failed=false;try{nav=std::make_unique<FrontendNavigation>(session,input,audio,seed);}catch(const std::bad_alloc&){failed=true;}allocation_budget=-1;
  if(failed){++failures;Check(session->Current()==original&&audio->Handles().empty()&&seed==91,"Failed NAV constructor partially published");}else{complete=true;nav->Release();}
 }
 Check(complete&&failures>=3,"NAV OOM sweep missed rollback");auto missing=Session("/Art/fe/nav-missing.fen");const auto prior=missing->Current();Reject([&]{FrontendNavigation bad(missing,input,audio,seed);});Check(prior==missing->Current(),"Missing cursor setup partly published");audio->Unload();
 session=Session();audio=Audio(false);{
  FrontendNavigation nav(session,input,audio,seed);nav.SetButtons(nav.Current(),4);auto viewport=Viewport();viewport.window=SDL_GetWindowID(window);nav.Acknowledge(nav.Current(),viewport);const auto b=nav.Bounds();
  nav.DeliverPointer(nav.Current(),{0,{(b.min_x+b.max_x)/2,(b.min_y+b.max_y)/2}});nav.Acknowledge(nav.Current(),viewport);
  const auto result=nav.Poll(window);Check(!result.pointer.active&&!result.back_pressed&&result.pointer.event.index==0&&result.pointer.event.position==std::array{-999.f,-999.f}&&!nav.Status().failed&&BackState(nav)=="off","NAV resize/minimized recovery poisoned its owner or retained hover");
  int width,height,pw,ph;Check(SDL_GetWindowSize(window,&width,&height)&&SDL_GetWindowSizeInPixels(window,&pw,&ph),"NAV real SDL extents absent");
  viewport.window_width=width;viewport.window_height=height;viewport.pixel_width=pw;viewport.pixel_height=ph;viewport.width=pw;viewport.height=ph;
  nav.Acknowledge(nav.Current(),viewport);FrontendPointerDesktopSample sample{viewport.window,unsigned(width),unsigned(height),unsigned(pw),unsigned(ph),1,42,(b.min_x+b.max_x)/1280*width+width/2.f,height/2.f-(b.min_y+b.max_y)/960*height,true,true};
  Check(!nav.Route(sample).pointer.active,"NAV viewport replacement skipped neutral gate");nav.Acknowledge(nav.Current(),viewport);++sample.sequence;Check(nav.Route(sample).pointer.active&&BackState(nav)=="over","NAV viewport replacement did not rearm");nav.Release();
 }audio->Unload();
 session=Session();audio=Audio(false,0x1234567);FrontendNavigation nav(session,input,audio,seed);nav.SetButtons(nav.Current(),4);Ack(nav);const auto frame=nav.Current();const auto b=nav.Bounds();const auto before_device_seed=seed;
 Reject([&]{nav.DeliverPointer(frame,{0,{(b.min_x+b.max_x)/2,(b.min_y+b.max_y)/2}});});Check(nav.Status().failed&&session->Current()==frame&&audio->Handles().empty()&&seed==before_device_seed,"Failed NAV SDL admission published effects");nav.Release();audio->Unload();
 session=Session();audio=Audio(false);FrontendNavigation failed(session,input,audio,seed);Ack(failed);const auto shown=failed.Current();
 failed.AdvanceVisual(shown,.125f);
 Reject([&]{failed.WithPresentedInput(shown,[]{throw std::runtime_error("Actual input callback failure");});});
 Check(failed.Status().failed,"Throwing NAV input window remained available");failed.Release();audio->Unload();
}
struct Host{bool live=false,disc=false;~Host(){if(live)ResetStartupFiles();if(disc)aurora_dvd_close();if(live){ResetStartupMemory();aurora_shutdown();}}};
}
int main(int argc,char** argv)
{
 try
 {
  Check(argc==4,"Supply disc/output/generated or owned");const bool owned=std::string_view(argv[3])=="owned";const auto folder=(std::filesystem::path(argv[2])/"navigation-host").string();std::filesystem::create_directories(folder);AuroraConfig config{};
  config.appName="Charged original NAV callbacks";config.userPath=config.cachePath=folder.c_str();config.resourcesPath=SDL_GetBasePath();config.desiredBackend=BACKEND_NULL;config.windowWidth=320;config.windowHeight=240;config.windowPosX=config.windowPosY=-1;config.mem1Size=MEM1_DEFAULT_SIZE;config.mem2Size=64*1024*1024;config.logLevel=LOG_WARNING;
  Host host;const auto h=aurora_initialize(argc,argv,&config);host.live=true;Check(h.window,"Aurora failed");InitializeStartupOS();nlInitMemory();Check(aurora_dvd_open(argv[1]),"NAV disc missing");host.disc=true;nlInitFileSystem();
  for(unsigned i=0;i<3;++i){const auto a=StandardAllocator.TotalFreeMemory(),b=VirtualAllocator.TotalFreeMemory();Lifecycle(owned,false);Lifecycle(owned,true);if(!owned&&i==0)Failures(h.window);Check(!nlAsyncReadsPending(nullptr)&&a==StandardAllocator.TotalFreeMemory()&&b==VirtualAllocator.TotalFreeMemory(),"NAV teardown did not restore arenas/files");}
  std::cout<<checks<<" original NAV/back checks passed; global DPD/HOME/rumble remain unavailable\n";
 }
 catch(const std::exception& e){allocation_budget=-1;std::cerr<<"FAILED: "<<e.what()<<" (check "<<checks<<")\n";return 1;}
}
