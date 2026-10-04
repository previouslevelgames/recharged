#include "runtime/frontend_font_load.h"
#include "runtime/startup.h"
#include "runtime/startup_files.h"
#include "NL/nlFileGC.h"
#include "NL/nlMemory.h"
#include "NL/MemAlloc.h"
#include <aurora/aurora.h>
#include <aurora/dvd.h>
#include <SDL3/SDL.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <source_location>
#include <thread>
using namespace mscharged;
namespace {
unsigned checks=0;
void Check(bool v,const char* why){++checks;if(!v)throw std::runtime_error(why);}
template<class F>void Reject(F f,std::source_location where=std::source_location::current())
{++checks;try{f();}catch(const std::exception&){return;}throw std::runtime_error("Invalid font operation accepted at "+std::to_string(where.line()));}
std::array<FrontendFontRequest,2> Requests()
{return {{{"/Art/fe/fonts/eurfonttext18.res","fe/fonts/eurfonttext18","fot-rodinprob18"},{"/Art/fe/fonts/eurfontheading36.res","fe/fonts/eurfontheading36","Scratchy36"}}};}
void Pump(FrontendFontLoad& s,bool external=false,bool errors=false)
{
    const auto end=std::chrono::steady_clock::now()+std::chrono::seconds(10);
    while(s.State()==FrontendFontLoadState::Loading)
    {
        try{if(external){nlServiceFileSystem();s.Poll();}else s.Service();}catch(...){if(!errors)throw;s.Poll();}
        auto p=s.Progress();Check(p.completed_reads<=p.requested_reads&&p.completed_pages<=p.requested_pages&&p.completed_fonts<=p.requested_fonts,"Font counters diverged");
        Check(std::chrono::steady_clock::now()<end,"Font load timed out");SDL_Delay(1);
    }
}
struct Overlay
{
    enum Mode { Good, Error, Short, Blocked } mode=Good;
    std::vector<std::uint8_t> bytes;
    std::uint32_t selected=0;
    std::atomic<unsigned> handles{0};std::atomic<bool> entered{false},finished{false};
    std::mutex mutex;std::condition_variable gate;bool released=false;
    struct Handle{Overlay* owner;std::size_t at=0;};
    static void* Open(void* p){auto* o=static_cast<Overlay*>(p);++o->handles;return new Handle{o};}
    static void Close(void* p){auto* h=static_cast<Handle*>(p);--h->owner->handles;delete h;}
    static std::int64_t Seek(void* p,std::int64_t offset,std::int32_t origin)
    {auto& h=*static_cast<Handle*>(p);if(origin||offset<0||std::size_t(offset)>h.owner->bytes.size())return -1;return h.at=offset;}
    static std::int64_t Read(void* p,std::uint8_t* out,std::size_t size)
    {
        auto& h=*static_cast<Handle*>(p);auto& o=*h.owner;
        if(h.at==o.selected&&o.mode!=Good)
        {
            o.entered=true;
            if(o.mode==Error){o.finished=true;return -1;}
            if(o.mode==Short){o.finished=true;return 0;}
            std::unique_lock lock(o.mutex);if(!o.gate.wait_for(lock,std::chrono::seconds(3),[&]{return o.released;})){o.finished=true;return -1;}
        }
        const auto count=std::min(size,o.bytes.size()-h.at);std::memcpy(out,o.bytes.data()+h.at,count);h.at+=count;o.finished=true;return count;
    }
    explicit Overlay(Mode m,std::uint32_t offset):mode(m),selected(offset)
    {
        unsigned long size=0;std::unique_ptr<void,void(*)(void*)> input(nlLoadEntireFile(Requests()[0].path.c_str(),&size,32,AllocateEnd,nullptr,0,&VirtualAllocator),nlFree);
        bytes.assign(static_cast<std::uint8_t*>(input.get()),static_cast<std::uint8_t*>(input.get())+size);
        const AuroraOverlayCallbacks callbacks{Open,Close,Read,Seek};aurora_dvd_overlay_callbacks(&callbacks);
        const auto path=Requests()[0].path;const AuroraOverlayFile file{path.c_str(),this,size};aurora_dvd_overlay_files(&file,1,nullptr);
    }
    void Release(){std::lock_guard lock(mutex);released=true;gate.notify_all();}
    ~Overlay(){Release();aurora_dvd_overlay_files(nullptr,0,nullptr);if(handles)std::terminate();}
};
void Lifecycle()
{
    auto requests=Requests();Reject([&]{FrontendFontLoad f({});});
    std::vector<FrontendFontRequest> excessive(17,requests[0]);Reject([&]{FrontendFontLoad f(excessive);});
    requests[1].alias="FOT-RODINPROB18";Reject([&]{FrontendFontLoad f(requests);});requests=Requests();
    for(unsigned stages:{0u,2u,4u,6u})
    {
        FrontendFontLoad f(requests);
        const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(3);
        while(f.Progress().completed_reads<stages){f.Service();Check(std::chrono::steady_clock::now()<deadline,"Font cancellation stage timed out");SDL_Delay(1);}
        bool wrong=false;std::thread t([&]{try{f.Poll();}catch(const std::logic_error&){wrong=true;}});t.join();Check(wrong,"Font owner accepted foreign thread");
        f.Cancel();f.Cancel();Check(f.State()==FrontendFontLoadState::Cancelled&&!nlAsyncReadsPending(nullptr),"Font cancel retained requests");Reject([&]{f.Result();});
    }
    // Raw page arrival alone must not publish. Drive earlier stages explicitly,
    // then withhold Poll once every final page callback has actually completed.
    {
        FrontendFontLoad f(requests);const auto end=std::chrono::steady_clock::now()+std::chrono::seconds(3);
        while(f.Progress().requested_pages<4){nlServiceFileSystem();f.Poll();Check(std::chrono::steady_clock::now()<end,"Page submission timed out");SDL_Delay(1);}
        while(f.Progress().completed_pages<4){nlServiceFileSystem();Check(std::chrono::steady_clock::now()<end,"Raw final-page completion timed out");SDL_Delay(1);}
        Check(f.State()==FrontendFontLoadState::Loading,"Raw page callbacks published fonts without assembly");Reject([&]{f.Result();});
        f.Cancel();Check(f.State()==FrontendFontLoadState::Cancelled&&!nlAsyncReadsPending(nullptr),"Final-page cancellation published or retained work");Reject([&]{f.Result();});
    }
    // 16 independent slots using genuine reads; source aliases remain distinct.
    std::vector<FrontendFontRequest> many(16,requests[0]);for(unsigned i=0;i<many.size();++i)many[i].alias="font"+std::to_string(i);
    {FrontendFontLoad f(many);Pump(f);Check(f.Result().size()==16&&f.Progress().completed_mask==0xffff,"Original16 slots did not complete");}
    for(unsigned stage:{0u,32u,96u})for(auto mode:{Overlay::Error,Overlay::Short,Overlay::Blocked})
    {
        Overlay o(mode,stage);auto f=std::make_unique<FrontendFontLoad>(requests);
        if(mode!=Overlay::Blocked){Pump(*f,mode==Overlay::Short,true);Reject([&]{f->Result();});Check(f->State()==FrontendFontLoadState::Failed,"Failed range became ready");}
        else
        {
            const auto end=std::chrono::steady_clock::now()+std::chrono::seconds(2);
            while(!o.entered){f->Service();Check(std::chrono::steady_clock::now()<end,"Blocked range was not entered");SDL_Delay(1);}
            std::jthread release([&]{SDL_Delay(10);o.Release();});f.reset();
        }
        Check(o.entered&&o.finished&&!o.handles&&!nlAsyncReadsPending(nullptr),"Font failure/cancel did not drain worker and file");
    }
    {
        FrontendFontLoad f(requests);nlShutdownFileSystem();f.Poll();Check(f.State()==FrontendFontLoadState::Failed,"Font missed file-service shutdown");Reject([&]{f.Result();});
    }
    nlInitFileSystem();
    // Fewer available slots than initial headers: partial submission must drain.
    {std::unique_ptr<nlFile> file(nlOpen(requests[0].path.c_str()));alignas(32)std::array<std::array<std::uint8_t,32>,63> buffers{};
     for(auto& b:buffers){nlSeek(file.get(),0,0);nlReadAsync(file.get(),b.data(),32,nullptr,0,32);}
     Reject([&]{FrontendFontLoad f(requests);});nlCancelPendingAsyncReads(file.get(),nullptr);Check(!nlAsyncReadsPending(nullptr),"Failed constructor retained font I/O");}
    struct Callback
    {
        FrontendFontLoad* owner;bool ran=false,fail=false;
        static void Call(void* data,unsigned long,void* context)
        {std::unique_ptr<void,void(*)(void*)> bytes(data,nlFree);auto& c=*static_cast<Callback*>(context);c.ran=true;
         Reject([&]{c.owner->Poll();});Reject([&]{c.owner->Service();});Reject([&]{c.owner->Cancel();});Reject([&]{FrontendFontLoad f(Requests());});
         if(c.fail)throw std::runtime_error("Independent shared-pump failure");}
    };
    for(bool fail:{false,true})
    {FrontendFontLoad f(requests);Callback callback{&f,false,fail};nlLoadEntireFileAsync(requests[0].path.c_str(),Callback::Call,&callback,32,AllocateEnd,nullptr,0,&VirtualAllocator);
     Pump(f,false,fail);Check(callback.ran,"Shared callback did not run");
     if(fail){Check(f.State()==FrontendFontLoadState::Failed&&!nlAsyncReadsPending(nullptr),"Shared exception published or retained font work");Reject([&]{f.Result();});}
     else Check(f.State()==FrontendFontLoadState::Ready,"Callback rejection corrupted font load");}
    struct Blocks{std::vector<void*> values;~Blocks(){for(auto p:values)VirtualAllocator.Free(p);}}blocks;
    for(unsigned size=1024*1024;size>=32;size/=2)for(;;){try{blocks.values.push_back(VirtualAllocator.Allocate(size,32,false));}catch(const std::bad_alloc&){break;}}
    Reject([&]{FrontendFontLoad f(requests);});Check(!nlAsyncReadsPending(nullptr),"Arena allocation failure retained requests");
}
struct Host{bool live=false,disc=false;~Host(){if(live)ResetStartupFiles();if(disc)aurora_dvd_close();if(live){ResetStartupMemory();aurora_shutdown();}}};
}
int main(int argc,char**argv)
{
    try
    {
        Check(argc==4,"Supply disc/output/mode");const std::string mode=argv[3];auto folder=(std::filesystem::path(argv[2])/"font-load-data").string();std::filesystem::create_directories(folder);
        AuroraConfig config{};config.appName="Charged staged font loading";config.userPath=config.cachePath=folder.c_str();config.resourcesPath=SDL_GetBasePath();config.desiredBackend=BACKEND_NULL;
#ifdef MSCHARGED_TEST_WORLD_GX
        config.desiredBackend=BACKEND_VULKAN;
#endif
        config.windowWidth=320;config.windowHeight=240;config.windowPosX=config.windowPosY=-1;config.logLevel=LOG_WARNING;config.mem1Size=MEM1_DEFAULT_SIZE;config.mem2Size=64*1024*1024;
        Host host;auto initialized=aurora_initialize(argc,argv,&config);host.live=true;Check(initialized.window,"Aurora window failed");InitializeStartupOS();nlInitMemory();Check(aurora_dvd_open(argv[1]),"Disc failed");host.disc=true;nlInitFileSystem();
        FrontendFontLoad::ResultType retained;
        for(unsigned repeat=0;repeat<3;++repeat)
        {
            auto a=StandardAllocator.TotalFreeMemory(),b=VirtualAllocator.TotalFreeMemory();
            if(mode=="empty"||mode=="missing")Reject([&]{FrontendFontLoad f(Requests());});
            else
            {
                FrontendFontLoad f(Requests());Reject([&]{f.Result();});Pump(f,repeat%2);
                if(mode=="success"||mode=="owned")
                {
                    retained=f.Result();const auto progress=f.Progress();unsigned pages=0;for(auto& font:retained)pages+=font->pages.size();
                    Check(progress.completed_fonts==2&&progress.completed_mask==3&&progress.requested_pages==pages&&progress.completed_pages==pages
                        &&progress.requested_reads==6+pages&&progress.completed_reads==6+pages,"Source font stage counters differ");
                    Check(retained[0]->alias==resources::FrontendNameHash("fot-rodinprob18")&&retained[1]->alias==resources::FrontendNameHash("scratchy36"),"Source font aliases differ");
                    if(mode=="owned")Check(retained[0]->glyphs.size()==267&&retained[1]->glyphs.size()==117,"Owned font glyph counts differ");
                }
                else{Reject([&]{f.Result();});Check(f.State()==FrontendFontLoadState::Failed,"Malformed bundle became ready");}
            }
            if(mode=="success")Lifecycle();
            Check(!nlAsyncReadsPending(nullptr)&&StandardAllocator.TotalFreeMemory()==a&&VirtualAllocator.TotalFreeMemory()==b,"Font batch failed both-arena recovery");
        }
        nlShutdownFileSystem();ResetStartupMemory();
        if(!retained.empty())Check(retained[0]->Glyph('A').width>0&&retained[1]->pages.size()>0,"Retained fonts died with native session");
        std::cout<<checks<<" staged font ownership checks passed: "<<mode<<"\n";
    }catch(const std::exception&e){std::cerr<<"FAILED: "<<e.what()<<" (check "<<checks<<")\n";return 1;}
}
