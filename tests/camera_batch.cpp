#include "dvd_fixture_medium.h"
#include "runtime/camera_batch.h"
#include "runtime/graphics_memory.h"
#include "runtime/startup.h"
#include "runtime/startup_files.h"
#include "NL/nlFile.h"
#include "NL/nlFileGC.h"
#include <aurora/aurora.h>
#include <aurora/dvd.h>
#include <SDL3/SDL.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <new>
#include <thread>

// Fail only this test thread's host allocations during the publication sweep.
// Real native-arena exhaustion is qualified separately below.
thread_local long allocation_budget=-1;
void* operator new(std::size_t size)
{
    if (allocation_budget==0) throw std::bad_alloc();
    if (allocation_budget>0) --allocation_budget;
    if (void* p=std::malloc(size?size:1)) return p;
    throw std::bad_alloc();
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p,std::size_t) noexcept { std::free(p); }
void operator delete[](void* p,std::size_t) noexcept { std::free(p); }

using namespace mscharged;
namespace
{
unsigned checks=0;
void Check(bool value,const char* message) { ++checks;if(!value)throw std::runtime_error(message); }
template<class E=std::exception,class F> void Reject(F action)
{ ++checks;try { action(); }catch(const E&){return;}throw std::runtime_error("Invalid batch operation succeeded"); }
using Requests=std::vector<CameraBatchRequest>;
Requests Names(unsigned n)
{
    Requests requests;
    for(unsigned i=0;i<n;++i)requests.push_back({"camera"+std::to_string(i),"/camera.cam"});
    return requests;
}
void Accounting(const CameraAssetBatch& batch)
{
    const auto p=batch.Progress();
    Check(p.completed==p.succeeded+p.failed&&p.requested==p.pending+p.completed+p.cancelled,
        "Batch terminal accounting diverged");
    Check(p.active<=MaximumActiveCameraBatchRequests&&p.active<=p.pending,"Batch exceeded active request bound");
}
void Pump(CameraAssetBatch& batch,bool external=false,bool allow_read_error=false)
{
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
    while(batch.State()==CameraBatchState::Loading)
    {
        try { if(external){nlServiceFileSystem();batch.Poll();}else batch.Service(); }
        catch(const std::runtime_error&){if(!allow_read_error)throw;batch.Poll();}
        Accounting(batch);
        Check(std::chrono::steady_clock::now()<deadline,"Camera batch read timed out");SDL_Delay(1);
    }
}
void Success(bool external)
{
    CameraAssetLibrary library;
    auto original=LoadCameraAsset("/camera.cam","retained");library.Insert(original);
    const auto requests=Names(MaximumCameraBatchRequests);
    {
        CameraAssetBatch batch(library,requests);Accounting(batch);
        Check(batch.Progress().active==8&&batch.Progress().pending==64,"Initial bounded queue differs");
        Reject<std::logic_error>([&]{batch.Publish();});
        Check(!library.Find("camera0")&&library.Find("retained")==original,"Pending transaction published data");
        Pump(batch,external);
        const auto p=batch.Progress();
        Check(batch.State()==CameraBatchState::Ready&&p.completed==64&&p.succeeded==64&&!p.failed&&!p.cancelled&&!p.pending&&!p.active,
            "Successful batch accounting differs");
        Check(!library.Find("camera0"),"Complete batch published without explicit request");
        batch.Publish();batch.Publish();batch.Service();batch.Poll();batch.Cancel();Accounting(batch);
        Check(batch.State()==CameraBatchState::Published,"Published batch was cancelled or republished");
        for(const auto& r:requests)
        {
            const auto value=library.Find(r.alias);
            Check(value&&value->Data().cameraPos[2].x==2&&value->Data().fFOV[1]==41,"Batch lost named native data");
        }
        Check(library.Find("CAMERA0")==library.Find("camera0"),"Batch alias canonicalization differs");
    }
    auto retained=library.Find("camera0");library.Erase("camera0");library.Clear();
    Check(retained->Data().m_uKeyCount==3&&retained->Data().cameraRot[0].w==1,"Published handle did not outlive library removal");
    Check(original->Data().fFocalLength[2]==4,"Original retained handle was invalidated");
}
void Validation()
{
    CameraAssetLibrary library;
    Reject<std::invalid_argument>([&]{CameraAssetBatch batch(library,{});});
    Reject<std::invalid_argument>([&]{CameraAssetBatch batch(library,Names(65));});
    for(const auto& r:std::vector<CameraBatchRequest>{{"","/camera.cam"},{std::string(256,'x'),"/camera.cam"},
        {std::string("bad\0name",8),"/camera.cam"},{"valid",""},{"valid",std::string("/camera.cam\0bad",15)},
        {"valid",std::string(2049,'x')}})
        Reject<std::invalid_argument>([&]{CameraAssetBatch batch(library,Requests{r});});
    for(const auto& names:std::vector<std::pair<std::string,std::string>>{{"First","fIRST"},{"a~","b]"}})
        Reject<std::runtime_error>([&]{CameraAssetBatch batch(library,Requests{{names.first,"/camera.cam"},{names.second,"/camera.cam"}});});
    auto old=LoadCameraAsset("/camera.cam","EXISTING");library.Insert(old);
    Reject<std::runtime_error>([&]{CameraAssetBatch batch(library,Requests{{"new","/camera.cam"},{"existing","/camera.cam"}});});
    Check(library.Find("existing")==old&&!library.Find("new")&&!nlAsyncReadsPending(nullptr),"Conflict validation changed library or queued I/O");
    CameraAssetBatch batch(library,Names(2));
    bool rejected=false;
    std::thread other([&]{try{batch.Poll();}catch(const std::logic_error&){rejected=true;}});other.join();
    Check(rejected,"Batch accepted foreign servicing thread");
    batch.Cancel();Accounting(batch);
    Check(batch.Progress().completed==0&&batch.Progress().cancelled==2,"Immediate cancellation accounting differs");
    Reject<std::runtime_error>([&]{batch.Publish();});
    batch.Cancel();batch.Service();batch.Poll();
}
void Failures()
{
    for(const auto* path:{"/empty.cam","/missing.cam","/malformed.cam","/truncated.cam"})
    {
        CameraAssetLibrary library;
        auto old=LoadCameraAsset("/camera.cam","old");library.Insert(old);
        CameraAssetBatch batch(library,Requests{{"good","/camera.cam"},{"bad",path},{"later","/camera.cam"}});
        Pump(batch);Accounting(batch);
        Check(batch.State()==CameraBatchState::Failed&&batch.Progress().failed==1&&batch.FailedAlias()=="bad","Failed request identity/accounting lost");
        if(std::string_view(path)=="/empty.cam"||std::string_view(path)=="/missing.cam")
            Check(batch.Progress().completed==1&&batch.Progress().cancelled==2,"Inline/start failure did not cancel all other requests");
        Reject([&]{batch.Publish();});
        Check(!library.Find("good")&&!library.Find("bad")&&!library.Find("later")&&library.Find("old")==old,
            "Failed batch partially published or changed existing assets");
        Check(!nlAsyncReadsPending(nullptr),"Failed batch retained NL requests");
    }
    CameraAssetLibrary library;
    CameraAssetBatch batch(library,Names(3));Pump(batch);
    auto conflict=LoadCameraAsset("/camera.cam","camera1");library.Insert(conflict);
    Reject<std::runtime_error>([&]{batch.Publish();});Accounting(batch);
    Check(batch.State()==CameraBatchState::Failed&&batch.Progress().succeeded==3&&batch.Progress().failed==0
        &&batch.FailedAlias().empty(),"Publication failure changed load completion accounting");
    Check(library.Find("camera1")==conflict&&!library.Find("camera0")&&!library.Find("camera2"),"Late library conflict partially committed");
}
struct Overlay
{
    enum Mode { Good,Short,Error,Blocked };
    std::atomic<Mode> mode{Good};std::atomic<unsigned> handles{0},reads{0};
    std::atomic<bool> entered{false},finished{false};
    std::vector<unsigned char> bytes;
    std::mutex mutex;std::condition_variable changed;bool released=false,fail_after_release=false;
    struct Handle { Overlay* owner;std::size_t at=0; };
    static void* Open(void* user){auto* p=new Handle{static_cast<Overlay*>(user)};++p->owner->handles;return p;}
    static void Close(void* data){auto* p=static_cast<Handle*>(data);--p->owner->handles;delete p;}
    static std::int64_t Seek(void* data,std::int64_t offset,std::int32_t origin)
    {auto& h=*static_cast<Handle*>(data);if(origin!=0||offset<0||std::size_t(offset)>h.owner->bytes.size())return -1;return h.at=offset;}
    static std::int64_t Read(void* data,std::uint8_t* output,std::size_t size)
    {
        auto& h=*static_cast<Handle*>(data);auto& o=*h.owner;++o.reads;o.entered=true;
        const auto mode=o.mode.load();
        if(mode==Error){o.finished=true;return -1;}
        if(mode==Short&&h.at>=3){o.finished=true;return 0;}
        if(mode==Blocked)
        {
            std::unique_lock lock(o.mutex);
            if(!o.changed.wait_for(lock,std::chrono::seconds(5),[&]{return o.released;})||o.fail_after_release)
            {o.finished=true;return -1;}
        }
        const auto limit=mode==Short?std::size_t(3):o.bytes.size();
        const auto n=std::min(size,limit-h.at);std::memcpy(output,o.bytes.data()+h.at,n);h.at+=n;o.finished=true;return n;
    }
    void Release(bool fail=false){std::lock_guard lock(mutex);fail_after_release=fail;released=true;changed.notify_all();}
    void WaitEntered()
    {
        const auto end=std::chrono::steady_clock::now()+std::chrono::seconds(3);
        while(!entered){Check(std::chrono::steady_clock::now()<end,"Controlled DVD worker did not start");SDL_Delay(1);}
    }
};
struct InstalledOverlay
{
    explicit InstalledOverlay(Overlay& o)
    {
        const AuroraOverlayCallbacks callbacks{Overlay::Open,Overlay::Close,Overlay::Read,Overlay::Seek};
        aurora_dvd_overlay_callbacks(&callbacks);
        const AuroraOverlayFile file{"/fault.cam",&o,static_cast<std::uint32_t>(o.bytes.size())};
        aurora_dvd_overlay_files(&file,1,nullptr);
    }
    ~InstalledOverlay(){aurora_dvd_overlay_files(nullptr,0,nullptr);}
};
void ReadErrors(const std::vector<unsigned char>& bytes)
{
    for(auto mode:{Overlay::Short,Overlay::Error})for(bool external:{false,true})
    {
        Overlay fault;fault.bytes=bytes;fault.mode=mode;InstalledOverlay overlay(fault);
        CameraAssetLibrary library;CameraAssetBatch batch(library,Requests{{"fault","/fault.cam"},{"good","/camera.cam"}});
        Pump(batch,external,true);Accounting(batch);
        Check(batch.State()==CameraBatchState::Failed&&batch.Progress().failed>=1&&fault.reads>0&&!fault.handles,
            "Actual NL read failure retained camera state or reported success");
        Reject([&]{batch.Publish();});Check(!library.Find("good"),"Read failure partially published cameras");
        mscharged::test::FinishDVDTestFaultCase(mode==Overlay::Error);
    }
    // A nonfatal short transfer is request-local; a real media fault also
    // fails a healthy request that was genuinely queued behind that I/O.
    for (bool fatal : {false,true})
    {
        Overlay fault;fault.bytes=bytes;fault.mode=fatal?Overlay::Blocked:Overlay::Short;
        InstalledOverlay overlay(fault);
        CameraAssetLoad unrelated("/fault.cam","unrelated");
        if(fatal)fault.WaitEntered();
        CameraAssetLibrary library;CameraAssetBatch batch(library,Names(1));bool observed=false;
        if(fatal)fault.Release(true);
        const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(3);
        while(!unrelated.Ready())
        {
            try {batch.Service();}catch(const std::runtime_error&){observed=true;}
            if(batch.State()!=CameraBatchState::Loading&&!unrelated.Ready())
                try {nlServiceFileSystem();}catch(const std::runtime_error&){observed=true;}
            Check(std::chrono::steady_clock::now()<deadline,"Unrelated read failure timed out");SDL_Delay(1);
        }
        Check(observed,"Shared NL pump error was swallowed");
        Pump(batch,false,fatal);
        if(fatal)
        {
            Check(batch.State()==CameraBatchState::Failed&&batch.Progress().failed==1&&!library.Find("camera0"),
                  "Queued healthy camera succeeded after the genuine media fatal latch");
            Reject([&]{batch.Publish();});
        }
        else
        {
            batch.Publish();
            Check(library.Find("camera0")&&batch.Progress().failed==0,"Foreign nonfatal NL failure aborted the wrong transaction");
        }
        Reject([&]{unrelated.Result();});
        Check(fault.entered&&fault.finished&&!fault.handles&&!nlAsyncReadsPending(nullptr),
              "Foreign camera fault case did not retire its actual source/file requests");
        mscharged::test::FinishDVDTestFaultCase(fatal);
    }
}
void ActiveCancellation(const std::vector<unsigned char>& bytes,bool destroy,bool fail)
{
    Overlay blocked;blocked.bytes=bytes;blocked.mode=Overlay::Blocked;InstalledOverlay overlay(blocked);
    CameraAssetLibrary library;
    auto batch=std::make_unique<CameraAssetBatch>(library,Requests{{"good","/camera.cam"},{"blocked","/fault.cam"}});
    blocked.WaitEntered();
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(3);
    while(batch->Progress().succeeded==0)
    {batch->Service();Check(std::chrono::steady_clock::now()<deadline,"First camera completion timed out");SDL_Delay(1);}
    Check(batch->Progress().succeeded==1&&batch->Progress().active==1&&!library.Find("good"),"Partial batch visibility/accounting differs");
    std::jthread release([&]{SDL_Delay(25);blocked.Release(fail);});
    if(destroy)batch.reset();
    else if(fail)
    {
        Pump(*batch,false,true);Accounting(*batch);
        Check(batch->State()==CameraBatchState::Failed&&batch->Progress().succeeded==1&&batch->Progress().failed==1,
            "Partial-success/read-failure accounting differs");
        Reject([&]{batch->Publish();});
    }
    else
    {
        batch->Cancel();Accounting(*batch);
        Check(batch->Progress().succeeded==1&&batch->Progress().cancelled==1&&batch->State()==CameraBatchState::Cancelled,
            "Partial-success cancellation accounting differs");
    }
    release.join();
    Check(blocked.finished&&!blocked.handles&&!nlAsyncReadsPending(nullptr)&&!library.Find("good"),"Active batch cleanup failed to drain workers or discard staged assets");
    mscharged::test::FinishDVDTestFaultCase(fail);
}
void NativeFailure(bool decoding)
{
    CameraAssetLibrary library;
    std::unique_ptr<CameraAssetBatch> batch;
    if(decoding)batch=std::make_unique<CameraAssetBatch>(library,Names(1));
    std::vector<void*> blocks;
    for(;;){try{blocks.push_back(StandardAllocator.Allocate(256*1024,32,false));}catch(const std::bad_alloc&){break;}}
    const auto remaining=StandardAllocator.LargestFreeBlock();
    if(remaining>256)blocks.push_back(StandardAllocator.Allocate(remaining-128,32,false));
    if(!decoding)batch=std::make_unique<CameraAssetBatch>(library,Names(2));
    Pump(*batch);Accounting(*batch);
    Check(batch->State()==CameraBatchState::Failed&&batch->Progress().failed==1,"Native allocation failure did not fail transaction");
    Reject<std::bad_alloc>([&]{batch->Publish();});
    for(void* p:blocks)StandardAllocator.Free(p);
    Check(!library.Find("camera0")&&!nlAsyncReadsPending(nullptr),"Native failure retained a partial camera set or request");
}
void PublicationAllocationFailures()
{
    unsigned failures=0,successes=0;
    for(long budget=0;budget<96;++budget)
    {
        CameraAssetLibrary library;
        auto old=LoadCameraAsset("/camera.cam","old");library.Insert(old);
        CameraAssetBatch batch(library,Names(3));Pump(batch);
        bool failed=false;allocation_budget=budget;
        try{batch.Publish();}catch(const std::bad_alloc&){failed=true;}
        allocation_budget=-1;
        Check(library.Find("old")==old,"Publication allocation failure changed existing asset");
        if(failed)
        {
            ++failures;
            Check(batch.State()==CameraBatchState::Failed&&!library.Find("camera0")&&!library.Find("camera1")&&!library.Find("camera2"),
                "Publication allocation failure exposed a partial set");
        }
        else {++successes;Check(library.Find("camera0")&&library.Find("camera1")&&library.Find("camera2"),"Successful publication lost a camera");break;}
        Accounting(batch);
    }
    Check(failures>8&&successes==1,"Publication allocation sweep missed rollback or successful commit");
}
void Shutdown()
{
    CameraAssetLibrary library;
    {
        CameraAssetBatch batch(library,Names(20));nlShutdownFileSystem();nlInitFileSystem();batch.Poll();Accounting(batch);
        Check(batch.State()==CameraBatchState::Failed&&batch.Progress().failed==8&&batch.Progress().cancelled==12,
            "Shutdown/restart lost pending and unstarted request accounting");
        Reject([&]{batch.Publish();});
    }
    {CameraAssetBatch batch(library,Names(3));Pump(batch);batch.Cancel();Accounting(batch);
        Check(batch.Progress().succeeded==3&&batch.Progress().cancelled==0&&!library.Find("camera0"),"Cancel after completion changed completed accounting");}
    {CameraAssetBatch batch(library,Names(3));Pump(batch);} // Unpublished successful data is discarded.
    {CameraAssetBatch batch(library,Names(20));} // Unstarted and active work both disappear.
    Check(!nlAsyncReadsPending(nullptr),"Batch destruction left NL work alive");
}
struct Session
{
    bool live=false,disc=false;
    ~Session(){if(live)ResetStartupFiles();if(disc)aurora_dvd_close();if(live){ResetStartupMemory();aurora_shutdown();}}
};
}
int main(int argc,char** argv)
{
    try
    {
        Check(argc==3,"Supply generated Wii ISO and fixture directory");
        const auto path=(std::filesystem::path(argv[2])/"runtime").string();std::filesystem::create_directories(path);
        AuroraConfig cfg{};cfg.appName="Charged camera batch tests";cfg.userPath=cfg.cachePath=path.c_str();cfg.resourcesPath=SDL_GetBasePath();
        cfg.desiredBackend=BACKEND_NULL;cfg.windowWidth=320;cfg.windowHeight=240;cfg.windowPosX=cfg.windowPosY=-1;cfg.logLevel=LOG_WARNING;
        cfg.mem1Size=MEM1_DEFAULT_SIZE;cfg.mem2Size=64*1024*1024;
        Session session;const auto host=aurora_initialize(argc,argv,&cfg);session.live=true;Check(host.window,"Aurora core initialization failed");
        InitializeStartupOS();nlInitMemory();Check(aurora_dvd_open(argv[1]),"Cannot mount generated camera disc");session.disc=true;nlInitFileSystem();
        mscharged::test::ConfigureDVDTestMedium(argv[1]);
        std::ifstream file(std::filesystem::path(argv[2])/"camera.cam",std::ios::binary);
        const std::vector<unsigned char> bytes{std::istreambuf_iterator<char>(file),{}};Check(!bytes.empty(),"Missing generated overlay fixture");
        auto verify=[&](auto action){const auto a=StandardAllocator.TotalFreeMemory(),b=VirtualAllocator.TotalFreeMemory();action();
            Check(StandardAllocator.TotalFreeMemory()==a&&VirtualAllocator.TotalFreeMemory()==b,"Camera batch leaked native arenas");};
        for(unsigned session=0;session<3;++session)
        {
            verify([&]{Success(session%2);});verify(Validation);verify(Failures);verify([&]{ReadErrors(bytes);});
            verify([&]{ActiveCancellation(bytes,false,false);});verify([&]{ActiveCancellation(bytes,true,false);});
            verify([&]{ActiveCancellation(bytes,false,true);});verify([&]{NativeFailure(false);});verify([&]{NativeFailure(true);});
            verify(PublicationAllocationFailures);verify(Shutdown);
            nlShutdownFileSystem();nlInitFileSystem();
        }
        std::cout<<checks<<" camera batch publication, NL read, cancellation, accounting and allocation rollback checks passed; three complete arena recoveries\n";
        return 0;
    }
    catch(const std::exception& e){allocation_budget=-1;std::cerr<<"FAILED: "<<e.what()<<" (check "<<checks<<")\n";return 1;}
}
