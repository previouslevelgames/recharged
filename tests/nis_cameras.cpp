#include "runtime/nis_cameras.h"
#include "runtime/graphics_memory.h"
#include "runtime/startup.h"
#include "runtime/tasks.h"
#include "NL/nlTask.h"
#include "resources/chunk_reader.h"
#include "Game/Camera/CameraMan.h"
#include <bit>
#include <cmath>
#include <climits>
#include <cstdlib>
#include <new>
#include <optional>
#include <cstring>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <thread>

// Selected ownership failures occur in host bookkeeping, independently of game arenas.
thread_local long allocation_budget=-1;
void* operator new(std::size_t size)
{
    if(allocation_budget==0)throw std::bad_alloc();
    if(allocation_budget>0)--allocation_budget;
    if(void* value=std::malloc(size?size:1))return value;
    throw std::bad_alloc();
}
void* operator new[](std::size_t size){return ::operator new(size);}
void operator delete(void* value)noexcept{std::free(value);}
void operator delete[](void* value)noexcept{std::free(value);}
void operator delete(void* value,std::size_t)noexcept{std::free(value);}
void operator delete[](void* value,std::size_t)noexcept{std::free(value);}
extern "C" std::uint32_t ChargedFixtureGetTick() { return 60750; }
extern "C" std::uint32_t ChargedFixtureGetBusClock() { return 243000000; }
using namespace mscharged;
namespace
{
unsigned checks = 0, ended = 0, transition_calls = 0;
void Check(bool value, const char* message) { ++checks; if (!value) throw std::runtime_error(message); }
void Near(float a, float b) { Check(std::isfinite(a) && std::abs(a-b)<.0001f, "NIS camera numeric state differs"); }
template<class F> void Reject(F action)
{ ++checks; try { action(); } catch(const std::exception&) { return; } throw std::runtime_error("Invalid NIS camera operation accepted"); }
using Blob = std::vector<std::uint8_t>;
void Word(Blob& data, std::uint32_t word) { for (int n : {24,16,8,0}) data.push_back(word>>n); }
void Set(Blob& data, std::size_t at, std::uint32_t word) { for (int n : {24,16,8,0}) data.at(at++)=word>>n; }
void CameraChunk(Blob& data, unsigned count, float base, float target=0)
{
    const auto begin=data.size(); Word(data,0x8002500b);Word(data,0);
    std::map<unsigned,Blob> channels;
    channels[0x25000]={'n','i','s',0};Word(channels[0x2500c],count);
    for(unsigned i=0;i<count;++i)
    {
        for(float v:{base+i,2.f,3.f})Word(channels[0x25003],std::bit_cast<unsigned>(v));
        for(float v:{target,0.f,0.f})Word(channels[0x25006],std::bit_cast<unsigned>(v));
        for(float v:{0.f,0.f,0.f,1.f})Word(channels[0x25004],std::bit_cast<unsigned>(v));
        Word(channels[0x25009],std::bit_cast<unsigned>(45.f+i));Word(channels[0x2500a],std::bit_cast<unsigned>(4.f+i));
    }
    for(const auto& [id,payload]:channels)
    {
        Word(data,id|0x05000000);const auto size=data.size();Word(data,0);data.resize(resources::Align(data.size(),32));
        data.insert(data.end(),payload.begin(),payload.end());Set(data,size,data.size()-size-4);
    }
    Set(data,begin+4,data.size()-begin-8);
}
Blob Fixture(float target=0)
{
    Blob data;CameraChunk(data,3,10,target);CameraChunk(data,6,20,target);return data;
}
NisCameras* callback_owner = nullptr;
NisCameraBinding* callback_binding = nullptr;
OriginalCameras* callback_core = nullptr;
void GuardedEnd()
{
    ++ended;
    Reject([] { callback_owner->Swap(); });
    Reject([] { callback_owner->Select(0,*callback_binding,0); });
    Reject([] { callback_owner->Activate(); });
    Reject([] { callback_owner->Advance(0); });
    Reject([] { callback_owner->Seek(0,0); });
    Reject([] { callback_owner->SetOffset(0,{0,0,0}); });
    Reject([] { callback_owner->SetInputs(0,{}); });
    Reject([] { callback_owner->SetEndCallback(0,nullptr); });
    Reject([] { callback_owner->Release(); });
    Reject([] { callback_binding->Release(); });
    Reject([] { callback_core->Release(); });
}
void ThrowingEnd() { throw std::runtime_error("injected NIS callback failure"); }
void Transition(eCameraMessage message)
{
    ++transition_calls;Check(message==eCM_ABORTED_BY_PUSH,"NIS activation did not abort the prior transition");
    Reject([] { callback_owner->Swap(); });Reject([] { callback_owner->Release(); });
    Reject([] { callback_binding->Release(); });
}
void Basic()
{
    OriginalCameras core;CameraPoseInput base;cCameraManager::PushCamera(&base);
    NisCameras cameras(core);
    Near(cameras.TimeLeft(),0);Near(cameras.CameraTimeLeft(0),-1);Near(cameras.CameraTimeLeft(1),-1);
    Check(!cameras.Camera(0)&&!cameras.Camera(1),"Empty NIS slots published cameras");
    Reject([&]{cameras.Activate();});Reject([&]{cameras.Swap();});
    std::optional<NisCameraBinding> a,b;std::weak_ptr<const CameraAsset> first,second;
    {
        auto bytes=Fixture();NisCameraAssets assets(bytes,"source");
        a.emplace(assets,0,true);b.emplace(assets,1);first=assets.Camera(0);second=assets.Camera(1);
        bytes.clear();bytes.shrink_to_fit();
    }
    cameras.Select(0,*a,2); // Original modulo chooses first of two tracks.
    cameras.Select(1,*b,3); // Second track.
    auto* primary=cameras.Camera(0);auto* secondary=cameras.Camera(1);
    Check(primary!=secondary&&a->Camera()==primary&&b->Camera()==secondary,"Initial NIS backlink differs");
    Near(primary->GetCameraPosition().x,-10);Near(secondary->GetCameraPosition().x,20);
    Near(cameras.TimeLeft(),.1f);Near(cameras.CameraTimeLeft(1),.1f);
    cameras.Activate();Check(cCameraManager::PeekCamera()==primary,"NIS primary not current");
    core.AttachFilters(*const_cast<cBaseCamera*>(primary));auto* filter=primary->m_pFilter[0];
    core.Advance(.04f,.04f);Near(cameras.Time(0),0);Near(cameras.Time(1),0); // Manager must not double advance.
    cameras.Seek(0,.25f);cameras.Seek(1,.5f);
    Near(cameras.CameraTimeLeft(1),.075f);Near(cameras.TimeLeft(),.075f);
    cameras.SetOffset(0,{3,4,5});Near(primary->GetCameraPosition().x,-10.5f);
    cameras.Advance(0);Near(primary->GetCameraPosition().x,-7.5f);Near(primary->GetCameraPosition().y,6);
    cameras.Swap();
    Check(cameras.Camera(0)==primary&&cameras.Camera(1)==secondary&&cCameraManager::PeekCamera()==primary,"Swap changed slot manager identity");
    Check(a->Camera()==secondary&&b->Camera()==primary&&a->RenderMode()==1&&b->RenderMode()==0,"Swap lost binding mode/backlink");
    Check(primary->m_pFilter[0]==filter&&secondary->m_pFilter[0]==nullptr,"Swap copied base-camera filters");
    Near(cameras.Time(0),.5f);Near(cameras.Time(1),.25f);Near(cameras.TimeLeft(),.1f);Near(cameras.CameraTimeLeft(1),.075f);
    Near(secondary->GetCameraPosition().x,-7.5f);
    for(unsigned i=0;i<128;++i)
    {
        cameras.Swap();Check(cameras.Camera(0)==primary&&cCameraManager::PeekCamera()==primary,"Repeated swap changed primary identity");
        Check(a->Camera()==(i%2?secondary:primary),"Repeated swap lost backlink");
    }
    core.Advance(.03f,.03f);Near(cameras.Time(0),.5f);Near(cameras.Time(1),.25f);
    auto overrun=cameras.Advance(.3f);Near(overrun[0],.2f);Near(overrun[1],.225f);
    Near(cameras.Time(0),1);Near(cameras.Time(1),1);Near(cameras.TimeLeft(),0);
    core.Advance(0,0);Near(cCameraManager::m_cameraPosition.x,25);
    // The original time-left expression can be negative after an explicit seek.
    cameras.Seek(0,1.5f);Near(cameras.TimeLeft(),-.1f);Near(cameras.CameraTimeLeft(1),-.1f);
    cameras.Seek(0,0);cameras.SetInputs(0,{true,true,0});cameras.Advance(0);
    Check(primary->GetFOV()>0&&primary->GetFOV()<45,"Explicit widescreen input did not reach original camera math");
    b.reset(); // Owns primary after 129 swaps; must leave the other binding intact.
    Check(!cameras.Camera(0)&&a->Camera()==secondary&&cCameraManager::PeekCamera()==&base,"Binding disposal unselected the wrong swapped camera");
    a.reset();Check(!cameras.Camera(1),"Secondary binding disposal left playback");
    Check(first.expired()&&second.expired(),"Destroyed bindings/playbacks retained game data");
    cameras.Release();Check(cCameraManager::PeekCamera()==&base,"NIS teardown changed borrowed base camera");
}
void ModeAndValidation()
{
    OriginalCameras core;NisCameraAssets assets(Fixture(),"modes");NisCameraBinding a(assets,0),b(assets,2);
    NisCameras cameras(core);cameras.Select(0,a,0);cameras.Select(1,b,1);cameras.Activate();
    Reject([&]{NisCameraBinding invalid(assets,-1);});Reject([&]{NisCameraBinding invalid(assets,3);});
    for(unsigned slot:{2u,UINT_MAX})
    {
        Reject([&]{cameras.Select(slot,a,0);});Reject([&]{cameras.Camera(slot);});
        Reject([&]{cameras.Time(slot);});Reject([&]{cameras.CameraTimeLeft(slot);});Reject([&]{cameras.Seek(slot,0);});
    }
    Reject([&]{cameras.Select(0,a,-1);});Reject([&]{cameras.Select(1,a,0);});
    {NisCameras other(core);Reject([&]{other.Select(0,a,0);});}
    Reject([&]{cameras.Advance(-1);});Reject([&]{cameras.Seek(0,-1);});
    Reject([&]{cameras.SetOffset(0,{NAN,0,0});});Reject([&]{cameras.SetInputs(0,{false,false,INFINITY});});
    bool wrong=false;std::thread worker([&]{try{cameras.Release();}catch(const std::logic_error&){wrong=true;}});worker.join();
    Check(wrong,"Wrong-thread NIS release accepted");
    Check(!cameras.Failed(),"Argument validation poisoned NIS owner");
    cameras.Swap();Check(a.RenderMode()==1&&b.RenderMode()==2,"Original mode 2 changed during swap");
    cameras.Swap();Check(a.RenderMode()==0&&b.RenderMode()==2,"Mode 0/1 did not reverse on repeated swap");
    const auto* primary=cameras.Camera(0);cameras.Select(0,a,1);
    Check(cameras.Camera(0)==primary&&a.Camera()==primary,"Reselection replaced stable primary identity");Near(cameras.Time(0),0);
    cameras.Release();Check(!a.Camera()&&!b.Camera(),"Owner release did not clear binding backlinks");
    a.Release();Reject([&]{cameras.Select(0,a,0);});
}
void Callbacks()
{
    OriginalCameras core;CameraPoseInput base,transition,second;cCameraManager::PushCamera(&base);
    NisCameraAssets assets(Fixture(),"callbacks");NisCameraBinding a(assets,0),b(assets,1);NisCameras cameras(core);
    callback_owner=&cameras;callback_binding=&a;callback_core=&core;
    cameras.Select(0,a,0);cameras.Select(1,b,1);
    transition_calls=0;cCameraManager::PushCameraWithTransition(&transition,1,eCT_EASE_IN,Transition,false);
    cameras.Activate();Check(transition_calls==1,"Activation did not deliver transition abort");
    cCameraManager::PushCameraWithTransition(&second,1,eCT_EASE_IN,Transition,false);
    cameras.Swap();Check(transition_calls==2,"Swap did not deliver transition abort");
    cCameraManager::Remove(base);cCameraManager::PushCameraWithTransition(&base,1,eCT_EASE_IN,Transition,false);
    cameras.Release();Check(transition_calls==2&&cCameraManager::m_transition==eCT_NONE&&cCameraManager::m_pCallback==nullptr,
        "NIS release delivered callback or left transition state");
}
void EndCallbacks()
{
    OriginalCameras core;NisCameraAssets assets(Fixture(),"ends");NisCameraBinding a(assets,0),b(assets,1);NisCameras cameras(core);
    callback_owner=&cameras;callback_binding=&a;callback_core=&core;
    cameras.Select(0,a,0);cameras.Select(1,b,1);cameras.Activate();ended=0;
    cameras.SetEndCallback(0,GuardedEnd);cameras.Advance(.1f);Check(ended==1&&!cameras.Failed(),"Caught callback mutation poisoned owner");
    cameras.Advance(.01f);Check(ended==2,"Original noncyclic end callback repetition changed");
    cameras.Swap();cameras.Advance(0);Check(ended==3,"End callback did not travel with swapped playback");
    cameras.Release();
}
void Failures()
{
    {
        OriginalCameras core;NisCameraAssets assets(Fixture(),"fail");NisCameraBinding a(assets,0),b(assets,1);NisCameras cameras(core);
        cameras.Select(0,a,0);cameras.Select(1,b,1);cameras.Activate();core.Advance(0,0);
        const auto published=cCameraManager::m_matView;cameras.SetEndCallback(0,ThrowingEnd);
        Reject([&]{cameras.Advance(.2f);});Check(cameras.Failed(),"Escaping callback did not poison NIS owner");
        Check(std::memcmp(&published,&cCameraManager::m_matView,sizeof(published))==0,"Failed manual advance published a camera pose");
        Reject([&]{cameras.Swap();});Reject([&]{cameras.Seek(0,0);});
        a.Release();Check(!a.Camera(),"Failed session could not release binding");cameras.Release();Check(!b.Camera(),"Failed session leaked backlink");
    }
    {
        OriginalCameras core;NisCameraAssets assets(Fixture(),"valid"),large(Fixture(std::numeric_limits<float>::max()),"large");
        NisCameraBinding a(assets,0),b(large,0);NisCameras cameras(core);cameras.Select(0,a,0);cameras.Activate();core.Advance(0,0);
        const auto* primary=a.Camera();const auto published=cCameraManager::m_matView;
        cameras.SetOffset(0,{std::numeric_limits<float>::max(),0,0});
        Reject([&]{cameras.Select(0,b,0);});Check(cameras.Failed()&&!b.Camera(),"Failed selection published its binding");
        Check(cCameraManager::PeekCamera()==primary&&std::memcmp(&published,&cCameraManager::m_matView,sizeof(published))==0,"Failed selection changed manager state");
        cameras.Release();Check(!a.Camera(),"Rollback teardown lost prior binding");
    }
}
void CoreLifetime()
{
    OriginalCameras core;NisCameraAssets assets(Fixture(),"lifetime");NisCameraBinding a(assets,0),b(assets,1);NisCameras old(core);
    old.Select(0,a,0);old.Select(1,b,1);old.Activate();core.Release();
    OriginalCameras next;CameraPoseInput base;cCameraManager::PushCamera(&base);
    Reject([&]{old.Activate();});Reject([&]{old.Swap();});Reject([&]{old.Advance(0);});Reject([&]{old.Select(0,a,0);});
    a.Release();old.Release();Check(!a.Camera()&&!b.Camera()&&cCameraManager::PeekCamera()==&base,"Old session teardown changed new camera stack");
}
void Destruction()
{
    OriginalCameras core;CameraPoseInput base;cCameraManager::PushCamera(&base);
    NisCameraAssets assets(Fixture(),"destruction");NisCameraBinding a(assets,0),b(assets,1);
    {
        NisCameras cameras(core);cameras.Select(0,a,0);cameras.Select(1,b,1);cameras.Swap();
    }
    Check(!a.Camera()&&!b.Camera()&&cCameraManager::PeekCamera()==&base,"Owner destructor left a live binding or stack entry");
    Blob actorOnly;Word(actorOnly,0x80017000);Word(actorOnly,0);NisCameraAssets empty(actorOnly,"empty");
    NisCameraBinding noCamera(empty,0);NisCameras cameras(core);
    Reject([&]{cameras.Select(0,noCamera,0);});
    Check(!cameras.Failed()&&!noCamera.Camera(),"Camera-free NIS selection changed state");
    cameras.Select(0,a,0);cameras.Select(1,b,1);cameras.Activate();
    // Explicit source release is one-way; it must not remove the unrelated slot.
    a.Release();Check(!cameras.Camera(0)&&cameras.Camera(1)==b.Camera(),"Source release removed unrelated playback");
    Reject([&]{cameras.Select(0,a,0);});
    Check(!cameras.Failed(),"Released source rejection poisoned owner");
}
void AllocationFailures()
{
    unsigned failures=0,successes=0;
    for(unsigned action=0;action<3;++action)
        for(long budget=0;budget<8;++budget)
        {
            OriginalCameras core;CameraPoseInput base;cCameraManager::PushCamera(&base);
            NisCameraAssets assets(Fixture(),"allocation");NisCameraBinding a(assets,0),b(assets,1),replacement(assets,0);
            NisCameras cameras(core);cameras.Select(0,a,0);cameras.Select(1,b,1);
            if(action==0)cameras.Activate();
            const auto* previous=cCameraManager::PeekCamera();const auto old_pose=previous->GetViewMatrix();
            bool failed=false;allocation_budget=budget;
            try
            {
                if(action==0)cameras.Select(0,replacement,1);
                else if(action==1)cameras.Activate();
                else cameras.Swap();
            }
            catch(const std::bad_alloc&){failed=true;}
            allocation_budget=-1;
            if(failed)
            {
                ++failures;Check(cameras.Failed(),"Host allocation failure did not poison partial operation");
                Check(cCameraManager::PeekCamera()==previous&&std::memcmp(&old_pose,&previous->GetViewMatrix(),sizeof(old_pose))==0,
                    "Preparation allocation failure mutated previous manager state");
                Check(!replacement.Camera(),"Failed selection published replacement binding");
            }
            else ++successes;
            cameras.Release();Check(!a.Camera()&&!b.Camera()&&!replacement.Camera(),"Allocation rollback leaked binding ownership");
            Check(cCameraManager::PeekCamera()==&base,"Allocation rollback damaged borrowed base camera");
        }
    Check(failures>=3&&successes>=3,"Host allocation sweep missed preparation failure or success");
}
void Owned(const char* filename)
{
    std::ifstream file(filename,std::ios::binary);Check(bool(file),"Cannot read owned NIS");Blob bytes{std::istreambuf_iterator<char>(file),{}};
    OriginalCameras core;NisCameraAssets assets(bytes,"owned");Check(assets.Layout().cameras.size()>=2,"Owned swap needs two tracks");
    bytes.clear();bytes.shrink_to_fit();
    for(unsigned track=1;track<assets.Layout().cameras.size();++track)
    {
        NisCameraBinding a(assets,0),b(assets,1,true);NisCameras cameras(core);
        cameras.Select(0,a,0);cameras.Select(1,b,track);cameras.Activate();const auto* identity=cameras.Camera(0);
        for(unsigned i=0;i<=100;++i)
        {
            const float time=i/100.f;cameras.Seek(0,time);cameras.Seek(1,1-time);core.Advance(0,0);CheckCameraPose(*identity);
            const auto expected=cameras.Camera(1)->GetViewMatrix();
            cameras.Swap();core.Advance(0,0);CheckCameraPose(*identity);
            Check(std::memcmp(&expected,&identity->GetViewMatrix(),sizeof(expected))==0,"Owned swap changed authored pose bits");
            Check(cameras.Camera(0)==identity&&a.Camera()!=b.Camera(),"Owned swap lost identities");
        }
        cameras.Release();Check(!a.Camera()&&!b.Camera(),"Owned teardown retained backlinks");
    }
}
}
int main(int argc,char** argv)
{
    try
    {
        std::vector<std::uint64_t> mem1(1024*1024),mem2(1024*1024);
        StandardAllocator.Initialize(mem1.data(),mem1.size()*8);VirtualAllocator.Initialize(mem2.data(),mem2.size()*8);gMemoryInitialized=1;
        {
            struct Tasks
            {
                Tasks() { nlTaskManager::Startup(4); }
                ~Tasks() { ShutdownNativeTaskManager(); }
            } tasks;
            if(argc==2)Owned(argv[1]);
            else for(unsigned i=0;i<3;++i){Basic();ModeAndValidation();Callbacks();EndCallbacks();Failures();CoreLifetime();Destruction();AllocationFailures();}
        }
        Check(StandardAllocator.TotalFreeMemory()==mem1.size()*8&&VirtualAllocator.TotalFreeMemory()==mem2.size()*8,"NIS camera sessions did not recover both arenas");
        ResetStartupMemory();std::cout<<checks<<" NIS playback checks passed\n";return 0;
    }
    catch(const std::exception& error){std::cerr<<"FAILED: "<<error.what()<<" (check "<<checks<<")\n";return 1;}
}
