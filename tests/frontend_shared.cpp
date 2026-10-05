#include "runtime/frontend_session.h"
#include "runtime/startup.h"
#include "runtime/startup_files.h"
#include "NL/MemAlloc.h"
#include "NL/nlFile.h"
#include "NL/nlFileGC.h"
#include "NL/nlMemory.h"
#include <aurora/aurora.h>
#include <aurora/dvd.h>
#include <SDL3/SDL.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <source_location>
#include <thread>
#include <type_traits>
using namespace mscharged;
using namespace mscharged::resources;
namespace {
unsigned checks = 0;
void Check(bool condition, const char* message) { ++checks; if (!condition) throw std::runtime_error(message); }
template<class F> void Reject(F f, std::source_location at = std::source_location::current())
{ ++checks; try { f(); } catch (const std::exception&) { return; } throw std::runtime_error("Accepted invalid operation at line " + std::to_string(at.line())); }
static_assert(!std::is_default_constructible_v<FrontendSessionResources>);
static_assert(!std::is_copy_constructible_v<FrontendSessionResources>);
void Decoder()
{
    using Data = std::vector<std::uint8_t>;
    const auto put=[](Data& data,std::size_t at,std::uint32_t value)
    {for(unsigned i=0;i<4;++i)data.at(at+i)=std::uint8_t(value>>(24-8*i));};
    const auto bundle=[&](unsigned count)
    {
        const std::size_t start=(32+12*count+31)&~std::size_t(31);
        Data bytes(start+count*64);put(bytes,0,32);put(bytes,4,count);put(bytes,8,1);put(bytes,12,start/32);
        for(unsigned i=0;i<count;++i)
        {
            put(bytes,32+12*i,i);put(bytes,36+12*i,(start+64*i)/32);put(bytes,40+12*i,64);
            put(bytes,start+64*i,1);put(bytes,start+64*i+4,2);bytes[start+64*i+15]=8;bytes[start+64*i+17]=8;
            std::fill_n(bytes.begin()+start+64*i+32,32,std::uint8_t(i));
        }
        return bytes;
    };
    auto maximum=bundle(1024);auto all=ReadPermanentFrontendImages(maximum);
    Check(all->textures.size()==1024,"Permanent directory boundary rejected");
    for(unsigned i=0;i<1024;++i)Check(all->textures.at(i)->pixels[0]==std::uint8_t(i),"Independent directory/pixel identity differs");
    Reject([&]{ReadPermanentFrontendImages(bundle(1025));});
    auto duplicate=bundle(2);put(duplicate,44,0);put(duplicate,128,0xffffffffU);
    Check(ReadPermanentFrontendImages(duplicate)->textures.size()==1,"First original hash did not hide duplicate payload");
    auto invalid=bundle(2);put(invalid,128+4,99);Reject([&]{ReadPermanentFrontendImages(invalid);});
    invalid=bundle(2);put(invalid,32,0xffffffffU);Reject([&]{ReadPermanentFrontendImages(invalid);});
    invalid=bundle(2);put(invalid,48,2);Reject([&]{ReadPermanentFrontendImages(invalid);});
    invalid=bundle(2);invalid.pop_back();Reject([&]{ReadPermanentFrontendImages(invalid);});
    invalid=bundle(2);put(invalid,4,4097);Reject([&]{ReadPermanentFrontendImages(invalid);});
    Data empty(16);put(empty,0,32);put(empty,8,1);put(empty,12,1);
    Check(ReadPermanentFrontendImages(empty)->textures.empty(),"Original empty permanent bundle rejected");
}
FrontendSessionRequest Request(const char* path = "/Art/fe/session.fen")
{ FrontendSessionRequest request; request.path = path; return request; }
void Pump(FrontendSession& session, bool tolerate = false)
{
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (session.State() == FrontendSessionState::Loading)
    {
        try { session.Service(); } catch (...) { if (!tolerate) throw; }
        if (std::chrono::steady_clock::now() >= end) throw std::runtime_error("Shared frontend read timed out");
        SDL_Delay(1);
    }
}
struct Overlay
{
    enum Mode { Fail, Short, Block } mode = Fail;
    std::atomic<unsigned> opened{0}, active{0}, reads{0};
    std::mutex mutex; std::condition_variable cv; bool released = false;
    struct File { Overlay* owner; std::size_t position = 0; };
    static void* Open(void* p) { auto* o=static_cast<Overlay*>(p); ++o->opened; ++o->active; return new File{o}; }
    static void Close(void* p) { auto* f=static_cast<File*>(p); --f->owner->active; delete f; }
    static std::int64_t Seek(void* p,std::int64_t at,std::int32_t origin)
    { if(origin || at<0 || at>128)return -1;return static_cast<File*>(p)->position=at; }
    static std::int64_t Read(void* p,std::uint8_t* bytes,std::size_t size)
    {
        auto& f=*static_cast<File*>(p); auto& o=*f.owner; ++o.reads;
        if(o.mode==Fail)return -1;
        if(o.mode==Block)
        {
            std::unique_lock lock(o.mutex);
            if(!o.cv.wait_for(lock,std::chrono::seconds(2),[&]{return o.released;}))return -1;
        }
        const auto n=std::min(size,o.mode==Short?(f.position?std::size_t(0):std::size_t(3)):128-f.position);
        std::fill_n(bytes,n,0);f.position+=n;return n;
    }
    Overlay(std::initializer_list<const char*> paths,Mode selected=Fail):mode(selected)
    {
        const AuroraOverlayCallbacks callbacks{Open,Close,Read,Seek}; aurora_dvd_overlay_callbacks(&callbacks);
        std::vector<AuroraOverlayFile> entries;for(auto path:paths)entries.push_back({path,this,128});
        aurora_dvd_overlay_files(entries.data(),entries.size(),nullptr);
    }
    ~Overlay(){ Release();aurora_dvd_overlay_files(nullptr,0,nullptr);if(active)std::terminate(); }
    void Release(){ {std::lock_guard lock(mutex);released=true;}cv.notify_all(); }
    void Wait(){const auto end=std::chrono::steady_clock::now()+std::chrono::seconds(2);while(!reads){if(std::chrono::steady_clock::now()>end)throw std::runtime_error("Blocked FEN read did not start");SDL_Delay(1);}}
};
void InspectShared(FrontendSession& session,const FrontendSession::Handle& first,FrontendSessionResources::Handle token)
{
    auto frame=session.Result();Check(frame->visuals==first->visuals&&frame->images==first->images,"Shared scene duplicated resource owners");
    Check(session.SharedResources()==token,"Shared scene changed opaque resource identity");
    Check(session.Progress().fen_completed&&!session.Progress().visual_completed_mask&&!session.Progress().image_completed_files,
        "Shared scene performed duplicate visual/image reads");
    Check(frame->image_completed_files==0,"Shared frame claims image reads it did not perform");
}
FrontendSession::Handle Generated()
{
    FrontendSession donor, receiver;
    Reject([&]{donor.SharedResources();});
    donor.Begin(Request(),FrontendSessionResourcesMode::PermanentMain);Pump(donor);
    auto first=donor.Result();auto token=donor.SharedResources();
    Check(first->images->textures.size()==3&&first->image_completed_files==1,"Permanent loader did not retain all unique entries");
    Check(donor.Progress().visual_completed_mask==7,"Permanent session omitted fonts/localization");
    Check(first->layout.ImageCount()==1&&first->layout.entries.size()==1,"Initial generated layout differs");
    const auto& source=std::get<FrontendLayoutImage>(first->layout.entries[0]);
    Check(source.texture->pixels[0]==65,"First directory hash did not win");
    donor.Advance(.25f);Check(donor.SharedResources()==token,"Timeline update lost resource proof");
    donor.Pop();Reject([&]{donor.SharedResources();});
    {
        // Any attempted dependency open/read now fails. Only the NEW FEN is
        // available, so completed shared scenes prove actual read isolation.
        Overlay forbidden({"/art/fe/MainUI.Dmn","/art/fe/english.loc","/art/fe/fonts/eurfonttext18.res","/art/fe/fonts/eurfontheading36.res"});
        receiver.BeginShared(Request("/Art/fe/nav.fen"),token);Pump(receiver);InspectShared(receiver,first,token);
        auto nav=receiver.Current();Check(std::get<FrontendLayoutImage>(nav->layout.entries[0]).texture->pixels[0]==91,"New FEN did not resolve its independent image");
        receiver.Advance(.5f);Check(receiver.Current()->graph.presentation_time==.5f&&first->graph.presentation_time==0,"Shared timeline mutated donor");
        receiver.BeginShared(Request(),token);Check(receiver.State()==FrontendSessionState::Loading,"Replacement was not queued");
        auto incompatible=Request();incompatible.language=FrontendLanguage::NAFrench;
        Reject([&]{receiver.BeginShared(incompatible,token);});
        incompatible=Request();incompatible.image_profile=FrontendImageProfile::InGame;
        Reject([&]{receiver.BeginShared(incompatible,token);});Reject([&]{receiver.BeginShared(Request(),{});});
        Reject([&]{receiver.Begin(incompatible,FrontendSessionResourcesMode::PermanentMain);});
        Reject([&]{receiver.Begin(Request(),static_cast<FrontendSessionResourcesMode>(99));});
        Check(receiver.State()==FrontendSessionState::Loading&&receiver.SharedResources()==token,"Incompatible admission disturbed pending work");
        Pump(receiver);InspectShared(receiver,first,token);
        receiver.BeginShared(Request("/Art/fe/hidden-present.fen"),token);Pump(receiver);
        Check(receiver.Result()->layout.entries.empty(),"Hidden-image fixture did not select an empty slide");
        for(const char* path:{"/Art/fe/bad.fen","/Art/fe/missing-image.fen","/Art/fe/hidden-missing.fen","/Art/fe/absent.fen"})
        {
            auto old=receiver.Current();try{receiver.BeginShared(Request(path),token);Pump(receiver);}catch(const std::exception&){}
            Check(receiver.State()==FrontendSessionState::Failed&&receiver.Current()==old&&receiver.SharedResources()==token,"Failed shared replacement lost current/proof");Reject([&]{receiver.Result();});
        }
        auto old=receiver.Current();receiver.BeginShared(Request(),token);receiver.Cancel();receiver.Cancel();
        Check(receiver.Current()==old&&receiver.SharedResources()==token&&!nlAsyncReadsPending(nullptr),"Cancellation lost retained state or reads");
        receiver.BeginShared(Request(),token);receiver.BeginShared(Request("/Art/fe/nav.fen"),token);Pump(receiver);InspectShared(receiver,first,token);
        Check(!forbidden.opened&&!forbidden.reads&&!forbidden.active,"Shared session touched localization/font/DMN dependencies");
    }
    for(auto mode:{Overlay::Fail,Overlay::Short,Overlay::Block})
    {
        auto old=receiver.Current();Overlay fault({"/art/fe/nav.fen"},mode);
        receiver.BeginShared(Request("/Art/fe/nav.fen"),token);
        if(mode==Overlay::Block)
        {
            fault.Wait();std::jthread unblock([&]{SDL_Delay(10);fault.Release();});receiver.Cancel();
            Check(receiver.State()==FrontendSessionState::Cancelled,"Active cancellation changed state");
        }
        else {Pump(receiver,true);Check(receiver.State()==FrontendSessionState::Failed,"FEN read error became ready");}
        Check(receiver.Current()==old&&receiver.SharedResources()==token&&!fault.active,"FEN failure retained worker storage");
    }
    bool wrong=false;std::thread thread([&]{try{receiver.BeginShared(Request(),token);}catch(const std::logic_error&){wrong=true;}});thread.join();Check(wrong,"Cross-thread shared mutation accepted");
    struct Callback
    {
        FrontendSession* session;FrontendSessionResources::Handle token;bool ran=false;unsigned pending=0;
        ~Callback(){if(pending)nlCancelEntireFileLoad(pending,nullptr);}
        static void Run(void* data,unsigned long,void* context)
        {
            std::unique_ptr<void,void(*)(void*)> owned(data,nlFree);auto& c=*static_cast<Callback*>(context);c.ran=true;c.pending=0;
            Reject([&]{c.session->BeginShared(Request(),c.token);});Reject([&]{c.session->Pop();});
            throw std::runtime_error("Actual shared NL callback failure");
        }
    } callback{&receiver,token};
    auto old=receiver.Current();callback.pending=nlLoadEntireFileAsync("/art/fe/session.fen",Callback::Run,&callback,32,AllocateEnd,nullptr,0,&VirtualAllocator);
    receiver.BeginShared(Request(),token);Pump(receiver,true);
    Check(callback.ran&&receiver.State()==FrontendSessionState::Failed&&receiver.Current()==old,"Shared service exception published partial frame");
    receiver.BeginShared(Request(),token);nlShutdownFileSystem();receiver.Poll();
    Check(receiver.State()==FrontendSessionState::Failed&&receiver.Current()==old,"NL shutdown left shared scene pending");nlInitFileSystem();
    receiver.BeginShared(Request("/Art/fe/nav.fen"),token);Pump(receiver);auto retained=receiver.Result();receiver.Pop();
    donor.Begin(Request("/Art/fe/text.fen"));Pump(donor);Reject([&]{donor.SharedResources();});
    Check(donor.Result()->images->textures.empty()&&donor.Result()->image_completed_files==0,"Ordinary text-only Begin changed behavior");
    donor.Pop();return retained;
}
std::uint32_t Hash(Bytes bytes){std::uint32_t h=2166136261;for(auto b:bytes)h=(h^b)*16777619U;return h;}
FrontendSession::Handle Owned(bool print)
{
    auto first=std::make_unique<FrontendSession>();auto request=Request("/Art/fe/options_main_menu.fen");request.initial_slide="in";
    first->Begin(request,FrontendSessionResourcesMode::PermanentMain);Pump(*first);auto options=first->Result();auto token=first->SharedResources();
    Check(options->images->textures.size()==398,"Owned MainUI texture count differs");
    if(print)for(const auto&[id,t]:options->images->textures)std::cout<<"T\t"<<id<<'\t'<<unsigned(t->game_format)<<'\t'<<t->width<<'\t'<<t->height<<'\t'<<unsigned(t->levels)<<'\t'<<t->pixels.size()<<'\t'<<Hash(t->pixels)<<'\n';
    first.reset();FrontendSession nav;nav.BeginShared(Request("/Art/fe/fe_overlay.fen"),token);Pump(nav);InspectShared(nav,options,token);
    for(unsigned i=0;i<120;++i)nav.Advance(1.f/60);
    Check(nav.Current()->images==options->images&&options->graph.presentation_time==0,"Owned simultaneous resources/timelines diverged");
    // Main is another actual independent FEN in this same original profile.
    FrontendSession main;auto main_request=Request("/Art/fe/main_menu_v3.fen");main_request.initial_slide="MAIN";main.BeginShared(main_request,token);Pump(main);InspectShared(main,options,token);
    std::cout<<"Owned shared MainUI: 398 textures, Options + NAV + Main, one permanent read\n";
    return nav.Current();
}
struct Host {bool live=false,disc=false;~Host(){if(live)ResetStartupFiles();if(disc)aurora_dvd_close();if(live){ResetStartupMemory();aurora_shutdown();}}};
}
int main(int argc,char** argv)
{
    try
    {
        Check(argc==4,"Supply disc, output directory and mode");const std::string mode=argv[3];
        if(mode=="success")Decoder();
        const auto folder=(std::filesystem::path(argv[2])/"frontend-shared-data").string();std::filesystem::create_directories(folder);
        AuroraConfig config{};config.appName="Charged shared frontend resources";config.userPath=config.cachePath=folder.c_str();
        config.resourcesPath=SDL_GetBasePath();config.desiredBackend=BACKEND_NULL;config.windowWidth=320;config.windowHeight=240;
        config.windowPosX=config.windowPosY=-1;config.logLevel=LOG_WARNING;config.mem1Size=MEM1_DEFAULT_SIZE;config.mem2Size=64*1024*1024;
        Host host;const auto state=aurora_initialize(argc,argv,&config);host.live=true;Check(state.window,"Aurora initialization failed");
        InitializeStartupOS();nlInitMemory();Check(aurora_dvd_open(argv[1]),"Cannot open shared frontend disc");host.disc=true;nlInitFileSystem();
        FrontendSession::Handle retained;
        for(unsigned repeat=0;repeat<3;++repeat)
        {
            const auto a=StandardAllocator.TotalFreeMemory(),b=VirtualAllocator.TotalFreeMemory();
            if(mode=="success")retained=Generated();else if(mode=="owned")retained=Owned(!repeat);else
            {
                FrontendSession owner;owner.Begin(Request("/Art/fe/text.fen"));Pump(owner);auto old=owner.Result();
                try{owner.Begin(Request(),FrontendSessionResourcesMode::PermanentMain);Pump(owner);}catch(const std::exception&){}
                Check(owner.State()==FrontendSessionState::Failed&&owner.Current()==old,"Malformed permanent profile published partially");Reject([&]{owner.Result();});Reject([&]{owner.SharedResources();});
            }
            Check(!nlAsyncReadsPending(nullptr),"Shared frontend leaked NL reads");
            Check(a==StandardAllocator.TotalFreeMemory()&&b==VirtualAllocator.TotalFreeMemory(),"Shared frontend did not recover both arenas");
        }
        nlShutdownFileSystem();ResetStartupMemory();
        if(retained)Check(retained->visuals->text&&!retained->images->textures.empty(),"Retained resources did not survive arena shutdown");
        std::cout<<checks<<" shared frontend checks passed\n";
    }
    catch(const std::exception& error){std::cerr<<"FAILED: "<<error.what()<<" (check "<<checks<<")\n";return 1;}
}
