#include "runtime/frontend_camera_assets.h"
#include "runtime/animated_camera.h"
#include "runtime/startup.h"
#include "runtime/startup_files.h"
#include "Game/Camera/CameraMan.h"
#include "NL/MemAlloc.h"
#include "NL/nlFileGC.h"
#include <aurora/aurora.h>
#include <aurora/dvd.h>
#include <SDL3/SDL.h>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <string_view>
#include <thread>

using namespace mscharged;
namespace
{
unsigned checks=0;
void Check(bool good,const char* message){++checks;if(!good)throw std::runtime_error(message);}
template<class F> void Reject(F action)
{++checks;try{action();}catch(const std::exception&){return;}throw std::runtime_error("Invalid frontend catalog operation succeeded");}
void Pump(CameraAssetBatch& batch,bool external)
{
    const auto end=std::chrono::steady_clock::now()+std::chrono::seconds(10);
    while(batch.State()==CameraBatchState::Loading)
    {
        if(external){nlServiceFileSystem();batch.Poll();}else batch.Service();
        const auto p=batch.Progress();
        Check(p.requested==p.pending+p.completed+p.cancelled&&p.completed==p.succeeded+p.failed,"Catalog accounting diverged");
        Check(std::chrono::steady_clock::now()<end,"Frontend camera loading timed out");
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}
void Load(std::string_view mode,bool external)
{
    const auto catalog=FrontendCameraCatalog();
    Check(catalog.size()==37,"Original frontend catalog count changed; review source adoption");
    CameraAssetLibrary library;
    auto existing=LoadCameraAsset(catalog[1].fileName,"preexisting");library.Insert(existing);
    auto batch=LoadFrontendCameraAssets(library);
    Check(batch.Progress().requested==37,"Frontend loader skipped an original catalog entry");
    for(const auto& entry:catalog)Check(!library.Find(entry.animationName),"Frontend catalog published before completion");
    Pump(batch,external);
    const auto p=batch.Progress();
    Check(!nlAsyncReadsPending(nullptr),"Completed/failed frontend batch retained NL requests");
    if(mode=="success"||mode=="owned")
    {
        Check(batch.State()==CameraBatchState::Ready&&p.requested==37&&p.completed==37&&p.succeeded==37
            &&!p.failed&&!p.cancelled&&!p.pending&&!p.active,"Frontend camera catalog did not complete all37requests");
        for(const auto& entry:catalog)Check(!library.Find(entry.animationName),"Frontend cameras published implicitly");
        batch.Publish();
        for(const auto& entry:catalog)
        {
            const auto asset=library.Find(entry.animationName);
            Check(asset&&asset->Name()==entry.animationName&&asset->Data().m_uKeyCount>=2,"Original camera alias/name mapping lost");
            if(mode=="success")Check(asset->Data().m_uKeyCount==3&&asset->Data().cameraPos[2].x==2,"Generated catalog track was not decoded");
        }
        auto selected=library.Find("FECHOOSECAPTAINS");
        Check(selected&&selected==library.Find(catalog.front().animationName),"Original idle alias selection differs");
        const auto keys=selected->Data().m_uKeyCount;
        library.Erase("fechoosecaptains");library.Clear();
        Check(selected->Data().m_uKeyCount==keys&&existing->Data().m_uKeyCount>=2,"Retained selection died with named library");
        OriginalCameras cameras;AnimatedCamera playback(selected);cCameraManager::PushCamera(&playback.Camera());
        for(float t:{0.f,.25f,.5f,.75f,1.f}){playback.Seek(t);cameras.Advance(0,0);CheckCameraPose(playback.Camera());}
        std::cout<<"Original frontend catalog: 37 requested, 37 completed, 37 published; retained fechoosecaptains "
            <<keys<<" keys sampled through original CameraMan\n";
    }
    else
    {
        const auto* alias=mode=="missing-first"?catalog.front().animationName:catalog.back().animationName;
        Check(batch.State()==CameraBatchState::Failed&&p.failed==1&&batch.FailedAlias()==alias
            &&p.completed+p.cancelled==37&&!p.pending&&!p.active,"Frontend failure lost requested/error/cancel accounting");
        Reject([&]{batch.Publish();});
        for(const auto& entry:catalog)Check(!library.Find(entry.animationName),"Missing/malformed frontend track exposed a partial set");
        Check(library.Find("preexisting")==existing,"Failed catalog transaction changed prior assets");
        std::cout<<"Original frontend catalog rejected "<<alias<<"; "<<p.completed<<" completed, "<<p.cancelled
            <<" cancelled, zero entries published\n";
    }
}
void Cancel()
{
    CameraAssetLibrary library;auto batch=LoadFrontendCameraAssets(library);batch.Cancel();
    const auto p=batch.Progress();
    Check(p.requested==37&&p.cancelled==37&&!p.completed&&!p.pending&&!p.active,"Frontend immediate cancellation accounting differs");
    Check(!nlAsyncReadsPending(nullptr),"Cancelled frontend batch retained NL workers");
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
        if(argc==2&&std::string_view(argv[1])=="--catalog")
        {
            for(const auto& entry:FrontendCameraCatalog())std::cout<<entry.fileName<<'\t'<<entry.animationName<<'\n';
            return 0;
        }
        Check(argc==4,"Supply ISO, data directory and success/missing-first/missing-last/malformed-last/owned mode");
        const std::string_view mode=argv[3];
        Check(mode=="success"||mode=="missing-first"||mode=="missing-last"||mode=="malformed-last"||mode=="owned","Unknown catalog test mode");
        const auto folder=(std::filesystem::path(argv[2])/"frontend-camera-data").string();std::filesystem::create_directories(folder);
        AuroraConfig config{};config.appName="Charged frontend camera catalog";config.userPath=config.cachePath=folder.c_str();
        config.resourcesPath=SDL_GetBasePath();config.desiredBackend=BACKEND_NULL;config.windowWidth=320;config.windowHeight=240;
        config.windowPosX=config.windowPosY=-1;config.logLevel=LOG_WARNING;config.mem1Size=MEM1_DEFAULT_SIZE;config.mem2Size=64*1024*1024;
        Session session;const auto host=aurora_initialize(argc,argv,&config);session.live=true;Check(host.window,"Aurora initialization failed");
        InitializeStartupOS();nlInitMemory();Check(aurora_dvd_open(argv[1]),"Cannot mount camera catalog disc");session.disc=true;nlInitFileSystem();
        for(unsigned i=0;i<(mode=="owned"?1u:3u);++i)
        {
            const auto a=StandardAllocator.TotalFreeMemory(),b=VirtualAllocator.TotalFreeMemory();
            Load(mode,i%2);
            if(mode=="success")Cancel();
            Check(StandardAllocator.TotalFreeMemory()==a&&VirtualAllocator.TotalFreeMemory()==b,"Frontend camera catalog leaked native arenas");
            nlShutdownFileSystem();nlInitFileSystem();
        }
        std::cout<<checks<<" frontend camera catalog checks passed; all native arenas recovered\n";
        return 0;
    }
    catch(const std::exception& e){std::cerr<<"FAILED: "<<e.what()<<" (check "<<checks<<")\n";return 1;}
}
