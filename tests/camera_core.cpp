#include "runtime/cameras.h"
#include "runtime/tasks.h"
#include "runtime/startup.h"
#include "runtime/graphics_memory.h"
#include "Game/Camera/CameraMan.h"
#include "Game/Camera/noisefilter.h"
#include "Game/Camera/DebugCam.h"
#include "NL/MemAlloc.h"
#include "NL/nlMemory.h"
#include "NL/nlTask.h"
#include "NL/gl/glMatrix.h"
#include <cmath>
#include <functional>
#include <iostream>
#include <limits>
#include <thread>
#include <vector>

extern "C" std::uint32_t ChargedFixtureGetTick() { return 60750; }
extern "C" std::uint32_t ChargedFixtureGetBusClock() { return 243000000; }
using namespace mscharged;
namespace
{
unsigned checks=0, destroyed=0;
std::vector<eCameraMessage> messages;
std::function<void()> on_message;
void Check(bool value,const char* what) { ++checks; if (!value) throw std::runtime_error(what); }
void Near(float value,float expected,float tolerance=0.00005f)
{
    ++checks;
    if (!std::isfinite(value) || std::abs(value-expected)>tolerance)
        throw std::runtime_error("Camera value "+std::to_string(value)+" differs from "+std::to_string(expected));
}
template<class E=std::exception,class F> void Reject(F action)
{
    try { action(); } catch (const E&) { ++checks; return; }
    throw std::runtime_error("Invalid camera operation accepted");
}
void Callback(eCameraMessage message) { messages.push_back(message); if (on_message) on_message(); }
struct Fixture : cBaseCamera
{
    nlMatrix4 view;
    nlVector3 pos{0,0,4}, target{};
    float fov=40;
    unsigned updates=0, reactivated=0;
    eCameraType type=eCameraType_Debug;
    bool fail_update=false;
    std::function<void()> on_destroy;
    Fixture() { glMatrixLookAt(view,pos,target,{0,1,0}); }
    ~Fixture() override { ++destroyed; if (on_destroy) on_destroy(); }
    eCameraType GetType() override { return type; }
    void Update(float) override { ++updates; if(fail_update) throw std::runtime_error("camera fixture"); }
    void Reactivate() override { ++reactivated; }
    const nlMatrix4& GetViewMatrix() const override { return view; }
    const nlVector3& GetCameraPosition() const override { return pos; }
    const nlVector3& GetTargetPosition() const override { return target; }
    float GetFOV() const override { return fov; }
    void At(nlVector3 position,float degrees) { pos=position;fov=degrees;glMatrixLookAt(view,pos,target,{0,1,0}); }
};
struct Throwing : Fixture { Throwing() { throw std::runtime_error("constructor fixture"); } };
struct LargeCamera : Fixture { unsigned char payload[512]; };

void Stack()
{
    OriginalCameras owner;
    Reject([] { OriginalCameras duplicate; });
    Fixture a,b,c;
    Reject([] { cCameraManager::PushCamera(nullptr); });
    Reject([] { cCameraManager::PopCamera(); });
    cCameraManager::PushCamera(&a); cCameraManager::PushCamera(&b); cCameraManager::PushCamera(&c);
    Reject([&] { cCameraManager::PushCamera(&b); });
    Check(cCameraManager::HasCamera(&b) && GetNextCamera()==&b,"Original ring order incorrect");
    cCameraManager::Remove(b);
    Check(!cCameraManager::HasCamera(&b) && b.m_next==nullptr,"Middle removal failed");
    Check(cCameraManager::PopCamera()==&c && cCameraManager::PopCamera()==&a,"Pop order incorrect");
    Check(!cCameraManager::PeekCamera(),"Last removal retained a camera");
    cCameraManager::PushCamera(&a); cCameraManager::PushCamera(&b);
    cCameraManager::Remove(eCameraType_Debug,false);
    Check(!cCameraManager::PeekCamera(),"Typed removal skipped ring members");
    owner.AttachFilters(a); cCameraManager::PushCamera(&a);
    owner.Release();
    Check(!a.m_pFilter[0] && !a.m_pFilter[1] && !a.m_next,"Borrowed camera retained session pointers");
}
void Ownership()
{
    OriginalCameras owner;
    const auto before=StandardAllocator.TotalFreeMemory();
    Reject([] { auto* p=new Throwing; (void)p; });
    Check(StandardAllocator.TotalFreeMemory()==before,"Throwing camera constructor leaked");
    Reject([] { auto* p=new (8,true) Throwing; (void)p; });
    Check(StandardAllocator.TotalFreeMemory()==before,"Aligned constructor failure leaked");
    auto* large=new LargeCamera;memset(large->payload,0x5a,sizeof(large->payload));
    Check(large->payload[511]==0x5a,"Camera allocation retained a fixed Wii size");delete large;
    auto* a=new Fixture; Fixture* b;
    { ScopedGameAllocator arena(VirtualAllocator);b=new (8,true) Fixture; }
    cCameraManager::PushCamera(a); cCameraManager::PushCamera(b);
    Fixture outsider;bool rejected=false;
    a->on_destroy=[&] { try { cCameraManager::PushCamera(&outsider); } catch(const std::logic_error&) { rejected=true; } };
    const auto count=destroyed;
    cCameraManager::Remove(eCameraType_Debug,true);
    Check(destroyed==count+2,"Owned typed removal did not destroy exactly twice");
    Check(rejected && !cCameraManager::HasCamera(&outsider),"Destructor mutated typed-removal traversal");
    auto* popped=new Fixture; cCameraManager::PushCamera(popped); cCameraManager::PopCamera();
    delete popped;
    auto* attached=new Fixture; cCameraManager::PushCamera(attached); delete attached;
    Check(!cCameraManager::PeekCamera(),"Destruction left a stale camera in the ring");
    Fixture borrowed; cCameraManager::PushCamera(&borrowed);
    auto* replacement=new Fixture;
    Reject([&] { cCameraManager::PushCameraWithTransition(replacement,1,eCT_EASE_IN,nullptr,true); });
    Reject([&] { cCameraManager::Remove(eCameraType_Debug,true); });
    Check(cCameraManager::PeekCamera()==&borrowed,"Rejected ownership changed the stack");
    cCameraManager::PopCamera(); cCameraManager::PushCamera(replacement);
    auto* next=new Fixture;
    cCameraManager::PushCameraWithTransition(next,1,eCT_EASE_IN,nullptr,true);
    Check(cCameraManager::PeekCamera()==next,"Owned transition replacement failed");
    // A detached owned allocation is also reclaimed by the session.
    auto* detached=new Fixture; (void)detached;
    owner.Release();
}
void Transition()
{
    OriginalCameras owner;Fixture a,b,c;
    b.At({2,0,4},60);c.At({4,0,4},80);
    cCameraManager::PushCamera(&a);owner.Advance(0,0);
    Near(cCameraManager::GetDistanceFromCameraToObject({0,0,0}),4);
    nlVector3 axis;cCameraManager::GetViewVector(axis);Near(axis.z,-1);
    cCameraManager::GetUpVector(axis);Near(axis.y,1);
    Reject([&] { cCameraManager::PushCameraWithTransition(&b,0,eCT_EASE_IN,nullptr,false); });
    Reject([&] { cCameraManager::PushCameraWithTransition(&b,-1,eCT_EASE_IN,nullptr,false); });
    messages.clear();
    cCameraManager::PushCameraWithTransition(&b,1,eCT_EASE_IN,Callback,false);
    owner.Advance(.25f,.25f);
    Near(cCameraManager::m_fFOV,40); // Original evaluates before advancing its time.
    owner.Advance(0,0);
    Near(cCameraManager::m_fFOV,42.0703125f);
    Near(cCameraManager::m_cameraPosition.x,.20703125f);
    owner.Advance(.25f,.25f);owner.Advance(0,0);
    Near(cCameraManager::m_fFOV,50);Near(cCameraManager::m_cameraPosition.x,1);
    Near(cCameraManager::m_matView.m13,std::sin(std::atan(.5f)*.5f),.0001f);
    owner.Advance(.5f,.5f);owner.Advance(0,0);
    Near(cCameraManager::m_fFOV,60);Check(messages.empty(),"Transition completed at >= instead of original >");
    on_message=[&] { Reject([&] { cCameraManager::PushCamera(&c); }); };
    owner.Advance(.01f,.01f);on_message={};
    Check(messages==std::vector<eCameraMessage>{eCM_COMPLETE},"Completion callback count changed");
    owner.Advance(0,0);Check(messages.size()==1,"Completion callback repeated");
    cCameraManager::PopCamera();
    Reject([&] { cCameraManager::PopCameraWithTransition(1,eCT_EASE_IN,nullptr); });
    cCameraManager::PushCameraWithTransition(&b,1,eCT_EASE_IN,Callback,false);
    cCameraManager::PushCamera(&c);
    Check(messages.back()==eCM_ABORTED_BY_PUSH,"Push did not abort transition");
    cCameraManager::PopCamera();
    cCameraManager::PushCameraWithTransition(&c,1,eCT_EASE_IN,Callback,false);
    cCameraManager::PopCamera();
    Check(messages.back()==eCM_ABORTED_BY_POP,"Pop did not abort transition");
    cCameraManager::PushCameraWithTransition(&c,1,eCT_EASE_IN,Callback,false);
    owner.Advance(.25f,.25f);
    Check(cCameraManager::PopCameraWithTransition(2,eCT_EASE_IN,Callback)==&c,"Transition pop failed");
    Near(cCameraManager::m_fTransitionTime,.75f);Near(cCameraManager::m_fTransitionSpeed,.5f);
}
float NoiseOracle(int value)
{
    std::uint64_t n=((std::uint64_t(std::uint32_t(value))<<13)^std::uint32_t(value))&0xffffffffu;
    std::uint64_t h=(n*((n*n*15731+789221)&0xffffffffu)+1376312589)&0xffffffffu;
    return float(1.0-double(h&0x7fffffffu)/1073741824.0);
}
void Filters()
{
    OriginalCameras owner;Fixture a;owner.AttachFilters(a);cCameraManager::PushCamera(&a);
    nlMatrix4 identity,result;identity.SetIdentity();owner.Noise().Filter(identity,result);
    Near(result.m41,0);Near(result.m42,0);Near(result.m43,0);
    FireCameraRumbleFilter(.1f,.2f,5000,10);
    nlTaskManager::m_pInstance->mCurrentState=1;owner.Advance(.1f,.1f);
    Near(owner.Rumble().v2Pos1.x,.1f);
    nlTaskManager::m_pInstance->mCurrentState=4;
    owner.Rumble().Rumble(.1f,.2f,5000,10);owner.Rumble().Update(.1f);
    const auto clamped=owner.Rumble().v2Pos1;
    owner.Rumble().Rumble(.1f,.2f,5000,10);owner.Rumble().Update(.02f);
    Near(owner.Rumble().v2Pos1.x,clamped.x);Near(owner.Rumble().v2Pos1.y,clamped.y);
    auto& noise=owner.Noise();noise.Start({1,2,3},10,-1);
    const auto seed=noise.mSeed;
    noise.Update(.125f);
    const float blend=(1-std::cos(3.1415927f*.25f))*.5f;
    Near(noise.mDisplacement.x,NoiseOracle(1+int(seed.x))*(1-blend)+NoiseOracle(2+int(seed.x))*blend);
    Near(noise.mDisplacement.y,2*(NoiseOracle(1+int(seed.y))*(1-blend)+NoiseOracle(2+int(seed.y))*blend));
    Near(noise.mDisplacement.z,3*(NoiseOracle(1+int(seed.z))*(1-blend)+NoiseOracle(2+int(seed.z))*blend));
    for (unsigned i=0;i<500;++i) { noise.Update(.01f);Check(std::isfinite(noise.mDisplacement.x),"Noise became non-finite"); }
    noise.Start({1,1,1},10,.1f);noise.Update(.1f);Check(!noise.mActive,"Finite noise duration did not stop");
    noise.Reset();noise.Filter(identity,result);Near(result.m41,0);
    Reject([&] { noise.Start({1,1,1},std::numeric_limits<float>::infinity(),1); });
    noise.Start({1,1,1},1e20f,-1);Reject([&] { noise.Update(1); });
    Reject([&] { owner.Rumble().Update(-1); });
    Reject([&] { owner.Rumble().Rumble(0,0,std::numeric_limits<float>::infinity(),0); });
    Fixture b;cCameraManager::PushCamera(&b);cCameraManager::PopCamera();
    Check(a.reactivated==2,"Remaining camera/filter reset order changed");
}
void Failures()
{
    { OriginalCameras owner;Fixture a; bool rejected=false;
      std::thread wrong([&] { try { cCameraManager::PushCamera(&a); } catch (...) { rejected=true; } });wrong.join();
      Check(rejected,"Foreign thread mutated camera state");
      cCameraManager::PushCamera(&a);a.fail_update=true;Reject([&] { owner.Advance(0,0); });
      Reject([&] { owner.Advance(0,0); });owner.Release(); }
    { OriginalCameras owner;Fixture a,b;cCameraManager::PushCamera(&a);
      on_message=[] { throw std::runtime_error("callback fixture"); };
      cCameraManager::PushCameraWithTransition(&b,1,eCT_EASE_IN,Callback,false);
      Reject([&] { owner.Advance(2,2); });Reject([&] { cCameraManager::PopCamera(); });on_message={}; }
    { OriginalCameras owner; cCameraManager::PushWorldUpVector();
      Check(cCameraManager::m_UpVectorStackSize==1,"Up-vector push failed");
      cCameraManager::PopWorldUpVector();Check(cCameraManager::m_UpVectorStackSize==0,"Up-vector pop failed");
      cCameraManager::PushWorldUpVector();Reject([] { cCameraManager::PushWorldUpVector(); }); }
    { OriginalCameras owner;Fixture a;cCameraManager::PushCamera(&a);
      a.fov=std::numeric_limits<float>::quiet_NaN();Reject([&] { owner.Advance(0,0); }); }
    { OriginalCameras owner;Fixture a,b;a.m_pFilter[0]=&owner.Noise();cCameraManager::PushCamera(&a);
      Reject([&] { cCameraManager::PushCamera(&b); });a.m_pFilter[0]=nullptr; }
    { OriginalCameras owner;Reject([] { auto* p=cBaseCamera::operator new(std::size_t(1)<<40);(void)p; }); }
    // Original NL trigonometry uses quantized lookup tables.
    Near(AdjustFOVForWidescreen(60),2*std::atan(std::tan(3.1415927f/6)*1.25f/1.666f)*180/3.1415927f,.1f);
    Reject([] { AdjustFOVForWidescreen(180); });
    static_assert(sizeof(cDebugCamera)>160,"Expected native debug camera layout");
}
}
int main()
{
    try
    {
        std::vector<std::uint64_t> mem1(1024*1024),mem2(1024*1024);
        ResetStartupMemory();StandardAllocator.Initialize(mem1.data(),mem1.size()*8);
        VirtualAllocator.Initialize(mem2.data(),mem2.size()*8);gMemoryInitialized=1;
        for(int i=0;i<3;++i)
        {
            nlTaskManager::Startup(4);
            Stack();Ownership();Transition();Filters();Failures();
            ShutdownNativeTaskManager();
            Check(!cCameraManager::m_cameraStack,"Camera shutdown retained the ring");
            Check(StandardAllocator.TotalFreeMemory()==mem1.size()*8 && VirtualAllocator.TotalFreeMemory()==mem2.size()*8,"Camera core leaked game arenas");
        }
        ResetStartupMemory();std::cout<<checks<<" original camera core checks and three arena recoveries passed\n";
    }
    catch(const std::exception& e) { std::cerr<<e.what()<<'\n';return 1; }
}
