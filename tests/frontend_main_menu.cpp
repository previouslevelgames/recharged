#include "runtime/frontend_main_menu.h"
#include "runtime/startup.h"
#include "runtime/startup_files.h"
#include "audio_bank_fixture.h"
#include "NL/nlFileGC.h"
#include "NL/nlFile.h"
#include "NL/nlMemory.h"
#include "NL/MemAlloc.h"
#include <aurora/aurora.h>
#include <aurora/dvd.h>
#include <SDL3/SDL.h>
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <thread>
using namespace mscharged;
using namespace mscharged::resources;
namespace
{
unsigned checks=0;
void Check(bool value,const char* message){++checks;if(!value)throw std::runtime_error(message);}
template<class F>void Reject(F action){++checks;try{action();}catch(const std::exception&){return;}throw std::runtime_error("Invalid Main Menu operation accepted");}
std::vector<std::uint8_t> Load(const char* path)
{
    unsigned long size=0;void* data=nlLoadEntireFile(path,&size,32,AllocateStart,nullptr,0,nullptr);
    std::unique_ptr<void,void(*)(void*)> storage(data,nlFree);Check(data&&size,"Main audio input is missing");
    return {static_cast<std::uint8_t*>(data),static_cast<std::uint8_t*>(data)+size};
}
void Pump(FrontendSession& session)
{
    const auto limit=std::chrono::steady_clock::now()+std::chrono::seconds(15);
    while(session.State()==FrontendSessionState::Loading){session.Service();Check(std::chrono::steady_clock::now()<limit,"Main scene read timed out");SDL_Delay(1);}session.Result();
}
std::shared_ptr<FrontendSession> Session(const char* path="/Art/fe/main_menu_v3.fen")
{auto s=std::make_shared<FrontendSession>();s->Begin({path,FrontendLanguage::English,FrontendImageProfile::Main,"MAIN",true});Pump(*s);return s;}
std::shared_ptr<FrontendAudio> Audio(bool owned)
{
    if(owned)
    {
        const auto global=Load("/audio/nlxgs.bun");auto catalog=ReadAudioBankCatalog(global);AudioBankLoad load(catalog,23,21);
        const auto limit=std::chrono::steady_clock::now()+std::chrono::seconds(15);
        while(load.State()==AudioBankLoadState::Loading){load.Service();Check(std::chrono::steady_clock::now()<limit,"Menu bank read timed out");SDL_Delay(1);}
        return std::make_shared<FrontendAudio>(load.Result(),ReadAudioCalculationInitial(global),AudioVoicesOptions{32});
    }
    using namespace audio_bank_fixture;
    auto f=Make();Put(f.bytes,f.map+20,0x6b0689d4);Put(f.bytes,f.cues+40,0x6b0689d4);
    auto bank=std::make_shared<const LoadedAudioBank>(LoadedAudioBank{23,21,{23,"FE_GEN_Sfx"},{21,0,1,false},ReadAudioResidentBank(f.bytes,f.wave)});
    Data section;Append(section,0x23401,Words({2,0xf1000100,0}));Append(section,0x23402,Words({0,0,0,0,0,0,1,0,0,0xf1000100,0,0}));
    return std::make_shared<FrontendAudio>(bank,ReadAudioCalculationInitial(Wrap(0x80000001,Wrap(0x80023400,section))),AudioVoicesOptions{32});
}
void Input(FrontendInput& input,bool press=false)
{std::array<FrontendPadSample,4> pads{};for(auto& pad:pads)pad.connected=true;pads[0].buttons=press?0x100:0;input.Update(pads,1.f/60);}
FrontendPointerViewport Viewport(){return {1,960,720,1920,1440,0,0,1920,1440};}
void Ack(FrontendMainMenu& menu){menu.Acknowledge(menu.Current(),Viewport());}
std::array<float,2> Center(const FrontendPointerBounds& b){return {(b.min_x+b.max_x)/2,(b.min_y+b.max_y)/2};}
unsigned NextSeed(unsigned value){const auto a=value^0x1d872b41u,b=a^(a>>5);return b^a^(b<<27);}
void DrainAudio(FrontendAudio& audio)
{
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(3);
    while(!audio.Handles().empty())
    {
        audio.ServiceAudio();audio.Update(1.f/60);SDL_Delay(2);
        Check(std::chrono::steady_clock::now()<deadline,"Original menu cue lifecycle did not complete");
    }
}
std::string Feedback(const FrontendSession::Handle& frame,unsigned item)
{
    constexpr const char* items[] = {"Domination_Menu_Item","Online _Menu_Item","RTSC_Menu_Item","Challenges_Menu_Item","101_Menu_Item","HOF_Menu_Item","Options_Menu_Item"};
    constexpr const char* names[] = {"GRUDGE","online","quest","scenario","HOF","HOF","HOF"};
    const std::array<std::string_view,3> path{"Layer",items[item],names[item]};
    const auto node=FindFrontendNode(frame->graph,{},FrontendNamedPath(path),FrontendNodeType::Component);Check(node.has_value(),"Actual authored highlight is absent");
    const auto instance=std::find_if(frame->graph.instances.begin(),frame->graph.instances.end(),[&](const auto& v){return v.offset==node->id;});
    const auto lib=std::find_if(frame->graph.library.begin(),frame->graph.library.end(),[&](const auto& v){return v.offset==instance->library;});
    const auto slide=std::find_if(frame->graph.slides.begin(),frame->graph.slides.end(),[&](const auto& v){return v.offset==lib->active_slide;});Check(slide!=frame->graph.slides.end(),"Actual highlight has no active slide");return slide->name;
}
bool DescriptionVisible(const FrontendSession::Handle& frame)
{
    const std::array<std::string_view,2> path{"Layer","Text"};
    const auto node=FindFrontendNode(frame->graph,{},FrontendNamedPath(path),FrontendNodeType::Text);Check(node.has_value(),"Authored Main description is absent");
    const auto found=std::find_if(frame->graph.instances.begin(),frame->graph.instances.end(),[&](const auto& v){return v.offset==node->id;});
    Check(found!=frame->graph.instances.end(),"Description instance is absent");return found->visible;
}
void AdvanceIntro(FrontendMainMenu& menu)
{
    const auto first=menu.Current();unsigned updates=0;
    while(!menu.Status().interactive&&updates++<1200)menu.AdvanceVisual(menu.Current(),1.f/60);
    Check(menu.Status().interactive,"Source MAIN intro never completed");Check(menu.Current()!=first,"Original base update did not advance retained visuals");Ack(menu);
}
void SourceLifecycle(bool owned,bool default_arrows=false)
{
    FrontendInput input;Input(input);auto audio=Audio(owned);auto session=Session(default_arrows?"/Art/fe/main-default-arrows.fen":"/Art/fe/main_menu_v3.fen");unsigned seed=0xabcdef12;
    FrontendMainMenu menu(session,input,audio,seed);Check(!menu.Status().interactive,"Menu ignored authored MAIN intro");Reject([&]{menu.Bounds();});
    if(!owned)for(bool value:menu.Status().default_arrows)Check(value==default_arrows,"Optional arrows did not retain the original empty-component contract");
    Ack(menu);menu.DeliverPointer(menu.Current(),{0,{0,0},true});Check(!menu.Status().selection,"Intro accepted a source press before binding");
    AdvanceIntro(menu);const auto bounds=menu.Bounds();
    if(!owned)for(unsigned i=0;i<7;++i)
    {
        Check(std::abs(bounds[i].min_x-(-260+float(i)*80))<1e-4f&&std::abs(bounds[i].max_x-(-220+float(i)*80))<1e-4f
            &&std::abs(bounds[i].min_y-20)<1e-4f&&std::abs(bounds[i].max_y-60)<1e-4f,"Independent generated 40x40 authored bounds differ");
    }
    for(unsigned i=0;i<7;++i)
    {
        auto prior=menu.Current();const auto count=audio->ActiveCount(0x6b0689d4);const auto rng=seed;
        menu.DeliverPointer(prior,{0,Center(bounds[i])});Check(menu.Status().highlighted[0]>0&&menu.Status().pointer_states[i][0]==1,"Original item Enter failed");
        Check(Feedback(menu.Current(),i)=="over"&&Feedback(prior,i)=="off","Highlight changed the retained preceding frame");
        Check(!DescriptionVisible(menu.Current())&&DescriptionVisible(prior),"Original hover did not hide the description on the replacement frame");
        Check(seed==(owned?rng:NextSeed(rng))&&audio->ActiveCount(0x6b0689d4)>count,"Source hover did not preserve its original selection/RNG contract");
        if(owned){const auto handles=audio->Handles();Check(handles.size()==1&&audio->Status(handles[0]).sample==1&&!audio->Status(handles[0]).limited,"Retail hover did not select actual sample1");}
        Reject([&]{menu.Acknowledge(prior,Viewport());});Ack(menu);
        menu.DeliverPointer(menu.Current(),{0,{-999,-999}});Check(menu.Status().highlighted[0]==0&&Feedback(menu.Current(),i)=="off","Original item Leave failed");Ack(menu);
        Check(DescriptionVisible(menu.Current()),"Original Leave did not restore the description");
        DrainAudio(*audio);
    }
    // Preserve source HasOtherPointerState behavior, including the second
    // pointer's state staying0 while another pointer already has state1.
    menu.DeliverPointer(menu.Current(),{0,Center(bounds[0])});Ack(menu);
    const auto count=audio->ActiveCount(0x6b0689d4);menu.DeliverPointer(menu.Current(),{1,Center(bounds[0])});Ack(menu);
    Check(menu.Status().highlighted[1]==1&&menu.Status().pointer_states[0][1]==0&&audio->ActiveCount(0x6b0689d4)==count,"Source second-pointer suppression differs");
    menu.DeliverPointer(menu.Current(),{0,{-999,-999}});Ack(menu);Check(Feedback(menu.Current(),0)=="off","Source other-pointer query was silently rewritten");
    menu.DeliverPointer(menu.Current(),{1,{-999,-999}});Ack(menu);Check(menu.Status().highlighted[1]==0,"Second pointer count did not close");
    DrainAudio(*audio);
    const auto before=menu.Current();menu.DeliverPointer(before,{0,Center(bounds[6]),true});
    const auto selected=menu.Status().selection;Check(selected&&selected->item==6&&selected->service==FrontendMainSelectionService::ApplyItem&&selected->source==before,"Original Select dispatch lost Options or exact source frame");
    Ack(menu);menu.DeliverPointer(menu.Current(),{0,Center(bounds[0]),true});Check(menu.Status().selection->item==6,"Awaiting ApplyItem service admitted another action");
    bool wrong=false;std::thread thread([&]{try{menu.Status();}catch(const std::logic_error&){wrong=true;}});thread.join();Check(wrong,"Foreign menu thread accepted");
    auto retained=menu.Current();menu.Release();menu.Release();Check(audio->Handles().empty()&&audio->ActiveCount(0x6b0689d4)==0,"Menu release retained its source sounds");
    Check(retained->images&&retained->visuals&&Feedback(retained,6)=="over","Menu release lost immutable presented resources");Reject([&]{menu.Current();});audio->Unload();session.reset();
    if(!owned)
    {
        auto malformed=Session("/Art/fe/main-missing.fen");const auto current=malformed->Current();auto owner=Audio(false);
        Reject([&]{FrontendMainMenu missing(malformed,input,owner,seed);});Check(malformed->Current()==current,"Failed Main setup published partial component changes");owner->Unload();
        auto blocked=Session();owner=Audio(false);FrontendMainMenu media(blocked,input,owner,seed,true);AdvanceIntro(media);
        media.DeliverPointer(media.Current(),{0,Center(media.Bounds()[6]),true});Check(!media.Status().selection&&Feedback(media.Current(),6)=="over","Original media build item rejection changed selection");media.Release();owner->Unload();
    }
}
void HostLifecycle(bool owned)
{
    FrontendInput input;Input(input);auto audio=Audio(owned);auto session=Session();unsigned seed=17;
    FrontendMainMenu menu(session,input,audio,seed);AdvanceIntro(menu);
    const auto v=Viewport();const auto center=Center(menu.Bounds()[6]);
    FrontendPointerDesktopSample sample{v.window,v.window_width,v.window_height,v.pixel_width,v.pixel_height,0,11,
        (center[0]/640.f+.5f)*float(v.window_width),(.5f-center[1]/480.f)*float(v.window_height),true,true};
    const auto route=[&]{++sample.sequence;auto result=menu.Route(sample);Ack(menu);return result;};
    Check(!route().active&&!menu.Status().selection,"New Main presentation bypassed neutral admission");
    Check(route().active&&Feedback(menu.Current(),6)=="over","Desktop Main hover did not execute original feedback");
    sample.focused=false;sample.primary_down=true;
    Check(!route().active&&Feedback(menu.Current(),6)=="off"&&!menu.Status().selection,"Focus loss leaked selection or retained hover");
    sample.focused=true;Check(!route().active&&!menu.Status().selection,"Held mouse reactivation bypassed neutral gate");
    sample.primary_down=false;Check(!route().active&&!menu.Status().selection,"Neutral mouse recovery admitted a press");
    Check(route().active&&Feedback(menu.Current(),6)=="over","Mouse did not recover after neutral observation");
    sample.captured=true;Check(!route().active&&Feedback(menu.Current(),6)=="off","ImGui capture retained actionable menu hover");
    sample.captured=false;sample.primary_down=true;Check(!route().active&&!menu.Status().selection,"Captured held button leaked a Main action");
    sample.primary_down=false;Check(!route().active,"Capture recovery skipped neutral observation");Check(route().active,"Capture recovery failed");
    Input(input,true);const auto source=menu.Current();const auto dispatch=route();
    Check(dispatch.active&&dispatch.event.pressed&&menu.Status().selection&&menu.Status().selection->item==6
        &&menu.Status().selection->source==source,"Original action30 did not dispatch Options on its presented frame");
    menu.Release();Check(audio->Handles().empty(),"Desktop menu release retained owned sounds");audio->Unload();
}
struct Host
{bool live=false,disc=false;~Host(){if(live)ResetStartupFiles();if(disc)aurora_dvd_close();if(live){ResetStartupMemory();aurora_shutdown();}}};
}
int main(int argc,char** argv)
{
    try
    {
        Check(argc==4,"Supply disc, output folder and generated/owned mode");const bool owned=std::string_view(argv[3])=="owned";Check(owned||std::string_view(argv[3])=="generated","Unknown menu test mode");
        const auto folder=(std::filesystem::path(argv[2])/"frontend-main-test-data").string();std::filesystem::create_directories(folder);
        AuroraConfig config{};config.appName="Charged original Main menu callbacks";config.userPath=config.cachePath=folder.c_str();config.resourcesPath=SDL_GetBasePath();
        config.desiredBackend=BACKEND_NULL;config.windowWidth=320;config.windowHeight=240;config.windowPosX=config.windowPosY=-1;
        config.mem1Size=MEM1_DEFAULT_SIZE;config.mem2Size=64*1024*1024;config.logLevel=LOG_WARNING;
        Host host;auto state=aurora_initialize(argc,argv,&config);host.live=true;Check(state.window,"Aurora core failed");InitializeStartupOS();nlInitMemory();
        Check(aurora_dvd_open(argv[1]),"Cannot mount menu test disc");host.disc=true;nlInitFileSystem();
        for(unsigned repeat=0;repeat<3;++repeat)
        {
            const auto a=StandardAllocator.TotalFreeMemory(),b=VirtualAllocator.TotalFreeMemory();SourceLifecycle(owned);HostLifecycle(owned);if(!owned)SourceLifecycle(false,true);
            Check(!nlAsyncReadsPending(nullptr)&&StandardAllocator.TotalFreeMemory()==a&&VirtualAllocator.TotalFreeMemory()==b,"Main menu did not recover files/arenas");
        }
        std::cout<<checks<<" original Main visual/callback checks passed; ApplyItem/music/save services pending\n";
    }
    catch(const std::exception& e){std::cerr<<"FAILED: "<<e.what()<<" (check "<<checks<<")\n";return 1;}
}
