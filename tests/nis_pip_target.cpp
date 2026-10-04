#include "runtime/nis_pip_render.h"
#include "runtime/views.h"
#include "runtime/materials.h"
#include "runtime/graphics_memory.h"
#include "runtime/graphics_state.h"
#include "runtime/startup.h"
#include "NL/gl/gl.h"
#include "NL/gl/glMemory.h"
#include "NL/gl/glMemoryInit.h"
#include "NL/gl/glTextureManager.h"
#include "NL/glx/glxTarget.h"
#include "NL/glx/glxTexture.h"
#include <iostream>
#include <memory>
#include <stdexcept>
#include <vector>

using namespace mscharged;
namespace
{
unsigned checks=0;
void Check(bool valid,const char* message){++checks;if(!valid)throw std::runtime_error(message);}
template<class F>void Reject(F action)
{++checks;try{action();}catch(const std::exception&){return;}throw std::runtime_error("Invalid PIP target operation accepted");}
void Run()
{
    MaterialPrograms materials;
    Reject([]{NisPipTarget invalid;});
    {OriginalViews small(256,128);Reject([]{NisPipTarget invalid;});}
    OriginalViews views(640,480);
    const auto mem1=StandardAllocator.TotalFreeMemory(),mem2=VirtualAllocator.TotalFreeMemory();
    auto* selected=glGetCurrentResourcePool();
    {
        NisPipTarget target;const auto pair=target.Pair();
        Check(NisPipTarget::ViewportWidth()==512&&NisPipTarget::ViewportHeight()==256,"Original PIP viewport dimensions differ");
        Check(pair.target->mWidth==256&&pair.target->mHeight==128&&pair.target->mFormat==GLTargetFormat_RGB565,"Original PIP target dimensions/format differ");
        Check(pair.target->mClearColourEnabled&&!pair.target->mClearDepthEnabled,"Original PIP target clear flags differ");
        Check(pair.target->mClearColour.c[0]==0&&pair.target->mClearColour.c[1]==0&&pair.target->mClearColour.c[2]==0
            &&pair.target->mClearColour.c[3]==255,"Original PIP clear colour differs");
        Check(pair.target->mTextureData&&glx_GetTex(target.Texture())==pair.target->mTexture,"PIP target has no real texture storage/registration");
        Check(target.Texture()==glHash("target/pip")&&glGetCurrentResourcePool()==selected,"PIP target changed pool or original name");
        Reject([]{NisPipTarget duplicate;});
        glNativeSetViewDispatch(true);Reject([&]{target.Release();});Reject([]{NisPipTarget invalid;});glNativeSetViewDispatch(false);
        bool rejected=false;std::thread worker([&]{try{target.Release();}catch(const std::logic_error&){rejected=true;}});worker.join();
        Check(rejected,"PIP target accepted wrong-thread release");
        target.Release();target.Release();Reject([&]{target.Pair();});Reject([&]{target.Texture();});
        Check(!glx_GetTex(glHash("target/pip")),"PIP release retained registered texture");
        Reject([&]{glValidateTarget(pair);});
    }
    Check(StandardAllocator.TotalFreeMemory()==mem1&&VirtualAllocator.TotalFreeMemory()==mem2,"PIP target leaked game memory");
    {
        // Exhaust actual registered texture slots. Construction must unwind its
        // independent target pool and leave both global registry and pool intact.
        std::vector<GLRenderPair> targets;
        GLTargetInfo info;info.width=info.height=8;info.format=GLTargetFormat_RGB565;
        for(unsigned i=0;i<32;++i)targets.push_back(glCreateTarget(("occupied/"+std::to_string(i)).c_str(),&info));
        const auto before1=StandardAllocator.TotalFreeMemory(),before2=VirtualAllocator.TotalFreeMemory();
        Reject([]{NisPipTarget exhausted;});
        Check(StandardAllocator.TotalFreeMemory()==before1&&VirtualAllocator.TotalFreeMemory()==before2
            &&glGetCurrentResourcePool()==selected&&!glx_GetTex(glHash("target/pip")),"Failed PIP construction leaked pool/registry state");
        for(auto& target:targets)glDestroyTarget(&target);
        NisPipTarget recovered;
    }
    Check(StandardAllocator.TotalFreeMemory()==mem1&&VirtualAllocator.TotalFreeMemory()==mem2,"PIP allocation retry leaked game memory");
    auto old=std::make_unique<NisPipTarget>();const auto old_pair=old->Pair();views.Release();
    OriginalViews replacement(640,480);NisPipTarget next;const auto next_pair=next.Pair();
    Check(old_pair.nativeGeneration!=next_pair.nativeGeneration,"PIP target generation survived session replacement");
    Reject([&]{old->Pair();});old.reset();
    Check(next.Pair().target==next_pair.target,"Old PIP owner destroyed a new session's target");
    next.Release();replacement.Release();materials.Release();
}
}
int main()
{
    try
    {
        std::vector<std::uint64_t> mem1(1024*1024),mem2(1024*1024);
        ResetStartupMemory();StandardAllocator.Initialize(mem1.data(),mem1.size()*8);VirtualAllocator.Initialize(mem2.data(),mem2.size()*8);gMemoryInitialized=1;
        for(unsigned repeat=0;repeat<3;++repeat)
        {
            const GLMemoryRequirement requirements[]={{GLM_Header,65536},{GLM_VertexData,4096}};
            const GLMemoryConfig config{65536,4096,requirements,2,32};
            glInitMemory(&config);InitializeOriginalGraphicsState();Run();glShutdownMemory();
            Check(StandardAllocator.TotalFreeMemory()==mem1.size()*8&&VirtualAllocator.TotalFreeMemory()==mem2.size()*8,"PIP target restart leaked game arenas");
        }
        ResetStartupMemory();std::cout<<checks<<" original PIP target registration, failure and lifecycle checks passed\n";return 0;
    }
    catch(const std::exception& error){std::cerr<<"FAILED: "<<error.what()<<" (check "<<checks<<")\n";return 1;}
}
