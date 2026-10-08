#include "platform/graphics_stats.h"
#include "runtime/effects_vertex_render.h"
#include "runtime/frames.h"
#include "runtime/views.h"
#include "runtime/materials.h"
#include "runtime/graphics_memory.h"
#include "runtime/graphics_state.h"
#include "runtime/startup.h"
#include "runtime/startup_files.h"
#include "Game/GraphicsMemoryStartup.h"
#include "NL/gl/gl.h"
#include "NL/gl/glMemoryInit.h"
#include "NL/gl/glMemory.h"
#include "NL/gl/glMatrix.h"
#include <aurora/aurora.h>
#include <aurora/event.h>
#include <aurora/gfx.h>
#include <dolphin/gx.h>
#include <dolphin/gx/GXAurora.h>
#include <dolphin/vi.h>
#include <SDL3/SDL.h>
#include <imgui.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
using namespace mscharged;
namespace
{
std::atomic_uint errors=0;unsigned checks=0,draws=0;
constexpr std::array<int,3> background{20,24,30},float_colour{100,100,25},constant_colour{40,40,120};
void Check(bool yes,const char* message){++checks;if(!yes)throw std::runtime_error(message);}
template<class F>void Reject(F call){++checks;try{call();}catch(const std::exception&){return;}throw std::runtime_error("Invalid effects GPU submission accepted");}
void Log(AuroraLogLevel level,const char*,const char* message,unsigned n){if(level>=LOG_ERROR)++errors;std::cerr.write(message,n);std::cerr<<'\n';}
void Drain(){AuroraGXSync();}void Invalidate(){GXInvalidateVtxCache();GXInvalidateTexAll();}
struct Session{bool live=false;~Session(){if(live){Drain();glShutdownMemory();ResetStartupFiles();ResetStartupMemory();aurora_shutdown();}}};
void Acquire(OriginalFrames& frames)
{
    const auto end=std::chrono::steady_clock::now()+std::chrono::seconds(10);
    for(;;){for(const auto* e=aurora_update();e->type!=AURORA_NONE;++e)Check(e->type!=AURORA_EXIT,"Effects GPU window closed");
        if(frames.Acquire())return;Check(std::chrono::steady_clock::now()<end,"Effects GPU acquisition timeout");SDL_Delay(1);}
}
resources::Texture Texture(unsigned id,std::array<unsigned char,4> colour)
{
    resources::Texture t;t.id=id;t.width=t.height=4;t.levels=1;t.game_format=3;t.gx_format=6;t.bits={8,8,8,0};t.pixels.resize(64);
    for(unsigned i=0;i<16;++i){t.pixels[i*2]=colour[3];t.pixels[i*2+1]=colour[0];t.pixels[i*2+32]=colour[1];t.pixels[i*2+33]=colour[2];}return t;
}
resources::EffectsGeometry Geometry()
{
    resources::EffectsGeometry result;
    for(unsigned i=0;i<2;++i)
    {
        resources::Packet p;p.primitive=0;p.material.program=i?0xee9d919d:0x19065bf6;p.material.textures[0]={100+i,3};
        p.material.specular_colour={.5,.25,1,1};p.raster=0xc0007;
        p.vertices={{{-.4f,-.4f,-.3f},{.125f,.125f}},{{.4f,-.4f,-.3f},{.125f,.125f}},
            {{.4f,.4f,-.3f},{.125f,.125f}},{{-.4f,.4f,-.3f},{.125f,.125f}}};
        for(auto& v:p.vertices)v.colour={128,255,128,255};p.indices={0,1,2,0,2,3};
        result.models.push_back({10+i,{p}});resources::EffectsVertexAnimation animation{10+i,3,4,12,1,{1},{}};
        for(unsigned frame=0;frame<3;++frame)for(auto& v:p.vertices){auto position=v.position;position[0]+=float(frame)-1;animation.positions.push_back(position);}
        result.animations.push_back(std::move(animation));
    }
    return result;
}
void Pixel(const ColourSamples& colours,unsigned sample,std::array<int,3> expected)
{
    for(unsigned i=0;i<3;++i)Check(std::abs(int(colours[sample][i])-expected[i])<=3,"Animated geometry GPU pixel oracle differs");
}
template<class Draw>
void Case(const char* name,EffectsVertexResources& resources,OriginalFrames& frames,AuroraFrames& backend,Draw draw,
    std::array<std::array<int,3>,3> expected)
{
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(15);unsigned quiet=0;
    try
    {
        for(unsigned frame=0;;++frame)
        {
            Check(std::chrono::steady_clock::now()<deadline,"Effects pipeline warmup timed out");
            Acquire(frames);glBeginFrame();GXSetPixelFmt(GX_PF_RGB8_Z24,GX_ZC_LINEAR);GXSetCopyClear({20,24,30,255},GX_MAX_Z24);
            draw();const bool sample=frame>=12&&quiet>=2;backend.read_colours=sample;
            glEndFrame();glSendFrame();resources.FinishFrame();draws+=aurora_get_stats()->drawCallCount;
            const auto pending=mscharged::platform::GetQueuedPipelineCount();quiet=pending?0:quiet+1;
            if(sample&&!pending)break;
        }
        for(unsigned x=0;x<3;++x)Pixel(backend.colours,3+x,expected[x]);
        Pixel(backend.colours,0,background);Pixel(backend.colours,8,background);
        std::cout<<name<<": ";for(unsigned x=3;x<6;++x){const auto& c=backend.colours[x];std::cout<<unsigned(c[0])<<','<<unsigned(c[1])<<','<<unsigned(c[2])<<' ';}std::cout<<'\n';
    }
    catch(...){frames.Cancel();resources.FinishFrame();throw;}
}
void Generated(GLView& view,OriginalFrames& frames,AuroraFrames& backend)
{
    const auto geometry=Geometry();const resources::TextureBundle textures{{Texture(100,{200,100,50,255}),Texture(101,{80,160,120,255})},{}};
    for(unsigned session=0;session<2;++session)
    {
        EffectsVertexResources resources(geometry,textures,Drain);nlMatrix4 identity;identity.SetIdentity();
        Reject([&]{SubmitEffectsVertex(resources,view,10,identity);});
        auto current=[&]{Check(SubmitEffectsVertex(resources,view,10,identity)==1,"Original effects packet was not attached");};
        Case("source frame0",resources,frames,backend,current,{float_colour,background,background});
        resources.Update(1.f/30);Check(resources.State(10).frame==1,"Original30Hz update did not select frame1");
        Case("source frame1",resources,frames,backend,current,{background,float_colour,background});
        resources.Update(1.f/30);Check(resources.State(10).frame==2,"Original30Hz update did not select frame2");
        Case("source frame2",resources,frames,backend,current,{background,background,float_colour});
        resources.Update(1.f/30);Check(resources.State(10).frame==0,"Original loop overshoot did not return to0");
        Case("loop restart",resources,frames,backend,current,{float_colour,background,background});
        Case("distinct same-frame clones",resources,frames,backend,[&]{SubmitEffectsVertex(resources,view,10,identity,0);SubmitEffectsVertex(resources,view,10,identity,2);},
            {float_colour,background,float_colour});
        resources.Configure(10,EffectsVertexMode::Hold);resources.Update(5);Check(resources.State(10).done,"Original hold completion absent");
        Case("hold last",resources,frames,backend,current,{background,background,float_colour});
        Case("constant material",resources,frames,backend,[&]{SubmitEffectsVertex(resources,view,11,identity,1);},
            {background,constant_colour,background});
        Case("original packet order",resources,frames,backend,[&]{SubmitEffectsVertex(resources,view,10,identity,1);SubmitEffectsVertex(resources,view,11,identity,1);},
            {background,constant_colour,background});
        auto translated=identity;translated.e[12]=1;
        Case("original model transform",resources,frames,backend,[&]{SubmitEffectsVertex(resources,view,10,translated,0);},
            {background,float_colour,background});
        Acquire(frames);glBeginFrame();auto malformed=identity;malformed.e[0]=NAN;Reject([&]{SubmitEffectsVertex(resources,view,10,malformed);});
        malformed=identity;malformed.e[15]=0;Reject([&]{SubmitEffectsVertex(resources,view,10,malformed);});
        Reject([&]{SubmitEffectsVertex(resources,view,10,identity,-1,0xffffffff);});
        SubmitEffectsVertex(resources,view,10,identity,0);Reject([&]{resources.Release();});frames.Cancel();resources.FinishFrame();
        resources.Reset(10);Case("after actual cancellation",resources,frames,backend,current,{float_colour,background,background});resources.Release();
    }
}
std::vector<std::uint8_t> Read(const char* path)
{std::ifstream f(path,std::ios::binary);Check(bool(f),"Owned effects file absent");return{std::istreambuf_iterator<char>(f),{}};}
void Owned(GLView& view,ViewMatrices& matrices,OriginalFrames& frames,AuroraFrames& backend,const char* geometry,const char* textures)
{
    const auto data=resources::ReadEffectsGeometry(Read(geometry));const auto texture_data=resources::ReadTextureBundle(Read(textures));
    EffectsVertexResources resources(data,texture_data,Drain);nlMatrix4 identity;identity.SetIdentity();unsigned coloured_models=0;
    try
    {
        for(const auto& animation:data.animations)
        {
            std::array<float,3> low{INFINITY,INFINITY,INFINITY},high{-INFINITY,-INFINITY,-INFINITY};
            for(const auto& p:animation.positions)for(unsigned i=0;i<3;++i){low[i]=std::min(low[i],p[i]);high[i]=std::max(high[i],p[i]);}
            const float radius=std::max({high[0]-low[0],high[1]-low[1],high[2]-low[2],.001f});
            const nlVector3 centre{(low[0]+high[0])*.5f,(low[1]+high[1])*.5f,(low[2]+high[2])*.5f};
            const nlVector3 eye{centre.x+radius,centre.y+radius,centre.z+radius},up{0,0,1};
            glMatrixLookAt(matrices.view,eye,centre,up);glMatrixOrthographicCentered(matrices.projection,radius*2,radius*1.5f,.001f,radius*6);
            unsigned visible=0,model_draws=0;const auto end=std::chrono::steady_clock::now()+std::chrono::seconds(15);unsigned quiet=0;
            resources.Reset(animation.model);
            for(unsigned n=0;n<120;++n)
            {
                Check(std::chrono::steady_clock::now()<end,"Owned effects render deadline exceeded");
                if(n)resources.Update(1.f/30);
                Acquire(frames);glBeginFrame();GXSetPixelFmt(GX_PF_RGB8_Z24,GX_ZC_LINEAR);GXSetCopyClear({20,24,30,255},GX_MAX_Z24);
                SubmitEffectsVertex(resources,view,animation.model,identity);
                backend.read_colours=n>=12&&quiet>=2;glEndFrame();glSendFrame();resources.FinishFrame();
                const auto calls=aurora_get_stats()->drawCallCount;model_draws+=calls;draws+=calls;
                const auto pending=mscharged::platform::GetQueuedPipelineCount();quiet=pending?0:quiet+1;
                if(backend.read_colours&&!pending)for(const auto& pixel:backend.colours)
                    if(std::abs(int(pixel[0])-20)>5||std::abs(int(pixel[1])-24)>5||std::abs(int(pixel[2])-30)>5)++visible;
            }
            Check(model_draws>0,"Owned effects model produced no real GX draws");if(visible)++coloured_models;
            std::cout<<"Owned effects "<<std::hex<<animation.model<<std::dec<<": "<<model_draws<<" draws, "<<visible<<" changed-grid samples, "<<animation.frames<<" authored frames\n";
        }
        Check(resources.Size()==11&&coloured_models>0,"Owned effects geometry has no qualified visible samples");
        std::cout<<"Owned visible models at sampled grid: "<<coloured_models<<"/11 (draw submission separately checked for all11)\n";
    }
    catch(...){frames.Cancel();resources.FinishFrame();throw;}
}
}
int main(int argc,char** argv)
{
    try
    {
        Check(argc==1||(argc==4&&std::string_view(argv[1])=="--owned"),"Use effects_vertex_pipeline_tests [--owned GEOMETRY TEXTURES]");
        const char* base=SDL_GetBasePath();Check(base,"Missing binary directory");const std::string path=std::string(base)+"effects-vertex-test-data";std::filesystem::create_directories(path);
        AuroraConfig cfg{};cfg.appName="Charged original vertex animation checks";cfg.userPath=cfg.cachePath=path.c_str();cfg.resourcesPath=base;
        cfg.desiredBackend=BACKEND_VULKAN;cfg.enableBackendValidation=true;cfg.windowWidth=640;cfg.windowHeight=480;cfg.windowPosX=cfg.windowPosY=-1;
        cfg.logLevel=LOG_WARNING;cfg.logCallback=Log;cfg.vsync=true;cfg.mem1Size=MEM1_DEFAULT_SIZE;cfg.mem2Size=64*1024*1024;
        Session session;const auto info=aurora_initialize(argc,argv,&cfg);session.live=true;Check(info.window&&info.backend==BACKEND_VULKAN,"Effects gate requires Vulkan");
        ImGui::GetIO().IniFilename=nullptr;ImGui::GetIO().LogFilename=nullptr;InitializeStartupOS();nlInitMemory();
        const auto free1=StandardAllocator.TotalFreeMemory(),free2=VirtualAllocator.TotalFreeMemory();
        glInitResourcePools();InitializeOriginalGraphicsMemory();InitializeOriginalGraphicsState();VIInit();VIConfigure(&GXNtsc480IntDf);
        alignas(32)std::array<unsigned char,65536> fifo{};GXInit(fifo.data(),fifo.size());AuroraSetViewportPolicy(AURORA_VIEWPORT_FIT);SetGraphicsCacheInvalidator(Invalidate);
        MaterialPrograms materials;OriginalViews views(640,480,Drain);ViewMatrices matrices;glMatrixOrthographicCentered(matrices.projection,4,3,0,1);
        auto* view=new(8,false)GLView(&matrices,{},GLViewSort_None);gRootView.AddChild(view);AuroraFrames backend;OriginalFrames frames(backend);
        Generated(*view,frames,backend);if(argc==4)Owned(*view,matrices,frames,backend,argv[2],argv[3]);
        Check(draws>=200,"Original effects packets did not produce real GX draw calls");glFinish();frames.Release();views.Release();materials.Release();glShutdownMemory();
        Check(StandardAllocator.TotalFreeMemory()==free1&&VirtualAllocator.TotalFreeMemory()==free2,"Effects GPU owner leaked native arenas");
        ResetStartupFiles();ResetStartupMemory();aurora_shutdown();session.live=false;Check(!errors,"Effects GPU validation emitted errors");
        std::cout<<checks<<" effects vertex GPU checks passed, "<<draws<<" draws\n";
    }
    catch(const std::exception& e){std::cerr<<"FAILED: "<<e.what()<<'\n';return 1;}
}
