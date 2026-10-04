#include "runtime/frontend_cameras.h"
#include "runtime/frontend_camera_assets.h"
#include "runtime/graphics_memory.h"
#include "runtime/startup.h"
#include <bit>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <new>
#include <vector>

// Sweep host allocation failures separately from actual game-arena exhaustion.
thread_local long allocation_budget=-1;
void* operator new(std::size_t size)
{
    if(allocation_budget==0)throw std::bad_alloc();
    if(allocation_budget>0)--allocation_budget;
    if(void* memory=std::malloc(size?size:1))return memory;
    throw std::bad_alloc();
}
void* operator new[](std::size_t size){return ::operator new(size);}
void operator delete(void* memory)noexcept{std::free(memory);}
void operator delete[](void* memory)noexcept{std::free(memory);}
void operator delete(void* memory,std::size_t)noexcept{std::free(memory);}
void operator delete[](void* memory,std::size_t)noexcept{std::free(memory);}
extern "C" std::uint32_t ChargedFixtureGetTick(){return 60750;}
extern "C" std::uint32_t ChargedFixtureGetBusClock(){return 243000000;}

using namespace mscharged;
namespace
{
unsigned checks=0;
using Blob=std::vector<std::uint8_t>;
std::vector<std::pair<unsigned,eCameraMessage>> messages;
std::function<void()> on_message;
void Check(bool good,const char* message){++checks;if(!good)throw std::runtime_error(message);}
void Near(float actual,float expected)
{++checks;if(!std::isfinite(actual)||std::abs(actual-expected)>.0003f)throw std::runtime_error("Frontend camera pose differs");}
template<class E=std::exception,class F>void Reject(F action)
{++checks;try{action();}catch(const E&){return;}throw std::runtime_error("Invalid frontend camera operation succeeded");}
void CallbackA(eCameraMessage message){messages.emplace_back(1,message);if(on_message)on_message();}
void CallbackB(eCameraMessage message){messages.emplace_back(2,message);if(on_message)on_message();}
void Word(Blob& bytes,std::uint32_t word){for(int shift:{24,16,8,0})bytes.push_back(word>>shift);}
Blob Fixture(unsigned index,bool motion=false)
{
    std::map<unsigned,Blob> chunks;
    chunks[0x25000]={'f','e',0,0};Word(chunks[0x2500c],3);
    for(unsigned key=0;key<3;++key)
    {
        for(float value:{float(index*2)+(motion?float(key):0.f),1.f,5.f})Word(chunks[0x25003],std::bit_cast<unsigned>(value));
        for(float value:{0.f,0.f,0.f})Word(chunks[0x25006],std::bit_cast<unsigned>(value));
        for(float value:{0.f,0.f,0.f,1.f})Word(chunks[0x25004],std::bit_cast<unsigned>(value));
        Word(chunks[0x25009],std::bit_cast<unsigned>(40.f+index+(motion?10.f*key:0.f)));
        Word(chunks[0x2500a],std::bit_cast<unsigned>(2.f));
    }
    Blob bytes;Word(bytes,0x8002500b);Word(bytes,0);
    for(const auto& [id,payload]:chunks){Word(bytes,id);Word(bytes,payload.size());bytes.insert(bytes.end(),payload.begin(),payload.end());}
    const auto size=bytes.size()-8;for(unsigned i=0;i<4;++i)bytes[4+i]=size>>(24-8*i);
    return bytes;
}
std::string Alias(unsigned index){return FrontendCameraCatalog()[index].animationName;}
void Fill(CameraAssetLibrary& library,unsigned count=37)
{for(unsigned i=0;i<count;++i)library.Insert(CameraAsset::Decode(Fixture(i),Alias(i)));}
void Pose(FrontendCameras& frontend,unsigned index)
{
    frontend.Advance(0,0);
    const auto* camera=frontend.ActiveCamera();Check(camera&&camera==cCameraManager::PeekCamera(),"Active camera identity differs");
    Check(cCameraManager::PeekCamera()->GetType()==eCameraType_Animated,"Frontend wrapper lost original animated type");
    CheckCameraPose(*camera);Near(camera->GetCameraPosition().x,index*2);Near(camera->GetFOV(),40+index);
    Near(cCameraManager::m_cameraPosition.x,index*2);Near(cCameraManager::m_cameraPosition.y,1);Near(cCameraManager::m_cameraPosition.z,5);
    Near(cCameraManager::m_fFOV,40+index);const auto& m=cCameraManager::m_matView;
    Near(m.m11,1);Near(m.m22,1);Near(m.m33,1);Near(m.m41,-float(index*2));Near(m.m42,-1);Near(m.m43,-5);
}
void Selection()
{
    OriginalCameras core;CameraAssetLibrary library;Fill(library);FrontendCameras frontend(core,library);
    Check(FrontendCameraCatalog().size()==37,"Catalog source count changed");
    for(unsigned i=0;i<37;++i)
    {
        auto name=Alias(i);for(char& c:name)if(c>='a'&&c<='z')c-=32;
        const auto& camera=frontend.Push(name,CallbackA);
        Check(frontend.ActiveAlias()==Alias(i)&&frontend.ActiveCamera()==&camera&&frontend.Size()==1,"Named selection differs");
        Pose(frontend,i);frontend.Pop(CallbackB);
        Check(!frontend.ActiveCamera()&&!cCameraManager::PeekCamera()&&frontend.Size()==0,"Immediate pop retained a camera");
    }
    Check(messages.empty(),"Immediate FE operations invoked the supplied callback");
    frontend.Push(Alias(0));std::weak_ptr<const CameraAsset> retained=library.Find(Alias(0));library.Clear();
    Check(!retained.expired(),"Selected track was released with the library");Pose(frontend,0);
    Reject([&]{frontend.Push(Alias(1));});Pose(frontend,0);
    frontend.Pop();Check(retained.expired(),"Popped frontend camera retained native data");
}
void Validation()
{
    OriginalCameras core;CameraAssetLibrary library;Fill(library,3);FrontendCameras frontend(core,library);
    library.Insert(CameraAsset::Decode(Fixture(0),"noncatalog"));
    Reject([&]{frontend.Push("noncatalog");});Reject([&]{frontend.Push(Alias(36));});Reject([&]{frontend.Push("");});
    Reject([&]{frontend.Pop();});Reject([&]{frontend.Push(Alias(0),nullptr,0,true);});
    Reject([&]{frontend.Push(Alias(0),nullptr,1);});
    frontend.Push(Alias(0));const auto* previous=frontend.ActiveCamera();
    const auto free=VirtualAllocator.TotalFreeMemory();
    for(float value:{-1.f,std::numeric_limits<float>::infinity(),std::numeric_limits<float>::quiet_NaN(),std::numeric_limits<float>::denorm_min()})
    {Reject([&]{frontend.Push(Alias(1),nullptr,value);});Reject([&]{frontend.Pop(nullptr,value);});}
    Reject([&]{frontend.Pop(nullptr,1);});
    Check(!frontend.Failed()&&frontend.ActiveCamera()==previous&&VirtualAllocator.TotalFreeMemory()==free,"Validation changed existing ownership");
    bool rejected=false;std::thread other([&]{try{frontend.Push(Alias(1));}catch(const std::logic_error&){rejected=true;}});other.join();
    Check(rejected&&frontend.ActiveCamera()==previous,"Wrong-thread selection changed the stack");
    for(unsigned i=1;i<MaximumFrontendCameras;++i)frontend.Push(Alias(i%3));
    Reject([&]{frontend.Push(Alias(1));});const auto* replaced=frontend.ActiveCamera();
    frontend.Push(Alias(2),nullptr,0,true);
    Check(frontend.Size()==MaximumFrontendCameras&&frontend.ActiveCamera()!=replaced,"Immediate replacement changed stack depth");
    frontend.Release();frontend.Release();Check(!cCameraManager::PeekCamera(),"Release left frontend stack entries");
    Reject([&]{frontend.Push(Alias(0));});
}
void Playback()
{
    OriginalCameras core;CameraAssetLibrary library;
    library.Insert(CameraAsset::Decode(Fixture(0,true),Alias(0)));FrontendCameras frontend(core,library);
    frontend.Push(Alias(0));library.Clear();
    frontend.Advance(.025f,.025f);Near(cCameraManager::m_cameraPosition.x,.5f);Near(cCameraManager::m_fFOV,45);
    frontend.Advance(.1f,.1f);Near(cCameraManager::m_cameraPosition.x,.5f);Near(cCameraManager::m_fFOV,45);
    frontend.Advance(.025f,.025f);Near(cCameraManager::m_cameraPosition.x,1);Near(cCameraManager::m_fFOV,50);
}
void Transitions()
{
    OriginalCameras core;CameraAssetLibrary library;Fill(library,3);FrontendCameras frontend(core,library);
    frontend.Push(Alias(0));Pose(frontend,0);const auto* first=frontend.ActiveCamera();
    frontend.Push(Alias(1),CallbackA,1);const auto* second=frontend.ActiveCamera();
    Check(second!=first&&GetNextCamera()==first&&frontend.Size()==2,"Push transition ring identity differs");
    frontend.Advance(0,.25f);Near(cCameraManager::m_fFOV,40);frontend.Advance(0,0);
    Near(cCameraManager::m_cameraPosition.x,.20703125f);Near(cCameraManager::m_fFOV,40.103515625f);
    frontend.Advance(0,.75f);frontend.Advance(0,0);Check(messages.empty(),"Transition completed at >= instead of original >");
    frontend.Advance(0,.01f);Check(messages==std::vector<std::pair<unsigned,eCameraMessage>>{{1,eCM_COMPLETE}},"Transition callback differs");
    Pose(frontend,1);frontend.Advance(0,0);Check(messages.size()==1,"Completion callback repeated");
    frontend.Push(Alias(2),CallbackA,1,true);
    Check(frontend.Size()==2&&GetNextCamera()==first&&frontend.ActiveCamera()!=second,"Transition replacement did not delete only its old top");
    frontend.Advance(0,.25f);frontend.Pop(CallbackB,2);
    Check(frontend.ActiveCamera()==first&&frontend.Size()==1&&messages.back()==std::pair<unsigned,eCameraMessage>{1,eCM_ABORTED_BY_POP},"Transition pop lost original identity/callback");
    Near(cCameraManager::m_fTransitionTime,.75f);Near(cCameraManager::m_fTransitionSpeed,.5f);
    frontend.Advance(0,.51f);Check(messages.back()==std::pair<unsigned,eCameraMessage>{2,eCM_COMPLETE},"Pop transition completion differs");Pose(frontend,0);
    frontend.Push(Alias(1),CallbackA,1);frontend.Push(Alias(2),CallbackB);
    Check(messages.back()==std::pair<unsigned,eCameraMessage>{1,eCM_ABORTED_BY_PUSH},"Immediate push did not abort the old callback");
    frontend.Pop();frontend.Push(Alias(2),CallbackA,1);frontend.Pop(CallbackB);
    Check(messages.back()==std::pair<unsigned,eCameraMessage>{1,eCM_ABORTED_BY_POP},"Immediate pop did not abort the old callback");
}
void BorrowedBase()
{
    OriginalCameras core;CameraPoseInput base;cCameraManager::PushCamera(&base);
    CameraAssetLibrary library;Fill(library,2);
    {FrontendCameras frontend(core,library);Reject([&]{frontend.Push(Alias(0),nullptr,0,true);});
     frontend.Push(Alias(0),nullptr,1);frontend.Pop(nullptr,1);
     Check(!frontend.ActiveCamera()&&cCameraManager::PeekCamera()==&base,"Frontend pop destroyed a borrowed base camera");
     frontend.Push(Alias(1));}
    Check(cCameraManager::PeekCamera()==&base,"Frontend destruction removed a borrowed camera");core.Advance(0,0);
}
void Mutation()
{
    OriginalCameras core;CameraAssetLibrary library;Fill(library,2);FrontendCameras frontend(core,library);
    frontend.Push(Alias(0));
    on_message=[&]{Reject([&]{frontend.Push(Alias(0));});Reject([&]{frontend.Pop();});Reject([&]{frontend.Release();});Reject([&]{frontend.Advance(0,0);});};
    frontend.Push(Alias(1),CallbackA,1);frontend.Advance(0,2);
    Check(!frontend.Failed()&&frontend.Size()==2,"Rejected callback mutation poisoned frontend ownership");
    frontend.Push(Alias(0),CallbackA,1);core.Advance(0,2); // Caller may service the shared core directly.
    on_message={};Check(!frontend.Failed()&&frontend.Size()==3,"Direct core callback bypassed frontend mutation guard");
}
void CallbackFailure(bool popping,bool advancing)
{
    OriginalCameras core;CameraAssetLibrary library;Fill(library,3);FrontendCameras frontend(core,library);
    frontend.Push(Alias(0));frontend.Push(Alias(1),CallbackA,1);
    const auto* old=cCameraManager::PeekCamera();const auto free=VirtualAllocator.TotalFreeMemory();
    on_message=[]{throw std::runtime_error("frontend callback failure");};
    if(advancing)Reject([&]{frontend.Advance(0,2);});
    else if(popping)Reject([&]{frontend.Pop();});
    else Reject([&]{frontend.Push(Alias(2),CallbackB,1,true);});
    on_message={};
    Check(frontend.Failed()&&cCameraManager::PeekCamera()==old&&VirtualAllocator.TotalFreeMemory()==free,"Failed callback lost stack/candidate ownership");
    Reject([&]{frontend.Push(Alias(2));});frontend.Release();
    Check(!cCameraManager::PeekCamera()&&!cCameraManager::m_pCallback,"Failed frontend cleanup left callback/stack pointers");
}
void SessionTeardown()
{
    OriginalCameras core;CameraAssetLibrary library;Fill(library,2);FrontendCameras frontend(core,library);
    frontend.Push(Alias(0));frontend.Push(Alias(1),CallbackA,1);
    const auto count=messages.size();core.Release();
    Check(messages.size()==count&&!cCameraManager::PeekCamera(),"Core teardown fired callback or retained embedded camera");
    frontend.Release();
}
void EmptyTransitionTeardown(bool explicit_release)
{
    OriginalCameras core;CameraPoseInput base;cCameraManager::PushCamera(&base);
    CameraAssetLibrary library;Fill(library,2);const auto count=messages.size();
    {
        FrontendCameras frontend(core,library);frontend.Push(Alias(0));frontend.Pop(CallbackA,1);
        Check(frontend.Size()==0&&cCameraManager::m_pCallback,"Last pop did not leave its original transition active");
        if(explicit_release)frontend.Release();
    }
    Check(!cCameraManager::m_pCallback&&cCameraManager::m_transition==eCT_NONE,"Empty owner teardown retained its transition");
    core.Advance(0,2);Check(messages.size()==count&&cCameraManager::PeekCamera()==&base,"Teardown called user callback or destroyed base");
}
void EmptyTransitionMutation()
{
    OriginalCameras core;CameraPoseInput base;cCameraManager::PushCamera(&base);
    CameraAssetLibrary library;Fill(library,2);FrontendCameras frontend(core,library);
    frontend.Push(Alias(0));frontend.Pop(CallbackA,1);
    on_message=[&]{Reject([&]{frontend.Release();});Reject([&]{frontend.Push(Alias(0));});};
    core.Advance(0,2);on_message={};
    Check(frontend.Size()==0&&!frontend.Failed(),"Empty frontend bypassed shared-core callback guard");
    frontend.Push(Alias(1));frontend.Release();
}
void SharedOwners(bool throw_callback)
{
    OriginalCameras core;CameraPoseInput base;cCameraManager::PushCamera(&base);
    CameraAssetLibrary library;Fill(library,2);
    auto first=std::make_unique<FrontendCameras>(core,library);FrontendCameras second(core,library);
    first->Push(Alias(0));first->Pop(CallbackA,1);const auto count=messages.size();
    if(throw_callback)on_message=[]{throw std::runtime_error("cross-owner callback failure");};
    if(throw_callback)
    {
        const auto free=VirtualAllocator.TotalFreeMemory();
        Reject([&]{second.Push(Alias(1),CallbackB,1);});on_message={};
        Check(second.Failed()&&!first->Failed()&&cCameraManager::PeekCamera()==&base&&VirtualAllocator.TotalFreeMemory()==free,
              "Cross-owner callback failure changed borrowed base/candidate ownership");
        first->Release();second.Release();
    }
    else
    {
        second.Push(Alias(1),CallbackB,1);
        Check(messages.size()==count+1&&messages.back()==std::pair<unsigned,eCameraMessage>{1,eCM_ABORTED_BY_PUSH},
              "Replacement transition notified the wrong frontend owner");
        first.reset(); // Empty old owner must not cancel the replacement owner's callback.
        core.Advance(0,2);
        Check(messages.size()==count+2&&messages.back()==std::pair<unsigned,eCameraMessage>{2,eCM_COMPLETE},
              "Old owner destruction cancelled the replacement transition");
    }
}
void DirectCoreFailure(bool empty)
{
    OriginalCameras core;CameraPoseInput base;cCameraManager::PushCamera(&base);
    CameraAssetLibrary library;Fill(library,2);FrontendCameras frontend(core,library);
    frontend.Push(Alias(0));
    if(empty)frontend.Pop(CallbackA,1);else frontend.Push(Alias(1),CallbackA,1);
    on_message=[]{throw std::runtime_error("direct core callback failure");};
    Reject([&]{core.Advance(0,2);});on_message={};
    Check(!frontend.Failed(),"Direct core servicing changed frontend-local operation state");
    frontend.Release();Check(cCameraManager::PeekCamera()==&base&&!cCameraManager::m_pCallback,"Poisoned core prevented frontend cleanup");
}
void ExternalStackMutation()
{
    OriginalCameras core;CameraAssetLibrary library;Fill(library,2);FrontendCameras frontend(core,library);
    frontend.Push(Alias(0));auto* detached=cCameraManager::PopCamera();
    Reject([&]{frontend.Advance(0,0);});Reject([&]{frontend.Push(Alias(1));});
    Check(detached&&!cCameraManager::PeekCamera(),"External pop unexpectedly destroyed its allocation");
    frontend.Release();
}
void SessionIdentity()
{
    OriginalCameras old;CameraAssetLibrary library;Fill(library,2);FrontendCameras frontend(old,library);
    frontend.Push(Alias(0));frontend.Push(Alias(1),CallbackA,1);old.Release();
    Check(!old.Active(),"Released original camera session remains active");
    OriginalCameras next;CameraPoseInput base,target;target.position.x=20;cCameraManager::PushCamera(&base);
    cCameraManager::PushCameraWithTransition(&target,1,eCT_EASE_IN,CallbackB,false);
    Check(next.Active()&&!old.Active(),"New session revived the old core identity");
    Reject([&]{FrontendCameras invalid(old,library);});
    Reject([&]{frontend.Push(Alias(0));});Reject([&]{frontend.Pop();});Reject([&]{frontend.Advance(0,0);});
    Reject([&]{frontend.ActiveCamera();});Reject([&]{frontend.Size();});
    Reject([&]{old.Advance(0,0);});Reject([&]{old.AttachFilters(base);});
    Reject([&]{old.Rumble();});Reject([&]{old.Noise();});
    bool rejected=false;std::thread other([&]{try{next.Active();}catch(const std::logic_error&){rejected=true;}});other.join();
    Check(rejected,"Active session query permitted wrong-thread access");
    const auto count=messages.size();frontend.Release();old.Release();
    Check(cCameraManager::m_pCallback==CallbackB&&cCameraManager::PeekCamera()==&target&&next.Active(),
          "Old frontend cleanup changed the new session stack/transition");
    next.Advance(0,2);Check(messages.size()==count+1&&messages.back()==std::pair<unsigned,eCameraMessage>{2,eCM_COMPLETE},
                          "New session callback did not survive old owner cleanup");
}
void AllocationFailures()
{
    unsigned failures=0,successes=0;
    for(long budget=0;budget<32;++budget)
    {
        OriginalCameras core;CameraAssetLibrary library;Fill(library,2);FrontendCameras frontend(core,library);
        frontend.Push(Alias(0));const auto* old=cCameraManager::PeekCamera();const auto free=VirtualAllocator.TotalFreeMemory();
        const auto name=Alias(1);bool failed=false;allocation_budget=budget;
        try{frontend.Push(name,nullptr,0,true);}catch(const std::bad_alloc&){failed=true;}
        allocation_budget=-1;
        if(failed){++failures;Check(cCameraManager::PeekCamera()==old&&VirtualAllocator.TotalFreeMemory()==free,"Allocation failure did not roll back replacement preparation");}
        else{++successes;Check(frontend.Size()==1&&frontend.ActiveAlias()==name,"Successful allocation sweep failed to replace");break;}
    }
    Check(failures>=2&&successes==1,"Frontend allocation sweep missed ownership stages");
    OriginalCameras core;CameraAssetLibrary library;Fill(library,2);FrontendCameras frontend(core,library);frontend.Push(Alias(0));
    const auto* old=cCameraManager::PeekCamera();auto* allocator=CurrentAllocator;std::vector<void*> blocks;
    for(;;){try{blocks.push_back(VirtualAllocator.Allocate(256*1024,32,false));}catch(const std::bad_alloc&){break;}}
    const auto remaining=VirtualAllocator.LargestFreeBlock();if(remaining>128)blocks.push_back(VirtualAllocator.Allocate(remaining-96,32,false));
    Reject<std::bad_alloc>([&]{frontend.Push(Alias(1),nullptr,0,true);});
    Check(frontend.Failed()&&cCameraManager::PeekCamera()==old&&CurrentAllocator==allocator,"Native allocation failure changed stack/allocator scope");
    for(auto* block:blocks)VirtualAllocator.Free(block);
}
}
int main()
{
    try
    {
        std::vector<std::uint64_t> mem1(1024*1024),mem2(1024*1024);
        StandardAllocator.Initialize(mem1.data(),mem1.size()*8);VirtualAllocator.Initialize(mem2.data(),mem2.size()*8);gMemoryInitialized=1;
        for(unsigned session=0;session<3;++session)
        {
            messages.clear();Selection();Validation();Playback();messages.clear();Transitions();BorrowedBase();Mutation();
            CallbackFailure(false,false);CallbackFailure(true,false);CallbackFailure(false,true);SessionTeardown();AllocationFailures();
            EmptyTransitionTeardown(false);EmptyTransitionTeardown(true);EmptyTransitionMutation();SharedOwners(false);SharedOwners(true);
            DirectCoreFailure(false);DirectCoreFailure(true);ExternalStackMutation();SessionIdentity();
            Check(StandardAllocator.TotalFreeMemory()==mem1.size()*8&&VirtualAllocator.TotalFreeMemory()==mem2.size()*8,"Frontend camera session leaked native arenas");
        }
        ResetStartupMemory();std::cout<<checks<<" frontend camera checks passed, all 37 aliases and both arenas recovered\n";return 0;
    }
    catch(const std::exception& error){allocation_budget=-1;std::cerr<<"FAILED: "<<error.what()<<" (check "<<checks<<")\n";return 1;}
}
