// Standalone static-world GPU qualification; no camera manager or frame scheduler.
#include "world_fixture.h"
#include "runtime/views.h"
#include "runtime/materials.h"
#include "runtime/graphics_memory.h"
#include "runtime/graphics_state.h"
#include "runtime/gpu_readback.h"
#include "runtime/startup.h"
#include "runtime/startup_files.h"
#include "Game/GraphicsMemoryStartup.h"
#include "NL/gl/glMemory.h"
#include "NL/gl/glMemoryInit.h"
#include "NL/gl/glMatrix.h"
#include "NL/glx/glxTarget.h"
#include <aurora/aurora.h>
#include <aurora/event.h>
#include <aurora/gfx.h>
#include <dolphin/gx.h>
#include <dolphin/gx/GXAurora.h>
#include <dolphin/vi.h>
#include <SDL3/SDL.h>
#include <imgui.h>
#include <atomic>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <string_view>
#include "world_owned.h"

using namespace mscharged;
using namespace world_fixture;
namespace
{
std::atomic_uint errors=0;
void Log(AuroraLogLevel level,const char*,const char* message,unsigned length)
{if(level>=LOG_ERROR)++errors;std::cerr.write(message,length);std::cerr<<'\n';}
void Check(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
void Drain(){AuroraGXSync();}
void Invalidate(){GXInvalidateVtxCache();GXInvalidateTexAll();}
struct Session
{
    bool live=false,gx=false;
    ~Session(){if(live){if(gx)Drain();glShutdownMemory();ResetStartupFiles();ResetStartupMemory();aurora_shutdown();}}
};
void Begin()
{
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(10);
    for(;;)
    {
        for(const auto* e=aurora_update();e->type!=AURORA_NONE;++e)Check(e->type!=AURORA_EXIT,"Window closed during world test");
        if(aurora_begin_frame())break;
        Check(std::chrono::steady_clock::now()<deadline,"Timed out acquiring world frame");SDL_Delay(1);
    }
    glplatFrameAllocNextFrame();GXSetPixelFmt(GX_PF_RGBA6_Z24,GX_ZC_LINEAR);
    GXSetCopyClear({16,16,16,255},GX_MAX_Z24);
}
using RGB=std::array<unsigned char,3>;
using Grid=std::array<RGB,9>;
Grid Background(){Grid grid;grid.fill({16,16,16});return grid;}
void PixelCase(const char* name,GLView& opaque,GLView& alpha,
    const std::vector<resources::StaticWorldObject>& objects,const std::vector<resources::StaticModel>& models,
    const resources::TextureBundle& textures,const Grid& expected,std::size_t visible,std::size_t op,std::size_t ap)
{
    auto* previous=glGetCurrentResourcePool();
    StaticWorldObjects world(objects,models,textures,{256*1024,256*1024},Drain);
    glSetCurrentResourcePool(&world.Pool());
    // Reuse permanent instance matrices across multiple frame-arena generations.
    try
    {
        for(unsigned frame=0;frame<23;++frame)
        {
            Begin();
            const auto result=SubmitStaticWorld(world,opaque,alpha,StaticWorldFrustum(Planes()));
            Check(result.objects==objects.size()&&result.visible==visible&&result.opaque_packets==op&&result.alpha_packets==ap,"World GPU submission counts");
            RenderOriginalViews(0,{});GXDrawDone();
            if(frame<20){aurora_end_frame();continue;}
            const auto pixels=EndFrameAndReadColours();
            for(unsigned i=0;i<9;++i)for(unsigned c=0;c<3;++c)
                if(std::abs(int(pixels[i][c])-expected[i][c])>4)
                {
                    std::cerr<<name<<" frame "<<frame<<" sample "<<i<<" channel "<<c<<": "<<unsigned(pixels[i][c])<<" expected "<<unsigned(expected[i][c])<<'\n';
                    throw std::runtime_error("World pixel mismatch");
                }
        }
    }
    catch(...){opaque.ResetPackets();alpha.ResetPackets();glSetCurrentResourcePool(previous);throw;}
    glSetCurrentResourcePool(previous);world.Release();
    std::cout<<name<<": three frames, nine pixel samples per frame\n";
}
void Synthetic(GLView& opaque,GLView& alpha)
{
    const std::vector models{Quad(1,10),Quad(2,12),Quad(3,11,1)};
    const resources::TextureBundle textures{{Texture(10,{255,0,0,255}),Texture(11,{0,255,0,128}),Texture(12,{0,0,255,255})},{}};
    auto grid=Background();grid[3]=grid[5]={255,0,0};
    PixelCase("Shared model at two authored positions",opaque,alpha,{Object(1,1,-.5f),Object(2,1,.5f)},models,textures,grid,2,2,0);
    auto transformed=Object(1,1,.5f,.5f);transformed.transform[0]=0;transformed.transform[1]=-.75f;
    transformed.transform[4]=1.25f;transformed.transform[5]=0;
    grid=Background();grid[2]={255,0,0};
    PixelCase("Authored rotation and unequal scale",opaque,alpha,{transformed},models,textures,grid,1,1,0);
    grid=Background();
    PixelCase("Sphere outside frustum",opaque,alpha,{Object(1,1,3)},models,textures,grid,0,0,0);
    auto box=Object(1,1);box.type=0x10002;box.bounds_min[0]=2;box.bounds_max[0]=3;
    PixelCase("Stadium uses authored box",opaque,alpha,{box},models,textures,grid,0,0,0);
    box.bounds_min[0]=-.2f;box.bounds_max[0]=.2f;grid[4]={255,0,0};
    PixelCase("Visible stadium box",opaque,alpha,{box},models,textures,grid,1,1,0);
    grid[4]={0,0,255};
    PixelCase("Depth with foreground submitted first",opaque,alpha,{Object(1,2,0,0,-.25f),Object(2,1,0,0,-.75f)},models,textures,grid,2,2,0);
    PixelCase("Depth with foreground submitted last",opaque,alpha,{Object(1,1,0,0,-.75f),Object(2,2,0,0,-.25f)},models,textures,grid,2,2,0);
    grid[4]={127,128,0};
    PixelCase("Alpha over opaque",opaque,alpha,{Object(1,3,0,0,-.25f),Object(2,1,0,0,-.75f)},models,textures,grid,2,1,1);
    grid[4]={255,0,0};
    PixelCase("Opaque occludes alpha behind it",opaque,alpha,{Object(1,3,0,0,-.75f),Object(2,1,0,0,-.25f)},models,textures,grid,2,1,1);
    auto mixed=Quad(4,10);mixed.packets.push_back(Quad(4,11,1).packets[0]);
    for(auto& v:mixed.packets[0].vertices)v.position[0]-=.5f;
    for(auto& v:mixed.packets[1].vertices)v.position[0]+=.5f;
    auto wide=Object(1,4);wide.radius=.8f;
    grid=Background();grid[3]={255,0,0};grid[5]={0,128,127};
    PixelCase("Mixed packets route separately",opaque,alpha,{wide,Object(2,2,.5f,0,-.75f)},{mixed,models[1]},textures,grid,2,2,1);
}
void CameraCulling(ViewMatrices& matrices, GLView& opaque, GLView& alpha)
{
    struct Pose { nlVector3 eye, right, up, back; };
    for (bool perspective : {false, true})
        for (const auto& pose : {Pose{{2,3,5},{1,0,0},{0,1,0},{0,0,1}},
             Pose{{2,3,5},{0,0,-1},{0,1,0},{1,0,0}},
             Pose{{2,3,5},{1,0,0},{0,0,1},{0,-1,0}}})
        {
            auto point = [&](float x, float y, float depth) {
                return nlVector3{pose.eye.x + x*pose.right.x + y*pose.up.x - depth*pose.back.x,
                    pose.eye.y + x*pose.right.y + y*pose.up.y - depth*pose.back.y,
                    pose.eye.z + x*pose.right.z + y*pose.up.z - depth*pose.back.z};
            };
            glMatrixLookAt(matrices.view,pose.eye,point(0,0,1),pose.up);
            if (perspective) glMatrixPerspective(matrices.projection,3.1415927f/2,1,1,9);
            else glMatrixOrthographicCentered(matrices.projection,2,2,1,9);
            const auto frustum=StaticWorldFrustum::FromCamera(matrices.view,matrices.projection);
            std::vector<resources::StaticWorldObject> objects;
            auto add = [&](unsigned model, float x, float y, float depth) {
                const auto p=point(x,y,depth);
                auto o=Object(unsigned(objects.size()+1),model,p.x,p.y,p.z);
                o.transform={pose.right.x,pose.right.y,pose.right.z,0,
                    pose.up.x,pose.up.y,pose.up.z,0,pose.back.x,pose.back.y,pose.back.z,0,p.x,p.y,p.z,1};
                objects.push_back(o);
            };
            add(1,0,0,4);add(2,0,0,3.8f);
            // Six genuine offscreen meshes, including behind the camera, use
            // the same persistent world-space transforms as their bounds.
            add(1,-30,0,4);add(1,30,0,4);add(1,0,-30,4);add(1,0,30,4);
            add(1,0,0,-1);add(1,0,0,20);
            auto* previous=glGetCurrentResourcePool();
            resources::TextureBundle textures{{Texture(10,{255,0,0,255}),Texture(11,{0,255,0,128})},{}};
            StaticWorldObjects world(objects,{Quad(1,10),Quad(2,11,1)},textures,{256*1024,256*1024},Drain);
            glSetCurrentResourcePool(&world.Pool());
            try
            {
                std::array<ColourSamples,3> reference;
                unsigned reference_draws=0,culled_draws=0;
                for (bool cull : {false,true})
                    for (unsigned frame=0;frame<23;++frame)
                    {
                        Begin();const auto r=SubmitStaticWorld(world,opaque,alpha,frustum,cull);
                        Check(r.objects==8&&r.visible==(cull?2:8)&&r.opaque_packets==(cull?1:7)&&r.alpha_packets==1,
                            "Camera culling did not remove exactly six offscreen submissions");
                        RenderOriginalViews(0,{});GXDrawDone();
                        if(frame<20)aurora_end_frame();
                        else
                        {
                            const auto pixels=EndFrameAndReadColours();
                            Check(std::abs(int(pixels[4][0])-127)<=4 && std::abs(int(pixels[4][1])-128)<=4
                                && pixels[4][2]<=4,"Camera culling lost opaque/alpha visible geometry");
                            if(!cull)reference[frame-20]=pixels;
                            else for(unsigned i=0;i<9;++i)for(unsigned c=0;c<4;++c)
                                Check(pixels[i][c]==reference[frame-20][i][c],"Camera culling changed visible pixels");
                        }
                        (cull?culled_draws:reference_draws)+=aurora_get_stats()->drawCallCount;
                    }
                Check(culled_draws>0&&culled_draws<reference_draws,"Camera culling did not reduce GPU draw calls");
                std::cout<<(perspective?"Perspective":"Orthographic")<<" camera at "<<pose.eye.x<<','<<pose.eye.y<<','<<pose.eye.z
                    <<" back "<<pose.back.x<<','<<pose.back.y<<','<<pose.back.z
                    <<": 6/8 objects culled, "<<reference_draws<<" -> "<<culled_draws
                    <<" draw calls, three identical 3x3 pixel grids\n";
            }
            catch(...){opaque.ResetPackets();alpha.ResetPackets();glSetCurrentResourcePool(previous);throw;}
            glSetCurrentResourcePool(previous);world.Release();
        }
}

}
int main(int argc,char** argv)
{
    try
    {
        Check(argc==1||(argc==6&&std::string_view(argv[1])=="--owned"),
            "Usage: world_pipeline_tests [--owned RESIDENT.raw TEMPORARY.raw GLOBAL.rlt OBJECT_IDS.txt]");
        const auto directory=std::filesystem::path(SDL_GetBasePath())/"world-test-data";
        std::filesystem::create_directories(directory);const auto path=directory.string();
        AuroraConfig cfg{};cfg.appName="Charged static world qualification";cfg.userPath=cfg.cachePath=path.c_str();
        cfg.resourcesPath=SDL_GetBasePath();cfg.desiredBackend=BACKEND_VULKAN;cfg.enableBackendValidation=true;
        cfg.windowWidth=640;cfg.windowHeight=480;cfg.windowPosX=cfg.windowPosY=-1;cfg.vsync=true;
        cfg.logLevel=LOG_WARNING;cfg.logCallback=Log;cfg.mem1Size=MEM1_DEFAULT_SIZE;cfg.mem2Size=64*1024*1024;
        Session session;const auto info=aurora_initialize(argc,argv,&cfg);session.live=true;
        Check(info.backend==BACKEND_VULKAN&&info.window,"World test requires Vulkan");
        ImGui::GetIO().IniFilename=ImGui::GetIO().LogFilename=nullptr;
        InitializeStartupOS();nlInitMemory();
        const auto free1=StandardAllocator.TotalFreeMemory(),free2=VirtualAllocator.TotalFreeMemory();
        glInitResourcePools();InitializeOriginalGraphicsMemory();InitializeOriginalGraphicsState();
        VIInit();VIConfigure(&GXNtsc480IntDf);
        alignas(32)std::array<unsigned char,65536> fifo{};GXInit(fifo.data(),fifo.size());session.gx=true;
        AuroraSetViewportPolicy(AURORA_VIEWPORT_FIT);SetGraphicsCacheInvalidator(Invalidate);
        {
            MaterialPrograms programs;OriginalViews views(640,480,Drain);ViewMatrices matrices;
            glMatrixOrthographicCentered(matrices.projection,2,2,0,1);
            GLView opaque(&matrices,{},GLViewSort_Texture),alpha(&matrices,{},GLViewSort_Texture);
            opaque.SetViewport(0,0,640,480);alpha.SetViewport(0,0,640,480);
            opaque.m_ClearColour=opaque.m_ClearDepth=true;
            gRootView.AddChild(&opaque);gRootView.AddChild(&alpha);
            glGetBackBufferTarget().target->mClearColour={16,16,16,255};
            if(argc==1){Synthetic(opaque,alpha);CameraCulling(matrices,opaque,alpha);}
            else world_owned::Run(argv+2,matrices,opaque,alpha,Begin,Drain);
        }
        glShutdownMemory();
        Check(StandardAllocator.TotalFreeMemory()==free1&&VirtualAllocator.TotalFreeMemory()==free2,"World GPU test leaked arenas");
        ResetStartupFiles();ResetStartupMemory();aurora_shutdown();session.live=false;
        Check(!errors,"World GPU backend errors");
        std::cout<<"Static world pixels and full arena recovery passed\n";return 0;
    }
    catch(const std::exception& e){std::cerr<<"FAILED: "<<e.what()<<'\n';return 1;}
}
