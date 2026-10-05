#include "runtime/frontend_boot_loading.h"
#include "runtime/frontend_boot_audio.h"
#include "audio_bank_fixture.h"
#include "runtime/startup.h"
#include "runtime/startup_files.h"
#include "NL/MemAlloc.h"
#include "NL/nlMemory.h"
#include "NL/nlFile.h"
#include "NL/nlFileGC.h"
#include <aurora/aurora.h>
#include <aurora/dvd.h>
#include <SDL3/SDL.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <source_location>
#include <thread>
thread_local long allocation_budget=-1;
void* operator new(std::size_t n){if(allocation_budget==0)throw std::bad_alloc();if(allocation_budget>0)--allocation_budget;if(auto p=std::malloc(n?n:1))return p;throw std::bad_alloc();}
void* operator new[](std::size_t n){return ::operator new(n);}
void operator delete(void* p)noexcept{std::free(p);}void operator delete[](void* p)noexcept{std::free(p);}
void operator delete(void* p,std::size_t)noexcept{std::free(p);}void operator delete[](void* p,std::size_t)noexcept{std::free(p);}
using namespace mscharged;
using namespace mscharged::resources;
namespace
{
unsigned checks=0;
void Check(bool value,const char* message){++checks;if(!value)throw std::runtime_error(message);}
template<class F>void Reject(F f,std::source_location at=std::source_location::current())
{++checks;try{f();}catch(const std::exception&){return;}throw std::runtime_error("Expected rejection at line "+std::to_string(at.line()));}
void Near(float a,float b){Check(std::abs(a-b)<1e-6f,"Independent boot clock differs");}
FrontendSessionRequest Request(const char* path="/Art/fe/boot_loading.fen",FrontendLanguage language=FrontendLanguage::English)
{FrontendSessionRequest r;r.path=path;r.image_profile=FrontendImageProfile::BootLoading;r.language=language;return r;}
void Pump(FrontendSession& session)
{
 const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(10);
 while(session.State()==FrontendSessionState::Loading){session.Service();Check(std::chrono::steady_clock::now()<deadline,"Boot scene loading timed out");SDL_Delay(1);}session.Result();
}
const FrontendInstance& Find(const FrontendSession::Handle& frame,const char* slide,const char* name)
{
 const std::array<std::string_view,3> names{slide,"Layer",name};const auto node=FindFrontendNode(frame->graph,{},FrontendNamedPath(names));Check(node.has_value(),"Authored test instance is missing");
 const auto it=std::find_if(frame->graph.instances.begin(),frame->graph.instances.end(),[&](const auto& p){return p.offset==node->id;});Check(it!=frame->graph.instances.end(),"Instance ID is absent");return *it;
}
std::string Active(const FrontendSession::Handle& f)
{for(const auto& slide:f->graph.slides)if(f->graph.active_slide==slide.offset)return slide.name;throw std::runtime_error("Missing active boot slide");}
void Neutral(FrontendInput& input,std::array<FrontendPadSample,4>& pads)
{for(auto& p:pads){p.connected=true;p.buttons=0;}input.Update(pads,0);}
void AudioGenerated()
{
 using namespace audio_bank_fixture;
 SDL_SetHint(SDL_HINT_AUDIO_DRIVER,"dummy");
 auto fixture=Make();Put(fixture.bytes,fixture.map+20,0xde83984e);Put(fixture.bytes,fixture.cues+40,0xde83984e);
 const auto loaded=std::make_shared<const LoadedAudioBank>(LoadedAudioBank{25,23,{25,"FE_GEN_Splash"},{23,0,1,false},ReadAudioResidentBank(fixture.bytes,fixture.wave)});
 Data section;Append(section,0x23401,Words({2,0xf1000100,0}));Append(section,0x23402,Words({0,0,0,0,0,0,1,0,0,0xf1000100,0,0}));
 const auto calculation=ReadAudioCalculationInitial(Wrap(0x80000001,Wrap(0x80023400,section)));
 FrontendInput input;std::array<FrontendPadSample,4> pads{};Neutral(input,pads);
 for(bool invalid_device:{true,false})
 {
  auto session=std::make_shared<FrontendSession>();session->Begin(Request());Pump(*session);
  unsigned seed=0xabcdef12;auto audio=std::make_shared<FrontendBootAudio>(loaded,calculation,seed,invalid_device?0x01234567:0);
  FrontendBootLoading boot(session,input,false,audio);
  boot.Update(boot.Current(),15.5f);boot.Update(boot.Current(),.5f);boot.Update(boot.Current(),.5f);boot.Update(boot.Current(),.25f);
  Check(boot.Status().phase==0,"Audio admission test missed ESRB phase");
  const auto before=boot.Current();const auto status=boot.Status();const auto rng=seed;
  if(invalid_device)
  {
   Reject([&]{boot.Update(before,.25f);});
   Check(boot.Current()==before&&session->Current()==before&&seed==rng&&boot.Status().phase==status.phase&&
    audio->Status().state==FrontendBootAudioState::Loaded,"Failed real audio device published boot state or RNG");
   continue;
  }
  unsigned failed=0;bool committed=false;
  // Sweep clone, original-step, retained graph/layout and audio preparation
  // allocations. A started stream must never survive a failed publication.
  for(long budget=0;budget<4096&&!committed;++budget)
  {
   allocation_budget=budget;
   try{boot.Update(before,.25f);committed=true;}catch(const std::bad_alloc&){++failed;}
   allocation_budget=-1;
   if(!committed)Check(boot.Current()==before&&session->Current()==before&&seed==rng&&boot.Status().phase==status.phase&&
    audio->Status().state==FrontendBootAudioState::Loaded,"Failed boot allocation admitted audio or partial scene");
  }
  Check(committed&&failed>50,"Boot transaction sweep missed scene and audio allocations");
  Check(Active(boot.Current())=="NLG"&&boot.Status().phase==3&&boot.Status().boundary==FrontendBootBoundary::None&&seed!=rng,
    "Successful audio admission failed to publish original NLG phase");
  Check(audio->Status().sample.has_value(),"Boot audio produced no selected sample");
  Reject([&]{boot.Reset(boot.Current());});
  boot.Update(boot.Current(),.5f);
  Check(boot.Status().phase==4&&audio->Status().state==FrontendBootAudioState::Unloaded,"Original phase3 did not unload real audio");
  boot.Update(boot.Current(),.25f);Check(Active(boot.Current())=="NLG","Final loading slide selected before original delay");
  boot.Update(boot.Current(),.25f);Check(Active(boot.Current())=="Slide1"&&boot.Status().elapsed==-1,"Original half-second final slide transition differs");
  const auto after_seed=seed;boot.Update(boot.Current(),1);Check(seed==after_seed&&boot.Status().phase==4,"Waiting boot scene consumed another audio selection");
  Check(Active(before)=="ESRB","Audio commit changed retained preceding scene");
  std::cout<<"Boot audio transaction rejected "<<failed<<" allocation points before complete publication\n";
 }
}
std::vector<std::uint8_t> ReadAudioFile(const char* path)
{
 unsigned long size=0;void* data=nlLoadEntireFile(path,&size,32,AllocateStart,nullptr,0,nullptr);
 std::unique_ptr<void,void(*)(void*)> owner(data,nlFree);Check(data&&size,"Owned boot audio metadata missing");
 return {static_cast<std::uint8_t*>(data),static_cast<std::uint8_t*>(data)+size};
}
void AudioOwned()
{
 SDL_SetHint(SDL_HINT_AUDIO_DRIVER,"dummy");const auto global=ReadAudioFile("/audio/nlxgs.bun");
 auto calculation=ReadAudioCalculationInitial(global);AudioBankLoad load(ReadAudioBankCatalog(global),25,23);
 const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(15);
 while(load.State()==AudioBankLoadState::Loading){load.Service();Check(std::chrono::steady_clock::now()<deadline,"Owned boot bank timed out");SDL_Delay(1);}
 FrontendInput input;std::array<FrontendPadSample,4> pads{};Neutral(input,pads);
 auto session=std::make_shared<FrontendSession>();session->Begin(Request());Pump(*session);
 unsigned seed=0xabcdef12;auto audio=std::make_shared<FrontendBootAudio>(load.Result(),calculation,seed);
 FrontendBootLoading boot(session,input,false,audio);unsigned frames=0,nlg_images=0;
 while((boot.Status().phase!=4||boot.Status().elapsed>=0)&&frames<3000)
 {
  boot.Update(boot.Current(),1.f/60);++frames;
  Check(boot.Status().boundary==FrontendBootBoundary::None,"Owned audio boot hit unavailable service");
  if(boot.Status().phase==3)nlg_images+=boot.Current()->layout.ImageCount();
 }
 Check(frames<3000&&nlg_images>60&&Active(boot.Current())=="Slide1"&&audio->Status().state==FrontendBootAudioState::Unloaded,
    "Owned logo/audio/unload did not reach original final loading slide");
 std::cout<<"Owned logo/audio boot: "<<frames<<" original updates, "<<nlg_images<<" NLG image entries; phase4 Slide1 waiting for startup services\n";
}
void Generated()
{
 FrontendInput input;auto session=std::make_shared<FrontendSession>();std::array<FrontendPadSample,4> pads{};
 Reject([&]{FrontendBootLoading absent(session,input);});session->Begin(Request());Pump(*session);auto initial=session->Current();
 FrontendBootLoading boot(session,input);Check(Active(boot.Current())=="strap"&&boot.Current()->layout.ImageCount()==1,"Original setup did not select a single strap image");
 Check(Find(initial,"strap","strap_16_9_us").visible&&!Find(boot.Current(),"strap","strap_16_9_us").visible,"Setup did not preserve immutable prior snapshot");
 for(const char* name:{"Slide1","ESRB","strap","nunchuk","NLG"})Check(!Find(boot.Current(),name,"no home").visible,"Original setup left HOME warning visible");
 Neutral(input,pads);pads[0].buttons=0x100;input.Update(pads,0);boot.Update(boot.Current(),.5f);Check(!boot.Status().strap_dismissed,"Strap accepted input before minimum display time");
 input.Update(pads,0);boot.Update(boot.Current(),1);Check(!boot.Status().strap_dismissed,"Strap accepted held input as fresh press");Near(boot.Status().elapsed,1.5f);
 Neutral(input,pads);pads[3].buttons=0x100;input.Update(pads,0);boot.Update(boot.Current(),0);Check(boot.Status().strap_dismissed&&boot.Status().strap_alpha==255,"Original accept transition faded in the same update");
 auto full=boot.Current();boot.Update(full,.125f);Near(boot.Status().strap_alpha,191.25f);Check(Find(boot.Current(),"strap","strap_us").attributes.colour[3]==191,"Original fade byte truncation differs");
 Check(Find(full,"strap","strap_us").attributes.colour[3]==255,"Fade mutated retained prior graph");
 boot.Update(boot.Current(),.125f);Near(boot.Status().strap_alpha,127.5f);boot.Update(boot.Current(),.25f);
 Check(boot.Status().phase==2&&Active(boot.Current())=="nunchuk"&&boot.Status().strap_alpha==0,"Half-second fade did not enter nunchuk");
 Check(boot.Current()->layout.TextCount()==1&&boot.Current()->layout.font_fallbacks.size()==1&&boot.Current()->layout.unavailable.empty(),"Generated nunchuk lost original font fallback");
 boot.Update(boot.Current(),.25f);Check(boot.Status().phase==0&&Active(boot.Current())=="ESRB","Source USA region branch did not select ESRB");
 boot.Update(boot.Current(),.25f);Check(boot.Status().phase==0,"ESRB completed before start plus duration");
 Check(boot.Current()->layout.TextCount()==1&&boot.Current()->layout.unavailable.empty(),"Generated ESRB paragraph is absent");
 boot.Update(boot.Current(),.25f);Check(boot.Status().phase==3&&Active(boot.Current())=="NLG"&&boot.Status().boundary==FrontendBootBoundary::PlayLogoSound,"Boot did not stop at actual logo sound request");
 auto blocked=boot.Current();boot.Update(blocked,60);Check(boot.Current()==blocked,"Blocked audio service advanced scene state");Reject([&]{boot.Update(full,0);});Reject([&]{boot.Update(blocked,-1);});
 // Explicit replay restores the source constructor state, authored resources and setup.
 Neutral(input,pads);boot.Reset(blocked);Check(boot.Status().phase==1&&!boot.Status().strap_dismissed&&boot.Status().boundary==FrontendBootBoundary::None,"Reset retained prior phase or boundary");
 int focused=0;input.PushFocus(&focused);boot.Update(boot.Current(),1.5f);pads[1].buttons=0x100;input.Update(pads,0);boot.Update(boot.Current(),0);
 Check(!boot.Status().strap_dismissed,"Boot input ignored original exclusive focus");boot.Update(boot.Current(),14);Check(boot.Status().strap_dismissed&&boot.Status().strap_alpha==255,"15.5-second auto-dismiss threshold differs");input.PopFocus(&focused);
 // Every source abstract action is supplied by the real retained PadManager mapping.
 for(unsigned mask:{0x1000u,0x10u,0x100u,0x200u,0x40u,0x20u,0x800u,0x400u})
 {Neutral(input,pads);boot.Reset(boot.Current());boot.Update(boot.Current(),1.5f);pads[2].buttons=mask;input.Update(pads,0);boot.Update(boot.Current(),0);Check(boot.Status().strap_dismissed,"Original boot action did not route from pad2");}
 auto stable=boot.Current();const auto state=boot.Status();bool rejected=false;std::thread foreign([&]{try{boot.Update(stable,0);}catch(const std::logic_error&){rejected=true;}});foreign.join();Check(rejected&&boot.Current()==stable,"Foreign boot mutation changed state");
 session->Begin(Request());session->Cancel();boot.Update(stable,0);Check(boot.Status().phase==state.phase,"Cancelled replacement lost retained handler");
 // External mutation invalidates this handler's exact generation rather than silently rebinding IDs.
 session->Reset();Reject([&]{boot.Update(boot.Current(),0);});boot.Release();boot.Release();Reject([&]{boot.Status();});
 // Setup failure must retain all source visibility/clock and previous publication.
 session->Begin(Request("/Art/fe/boot-missing.fen"));Pump(*session);auto malformed=session->Current();Reject([&]{FrontendBootLoading missing(session,input);});Check(session->Current()==malformed,"Failed setup published a partial strap/hidden-warning change");
 // Locale-specific texture references use actual authored resource IDs, including widescreen variants.
 for(auto language:{FrontendLanguage::NAFrench,FrontendLanguage::NASpanish})for(bool wide:{false,true})
 {
  session->Begin(Request("/Art/fe/boot_loading.fen",language));Pump(*session);FrontendBootLoading localized(session,input,wide);
  const auto current=localized.Current();const char* target=wide?"strap_16_9_us":"strap_us";
  const char* replacement=language==FrontendLanguage::NAFrench?(wide?"strap_16_9_French":"strap_French"):(wide?"strap_16_9_Spanish":"strap_Spanish");
  Check(Find(current,"strap",target).resource==Find(current,"art",replacement).resource,"Localized strap did not adopt original authored texture");
  Check(current->layout.ImageCount()==1,"Localized strap selected multiple visible images");
 }
 auto retained=session->Current();std::weak_ptr<FrontendSession> lifetime=session;
 {FrontendBootLoading holder(session,input);session->Begin(Request());session.reset();Check(!lifetime.expired(),"Boot owner lost retained session");}
 Check(lifetime.expired()&&!nlAsyncReadsPending(nullptr)&&retained->images,"Boot destruction failed to drain/release while preserving retained resources");
}
void Owned()
{
 FrontendInput input;auto session=std::make_shared<FrontendSession>();std::array<FrontendPadSample,4> pads{};Neutral(input,pads);
 for(auto language:{FrontendLanguage::English,FrontendLanguage::NAFrench,FrontendLanguage::NASpanish})
 {
  session->Begin(Request("/Art/fe/boot_loading.fen",language));Pump(*session);FrontendBootLoading boot(session,input);
  auto retained=boot.Current();
  std::cout<<"Owned initial slide "<<Active(retained)<<", reads "<<retained->image_completed_files<<", image entries "<<retained->layout.ImageCount()<<"\n";
  for(const auto& [reason,count]:retained->layout.unavailable)std::cout<<"  unavailable "<<reason<<": "<<count<<"\n";
  Check(Active(retained)=="strap"&&retained->image_completed_files==1,"Owned strap/minibundle setup differs");
  boot.Update(boot.Current(),0);auto first=boot.Current();Check(first->layout.ImageCount()==1,"Owned first updated strap frame has no single image");
  unsigned frames=0,nunchuk_text_frames=0,esrb_text_frames=0;
  while(boot.Status().boundary==FrontendBootBoundary::None&&frames<2400)
  {
   boot.Update(boot.Current(),1.f/60);++frames;
   const auto frame=boot.Current();
   const auto slide=std::find_if(frame->graph.slides.begin(),frame->graph.slides.end(),[&](const auto& s){return s.offset==frame->graph.active_slide;});
   if((boot.Status().phase==2||boot.Status().phase==0)&&slide->time>.5f&&slide->time<2.f)
   {
    Check(frame->layout.TextCount()==1&&frame->layout.unavailable.empty(),"Owned boot text is missing during its authored visible interval");
    const auto entry=std::find_if(frame->layout.entries.begin(),frame->layout.entries.end(),[](const auto& e){return std::holds_alternative<FrontendLayoutText>(e);});
    const auto& text=std::get<FrontendLayoutText>(*entry);Check(!text.layout.quads.empty(),"Owned boot text produced no glyphs");
    if(boot.Status().phase==2)
    {
     ++nunchuk_text_frames;
     Check(frame->layout.ImageCount()==1&&frame->layout.font_fallbacks.at(0x501e5791)==frame->visuals->font_registration_order.front()->alias,
        "Owned nunchuk did not use actual first registered font");
    }
    else
    {
     ++esrb_text_frames;Check(frame->layout.font_fallbacks.empty(),"Owned ESRB failed its exact font alias");
     Check(text.text.find(u"{p}")!=std::u16string::npos,"Owned ESRB lost retained paragraph token");
     Check(std::any_of(text.layout.quads.begin(),text.layout.quads.end(),[&](const auto& q){return std::abs(q.top-text.layout.quads.front().top)>text.layout.font->height*.5f;}),
        "Owned ESRB paragraph did not produce distinct original rows");
    }
   }
  }
  Check(nunchuk_text_frames>30&&esrb_text_frames>30,"Owned boot visual audit missed supported authored intervals");
  std::cout<<"Owned visible text frames: nunchuk "<<nunchuk_text_frames<<", ESRB "<<esrb_text_frames<<", font registration first "<<std::hex<<retained->visuals->font_registration_order.front()->alias<<std::dec<<"\n";
  Check(boot.Status().boundary==FrontendBootBoundary::PlayLogoSound&&boot.Status().phase==3&&Active(boot.Current())=="NLG","Owned original retail prefix did not reach explicit audio stop");
  Check(Active(retained)=="strap"&&first->layout.ImageCount()==1,"Owned retained initial frame was mutated");
  std::cout<<"Owned retail boot language "<<int(language)<<": "<<frames<<" frames, "<<retained->images->textures.size()<<" textures, audio boundary 23/de83984e\n";
  boot.Reset(boot.Current());Check(Active(boot.Current())=="strap"&&boot.Status().elapsed==0,"Owned replay setup differs");
 }
}
struct Host
{
 bool live=false,disc=false;
 ~Host(){if(live)ResetStartupFiles();if(disc)aurora_dvd_close();if(live){ResetStartupMemory();aurora_shutdown();}}
};
}
int main(int argc,char**argv)
{
 try
 {
  Check(argc==4,"Supply disc, output directory and mode");const bool owned=std::string_view(argv[3])=="owned";
  const auto folder=(std::filesystem::path(argv[2])/"frontend-boot-data").string();std::filesystem::create_directories(folder);
  AuroraConfig config{};config.appName="Charged retail frontend boot";config.userPath=config.cachePath=folder.c_str();config.resourcesPath=SDL_GetBasePath();
  config.desiredBackend=BACKEND_NULL;config.windowWidth=320;config.windowHeight=240;config.windowPosX=config.windowPosY=-1;config.logLevel=LOG_WARNING;
  config.mem1Size=MEM1_DEFAULT_SIZE;config.mem2Size=64*1024*1024;
  Host host;const auto state=aurora_initialize(argc,argv,&config);host.live=true;Check(state.window,"Aurora initialization failed");
  InitializeStartupOS();nlInitMemory();Check(aurora_dvd_open(argv[1]),"Cannot open boot disc");host.disc=true;nlInitFileSystem();
  for(unsigned repeat=0;repeat<3;++repeat)
  {
   const auto a=StandardAllocator.TotalFreeMemory(),b=VirtualAllocator.TotalFreeMemory();if(owned){Owned();AudioOwned();}else{Generated();AudioGenerated();}
   Check(!nlAsyncReadsPending(nullptr)&&StandardAllocator.TotalFreeMemory()==a&&VirtualAllocator.TotalFreeMemory()==b,"Handler lifetime failed native cleanup");
  }
  std::cout<<checks<<" frontend boot checks passed\n";
 }
 catch(const std::exception& e){allocation_budget=-1;std::cerr<<"FAILED: "<<e.what()<<" (check "<<checks<<")\n";return 1;}
}
