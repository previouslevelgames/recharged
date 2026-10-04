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
using namespace mscharged;
using namespace mscharged::resources;
namespace {
unsigned checks=0;
void Check(bool value,const char* why){++checks;if(!value)throw std::runtime_error(why);}
template<class F>void Reject(F f,std::source_location at=std::source_location::current())
{++checks;try{f();}catch(const std::exception&){return;}throw std::runtime_error("Invalid session operation accepted at line "+std::to_string(at.line()));}
FrontendSessionRequest Request(bool animate=true)
{FrontendSessionRequest r;r.path=animate?"/Art/fe/session.fen":"/Art/fe/text.fen";r.animate=animate;return r;}
void Pump(FrontendSession& s,bool external=false,bool errors=false)
{
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(10);
    while(s.State()==FrontendSessionState::Loading)
    {
        try{if(external){nlServiceFileSystem();s.Poll();}else s.Service();}
        catch(...){if(!errors)throw;s.Poll();}
        Check(std::chrono::steady_clock::now()<deadline,"Frontend session read timed out");SDL_Delay(1);
    }
}
void Inspect(FrontendSession::Handle frame,bool animated)
{
    Check(frame&&frame->visuals->localization&&frame->visuals->text&&frame->visuals->heading,"Incomplete retained resources");
    Check(frame->request.animate==animated&&frame->graph.active_slide==0x30,"Initial retained graph differs");
    Check(frame->layout.entries.size()==1&&(animated?frame->layout.ImageCount():frame->layout.TextCount())==1,"Retained layout differs");
    Check(frame->image_completed_files==(animated?1u:0u),"Wrong image read count");
}
struct FaultFile
{
    enum Mode { Error, Short, Blocked } mode;
    std::atomic<bool> entered{false}, finished{false};
    std::atomic<unsigned> handles{0};
    std::mutex mutex; std::condition_variable gate; bool released = false;
    struct Handle { FaultFile* owner; std::int64_t position = 0; };
    static void* Open(void* context) { auto* f = static_cast<FaultFile*>(context); ++f->handles; return new Handle{f}; }
    static void Close(void* context) { auto* h = static_cast<Handle*>(context); --h->owner->handles; delete h; }
    static std::int64_t Seek(void* context, std::int64_t offset, std::int32_t origin)
    { if (origin || offset < 0 || offset > 128) return -1; return static_cast<Handle*>(context)->position = offset; }
    static std::int64_t Read(void* context, std::uint8_t* data, std::size_t size)
    {
        auto& h = *static_cast<Handle*>(context); auto& f = *h.owner; f.entered = true;
        if (f.mode == Error) { f.finished = true; return -1; }
        if (f.mode == Blocked)
        {
            std::unique_lock lock(f.mutex);
            if (!f.gate.wait_for(lock, std::chrono::seconds(2), [&] { return f.released; }))
            { f.finished = true; return -1; }
        }
        const auto count = std::min<std::size_t>(size, f.mode == Short ? (h.position ? 0 : 3) : 128 - h.position);
        std::fill_n(data, count, 0); h.position += count; f.finished = true; return count;
    }
    explicit FaultFile(Mode m, const char* path = "/art/fe/session.fen", std::size_t size = 128) : mode(m)
    {
        const AuroraOverlayCallbacks callbacks{Open, Close, Read, Seek}; aurora_dvd_overlay_callbacks(&callbacks);
        const AuroraOverlayFile file{path, this, size}; aurora_dvd_overlay_files(&file, 1, nullptr);
    }
    ~FaultFile()
    {
        { std::lock_guard lock(mutex); released = true; } gate.notify_all();
        aurora_dvd_overlay_files(nullptr, 0, nullptr);
        if (handles) std::terminate();
    }
    void Wait()
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
        while (!entered) { Check(std::chrono::steady_clock::now() < deadline, "Fault worker did not start"); SDL_Delay(1); }
    }
};
void Transactions()
{
    FrontendSession owner;
    Check(owner.State()==FrontendSessionState::Idle&&!owner.Current(),"New scene session is not empty");
    Reject([&]{owner.Result();});Reject([&]{owner.Advance(0);});
    auto request=Request(false);owner.Begin(request);request.path="invalidated";Pump(owner);
    auto text=owner.Result();Inspect(text,false);
    Check(owner.Progress().fen_completed&&owner.Progress().visual_completed_mask==7,"Real FEN/font reads did not complete");
    Reject([&]{owner.Advance(0);});Reject([&]{owner.SelectPresentation("Slide");});
    FrontendInstanceChange text_edit;text_edit.instance=0x80;text_edit.property=FrontendInstanceProperty::String;text_edit.text=u"BA";
    owner.Apply(text,{&text_edit,1});auto edited=owner.Current();
    Check(edited!=text&&edited->graph.instances[0].text==u"BA"&&edited->layout.TextCount()==1,"Static user string did not publish retained layout");
    Check(text->graph.instances[0].text.empty(),"Setter mutated retained old snapshot");
    Reject([&]{owner.Apply(text,{&text_edit,1});});
    FrontendInstanceChange position;position.instance=0x80;position.property=FrontendInstanceProperty::Position;position.vector={17,29,0};
    FrontendInstanceChange missing;missing.instance=0x80;missing.property=FrontendInstanceProperty::StringId;missing.string_id="LOC_missing";
    const std::array bad_edits{position,missing};Reject([&]{owner.Apply(edited,bad_edits);});
    Check(owner.Current()==edited&&owner.Current()->graph.instances[0].attributes.position==edited->graph.instances[0].attributes.position,
        "Layout failure published partial static mutations");
    owner.Begin(Request(false));Pump(owner);text=owner.Result();Inspect(text,false);

    owner.Begin(Request());Check(owner.Current()==text,"Loading replacement removed current scene");
    Reject([&]{owner.Result();});owner.Cancel();owner.Cancel();
    Check(owner.State()==FrontendSessionState::Cancelled&&owner.Current()==text&&!nlAsyncReadsPending(nullptr),"Cancel did not preserve current or drain work");
    owner.Begin(Request());owner.Begin(Request());Pump(owner,true);auto initial=owner.Result();Inspect(initial,true);Inspect(text,false);
    owner.Advance(.25f);auto advanced=owner.Current();Check(advanced!=initial&&advanced->graph.presentation_time==.25f,"Advance failed to publish a new snapshot");
    Check(initial->graph.presentation_time==0,"Advance mutated retained snapshot");
    Check(owner.SelectPresentation("SLIDE"),"Original case-insensitive slide lookup failed");
    Check(owner.Current()->graph.presentation_time==.25f,"Same-slide selection reset presentation clock");
    const auto old_position=owner.Current()->graph.instances[0].attributes.position;
    Check(owner.SelectPresentation("Slide",true)&&owner.Current()->graph.presentation_time==0,"Forced presentation reset failed");
    Check(owner.Current()->graph.instances[0].attributes.position==old_position,"Presentation selection incorrectly sampled Update(0)");
    owner.Advance(0);Check(owner.Current()->graph.instances[0].attributes.position[0]==-160,"Explicit Update(0) did not sample selected slide");
    Check(owner.SelectPresentation("Blank")&&owner.Current()->layout.entries.empty(),"Empty authored slide failed");
    Check(!owner.SelectPresentation("missing")&&!owner.Current()->graph.active_slide&&owner.Current()->layout.entries.empty(),"Missing live slide did not clear active");
    owner.Advance(1);Check(owner.Current()->graph.presentation_time==0,"Inactive presentation advanced its clock");
    owner.Reset();Inspect(owner.Current(),true);
    auto original=owner.Current();FrontendInstanceChange tint;tint.instance=0x80;tint.property=FrontendInstanceProperty::Colour;tint.colour={32,64,96,128};
    owner.Apply(original,{&tint,1});auto tinted=owner.Current();
    Check(tinted->graph.instances[0].attributes.colour==tint.colour&&original->graph.instances[0].attributes.colour[0]==255,"Animated mutation did not isolate snapshots");
    owner.Advance(.125f);Check(owner.Current()->graph.instances[0].attributes.colour==tint.colour,"Unrelated animation discarded setter state");
    const auto keep=owner.Current();Reject([&]{owner.Apply(original,{&tint,1});});Reject([&]{owner.SetupLoadingScene(keep,true);});
    Check(owner.Current()==keep,"Failed loading setup replaced current frame");owner.Reset();

    bool wrong_thread=false;std::thread thread([&]{try{owner.Pop();}catch(const std::logic_error&){wrong_thread=true;}});thread.join();Check(wrong_thread,"Wrong-thread session mutation accepted");
    for(const char* path:{"/Art/fe/bad.fen","/Art/fe/missing-image.fen","/Art/fe/absent.fen"})
    {
        auto previous=owner.Current();request=Request();request.path=path;
        try{owner.Begin(request);Pump(owner);}catch(const std::exception&){}
        Check(owner.State()==FrontendSessionState::Failed&&owner.Current()==previous,"Failed replacement published partial scene");Reject([&]{owner.Result();});
        owner.Advance(.01f);Check(owner.Current()->graph.presentation_time>previous->graph.presentation_time,"Previous scene stopped after failed replacement");
    }
    for(const char* name:{"absent","Duplicate"})
    {
        auto previous=owner.Current();request=Request();request.initial_slide=name;owner.Begin(request);Pump(owner);
        Check(owner.State()==FrontendSessionState::Failed&&owner.Current()==previous,"Invalid initial selection did not retain current");Reject([&]{owner.Result();});
    }
    auto previous=owner.Current();request=Request();request.path="Art/fe/session.fen";Reject([&]{owner.Begin(request);});
    Check(owner.Current()==previous,"Relative path rejection discarded current");
    request=Request();request.path="/Art/fe/colour.fen";owner.Begin(request);Pump(owner);auto colour=owner.Result();
    Reject([&]{owner.Advance(.5f);});Check(owner.Current()==colour&&owner.Current()->graph.presentation_time==0,"Unsafe colour evaluation partially published");
    request=Request();request.image_profile=FrontendImageProfile::InGame;owner.Begin(request);Pump(owner);
    Check(owner.Result()->image_completed_files==2,"InGame session omitted an original image bundle");
    auto retained=owner.Current();owner.Pop();owner.Pop();Check(!owner.Current()&&owner.State()==FrontendSessionState::Idle,"Pop did not clear one current scene");
    Check(retained->layout.ImageCount()==1&&retained->images->textures.size()==1,"Retained frame did not survive pop");
    owner.Begin(Request());
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
    while(!owner.Progress().fen_completed||owner.Progress().visual_completed_mask!=7)
    {nlServiceFileSystem();Check(std::chrono::steady_clock::now()<deadline,"Raw staged completion timed out");SDL_Delay(1);}
    owner.Cancel();Check(!owner.Current()&&owner.State()==FrontendSessionState::Cancelled,"Cancellation published staged FEN/font data");
    owner.Begin(Request());nlShutdownFileSystem();owner.Poll();Check(owner.State()==FrontendSessionState::Failed&&!owner.Current(),"File shutdown left pending scene");nlInitFileSystem();
}
void Failures()
{
    auto owner=std::make_unique<FrontendSession>();owner->Begin(Request(false));Pump(*owner);auto previous=owner->Result();
    for(auto mode:{FaultFile::Error,FaultFile::Short})
    {
        FaultFile fault(mode);owner->Begin(Request());Pump(*owner,false,true);
        Check(owner->State()==FrontendSessionState::Failed&&owner->Current()==previous&&fault.finished&&!fault.handles,"Short/error FEN read leaked or changed current");Reject([&]{owner->Result();});
    }
    {FaultFile fault(FaultFile::Error,"/art/fe/session.fen",MaximumAssetBytes+1);Reject([&]{owner->Begin(Request());});Check(!fault.entered&&owner->Current()==previous,"Oversized FEN submitted work");}
    for(unsigned operation=0;operation<3;++operation)
    {
        FaultFile fault(FaultFile::Blocked);owner->Begin(Request());fault.Wait();
        std::jthread release([&]{SDL_Delay(15);{std::lock_guard lock(fault.mutex);fault.released=true;}fault.gate.notify_all();});
        if(operation==0)owner->Cancel();else if(operation==1)owner->Begin(Request(false));else owner.reset();
        Check(fault.finished&&!fault.handles,"Owner cancellation/replacement/destruction failed to join active I/O");
        if(owner){Check(owner->Current()==previous,"Cancellation replaced current");if(operation==1){Pump(*owner);previous=owner->Result();}}
    }
    owner=std::make_unique<FrontendSession>();owner->Begin(Request(false));Pump(*owner);previous=owner->Result();
    {
        std::unique_ptr<nlFile> file(nlOpen("/Art/fe/session.fen"));alignas(32) std::array<std::array<std::uint8_t,32>,62> buffers{};
        for(auto& bytes:buffers){nlSeek(file.get(),0,0);nlReadAsync(file.get(),bytes.data(),32,nullptr,0,32);}
        Reject([&]{owner->Begin(Request());});Check(owner->Current()==previous,"Partial submission replaced current");
        nlCancelPendingAsyncReads(file.get(),nullptr);Check(!nlAsyncReadsPending(nullptr),"Partial submission retained requests");
    }
    {
        struct Blocks{std::vector<void*> values;~Blocks(){for(auto p:values)VirtualAllocator.Free(p);}}blocks;
        for(;;){try{blocks.values.push_back(VirtualAllocator.Allocate(256*1024,32,false));}catch(const std::bad_alloc&){break;}}
        const auto remaining=VirtualAllocator.LargestFreeBlock();if(remaining>256)blocks.values.push_back(VirtualAllocator.Allocate(remaining-128,32,false));
        Reject([&]{owner->Begin(Request());});Check(owner->Current()==previous&&!nlAsyncReadsPending(nullptr),"Allocation failure leaked pending scene");
    }
    struct Callback
    {
        FrontendSession* owner;bool ran=false,fail=false;
        static void Run(void* data,unsigned long,void* context)
        {
            std::unique_ptr<void,void(*)(void*)> buffer(data,nlFree);auto& c=*static_cast<Callback*>(context);c.ran=true;
            Reject([&]{c.owner->Begin(Request());});Reject([&]{c.owner->Poll();});Reject([&]{c.owner->Cancel();});
            Reject([&]{c.owner->Pop();});Reject([&]{c.owner->Advance(0);});Reject([&]{c.owner->Service();});
            if(c.fail)throw std::runtime_error("Independent shared-pump callback failure");
        }
    } callback{owner.get()};
    for(bool fail:{false,true})
    {
        callback.ran=false;callback.fail=fail;previous=owner->Current();
        nlLoadEntireFileAsync("/Art/fe/session.fen",Callback::Run,&callback,32,AllocateEnd,nullptr,0,&VirtualAllocator);
        owner->Begin(Request());Pump(*owner,false,fail);Check(callback.ran,"Actual shared-pump callback not exercised");
        if(fail){Check(owner->State()==FrontendSessionState::Failed&&owner->Current()==previous,"Shared-pump failure published replacement");Reject([&]{owner->Result();});}
        else Check(owner->State()==FrontendSessionState::Ready,"Protected shared callback blocked valid completion");
    }
}
struct Host
{
    bool live=false,disc=false;
    ~Host(){if(live)ResetStartupFiles();if(disc)aurora_dvd_close();if(live){ResetStartupMemory();aurora_shutdown();}}
};
}
int main(int argc,char** argv)
{
    try
    {
        Check(argc==4,"Supply disc, output directory and mode");const std::string mode=argv[3];
        const auto folder=(std::filesystem::path(argv[2])/"session-runtime-data").string();std::filesystem::create_directories(folder);
        AuroraConfig config{};config.appName="Charged frontend scene sessions";config.userPath=config.cachePath=folder.c_str();
        config.resourcesPath=SDL_GetBasePath();config.desiredBackend=BACKEND_NULL;config.windowWidth=320;config.windowHeight=240;
        config.windowPosX=config.windowPosY=-1;config.logLevel=LOG_WARNING;config.mem1Size=MEM1_DEFAULT_SIZE;config.mem2Size=64*1024*1024;
        Host host;const auto state=aurora_initialize(argc,argv,&config);host.live=true;Check(state.window,"Aurora initialization failed");
        InitializeStartupOS();nlInitMemory();Check(aurora_dvd_open(argv[1]),"Cannot open scene-session disc");host.disc=true;nlInitFileSystem();
        FrontendSession::Handle retained;
        for(unsigned repeat=0;repeat<3;++repeat)
        {
            const auto a=StandardAllocator.TotalFreeMemory(),b=VirtualAllocator.TotalFreeMemory();
            {
                FrontendSession owner;
                if(mode=="success")
                {
                    Transactions();Failures();owner.Begin(Request());Pump(owner);retained=owner.Result();
                }
                else if(mode=="owned")
                {
                    FrontendSessionRequest request;request.path="/Art/fe/game_summary.fen";request.initial_slide="Slide1";
                    request.image_profile=FrontendImageProfile::InGame;owner.Begin(request);Pump(owner);retained=owner.Result();
                    Check(retained->layout.TextCount()==8&&retained->layout.ImageCount()==11,"Owned initial authored frame differs");
                    Check(retained->images->textures.size()==20&&retained->image_completed_files==2,"Owned resource graph differs");
                    for(unsigned i=0;i<120;++i)owner.Advance(1.f/60);
                    Check(owner.Current()!=retained&&owner.Current()->graph.presentation_time!=0,"Owned timeline did not advance");
                    owner.Begin(request);owner.Cancel();Check(owner.Current()->graph.presentation_time!=0,"Owned cancelled replacement lost current");
                    request.language=FrontendLanguage::NAFrench;owner.Begin(request);Pump(owner);auto french=owner.Result();
                    Check(french->visuals->localization->language==0x30d469c4&&retained->visuals->localization->language==0x7a947b29,"Owned localized replacement mutated retained frame");
                    owner.Pop();Check(!owner.Current()&&french->layout.TextCount()>0,"Owned pop invalidated retained scene");
                    std::cout<<"Owned scene replacement/pop: "<<retained->layout.TextCount()<<" text, "<<retained->layout.ImageCount()<<" image, "<<retained->images->textures.size()<<" textures\n";
                }
                else
                {
                    try{owner.Begin(Request());Pump(owner);}catch(const std::exception&){}
                    Check(owner.State()==FrontendSessionState::Failed&&!owner.Current(),"Malformed/missing dependency became ready");Reject([&]{owner.Result();});
                }
                owner.Pop();
            }
            Check(!nlAsyncReadsPending(nullptr),"Session retained native reads");
            Check(StandardAllocator.TotalFreeMemory()==a&&VirtualAllocator.TotalFreeMemory()==b,"Scene session failed both-arena recovery");
        }
        nlShutdownFileSystem();ResetStartupMemory();
        if(retained)Check(retained->visuals->text&&!retained->layout.entries.empty()&&!retained->images->textures.empty(),"Retained scene did not survive NL/arena shutdown");
        std::cout<<checks<<" frontend scene-session checks passed\n";
    }
    catch(const std::exception& e){std::cerr<<"FAILED: "<<e.what()<<" (check "<<checks<<")\n";return 1;}
}
