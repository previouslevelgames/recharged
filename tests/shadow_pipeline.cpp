// Generated geometry exercises the original shadow volume material, layer ordering,
// A8 copies and blend mesh. No game assets or character-shadow implementation.
#include "runtime/shadows.h"
#include "runtime/views.h"
#include "runtime/materials.h"
#include "runtime/static_inventory.h"
#include "runtime/graphics_memory.h"
#include "runtime/graphics_state.h"
#include "runtime/gpu_readback.h"
#include "runtime/startup.h"
#include "runtime/startup_files.h"
#include "Game/GraphicsMemoryStartup.h"
#include "Game/GL/GLShadowBlendMeshWriter.h"
#include "Game/Render/ShadowVolume.h"
#include "NL/gl/gl.h"
#include "NL/gl/glMemoryInit.h"
#include "NL/gl/glMatrix.h"
#include "NL/gl/glState.h"
#include "NL/glx/GXShadowVolumeMaterialProgram.h"
#include "NL/glx/glxTarget.h"
#include <aurora/aurora.h>
#include <aurora/event.h>
#include <aurora/gfx.h>
#include <dolphin/gx.h>
#include <dolphin/gx/GXAurora.h>
#include <dolphin/vi.h>
#include <SDL3/SDL.h>
#include <imgui.h>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string_view>

using namespace mscharged;
namespace
{
std::atomic_uint errors = 0;
void Log(AuroraLogLevel level, const char*, const char* message, unsigned length)
{ if(level>=LOG_ERROR)++errors; std::cerr.write(message,length);std::cerr<<'\n'; }
void Check(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
void Drain(){AuroraGXSync();}
void Invalidate(){GXInvalidateVtxCache();GXInvalidateTexAll();}
struct Session
{
    bool live=false,gx=false;
    ~Session(){if(live){if(gx)Drain();glShutdownMemory();ResetStartupFiles();ResetStartupMemory();aurora_shutdown();}}
};
resources::Texture White()
{
    resources::Texture t; t.id=10;t.width=t.height=4;t.levels=1;t.game_format=3;t.gx_format=6;
    t.bits={8,8,8,0};t.pixels.assign(64,200);
    for(unsigned i=0;i<16;++i)t.pixels[i*2]=255;
    return t;
}
resources::StaticModel Ground()
{
    resources::Packet p;p.primitive=0;p.material.program=0x21db4385;p.material.textures[0]={10,3};p.raster=0xc0007;
    p.vertices={{{-1,-1,-.5f},{0,0}},{{1,-1,-.5f},{1,0}},{{1,1,-.5f},{1,1}},{{-1,1,-.5f},{0,1}}};
    p.indices={0,1,2,0,2,3};return{1,{p}};
}
resources::StaticModel ResourceVolume()
{
    resources::Packet p;p.primitive=0;p.material.program=0x386ecbdd;
    p.material.textures[0]={10,3};p.material.switches[0]=1;
    for(const auto& v:std::array<std::array<float,3>,8>{{{-.22f,-.22f,-.25f},{.22f,-.22f,-.25f},{.22f,.22f,-.25f},{-.22f,.22f,-.25f},
        {-.22f,-.22f,-.85f},{.22f,-.22f,-.85f},{.22f,.22f,-.85f},{-.22f,.22f,-.85f}}})
        p.vertices.push_back({v,{0,0}});
    p.indices={0,1,2,0,2,3,4,6,5,4,7,6,0,5,1,0,4,5,3,6,7,3,2,6,0,7,4,0,3,7,1,6,2,1,5,6};
    return {2,{p}};
}
void Vertex(GLShadowBlendMeshWriter& writer,float x,float y,float z,float u=0,float v=0)
{writer.Vertex(x,y,z);writer.Colour({255,255,255,255});writer.Texcoord(u,v);}
glModel* Volume(GLShadowBlendMeshWriter& writer,float x,float front,float back,unsigned texture)
{
    writer.Begin(36,GLP_TriList,nullptr);
    const float left=x-.22f,right=x+.22f,bottom=-.22f,top=.22f;
    const std::array<std::array<float,3>,8> vertices={{{left,bottom,front},{right,bottom,front},{right,top,front},{left,top,front},
        {left,bottom,back},{right,bottom,back},{right,top,back},{left,top,back}}};
    // Closed outward volume; GX's clockwise front-face convention determines
    // the additive/subtractive surfaces in AttachShadowVolumeModels.
    const unsigned indices[]={0,2,1,0,3,2,4,5,6,4,6,7,0,1,5,0,5,4,3,7,6,3,6,2,0,4,7,0,7,3,1,2,6,1,6,5};
    for(unsigned triangle=0;triangle<36;triangle+=3)
        for(unsigned corner:{0u,2u,1u})
        {const auto& p=vertices[indices[triangle+corner]];Vertex(writer,p[0],p[1],p[2]);}
    auto* model=writer.GetModel();
    new(model->packets->materialParameters)GXShadowVolumeParameters{glTextureBinding(texture,1,1),1};
    writer.End();return model;
}
glModel* Quad(GLShadowBlendMeshWriter& writer,float width,float height,unsigned texture,bool mask)
{
    writer.Begin(4,GLP_TriStrip,nullptr);
    Vertex(writer,width,0,0,1,0);Vertex(writer,0,0,0,0,0);Vertex(writer,width,height,0,1,1);Vertex(writer,0,height,0,0,1);
    auto* model=writer.GetModel();
    new(model->packets->materialParameters)GXShadowVolumeParameters{glTextureBinding(texture,1,1),mask?0:1};
    writer.End();return model;
}
struct FrameInput
{
    float x=0,front=-.25f,back=-.85f;
    unsigned volumes=1;
    int fill_partition=-1,sample_partition=-1;
    StadiumShadowVolume* resource=nullptr;
};
bool Events()
{
    for(const auto* event=aurora_update();event->type!=AURORA_NONE;++event)
        if(event->type==AURORA_EXIT)return false;
    return true;
}
void Draw(ShadowLayers& layers,glModel& ground,const FrameInput& input)
{
    glplatFrameAllocNextFrame();
    GXSetPixelFmt(GX_PF_RGBA6_Z24,GX_ZC_LINEAR);
    GXSetCopyClear({200,200,200,0},GX_MAX_Z24);
    layers.ResetPartitions();
    glSetCurrentMatrix(glGetIdentityMatrix());
    glModelSetMatrix(&ground,glGetIdentityMatrix());
    layers.Layer(eCLV_Shadowed).AttachModel(&ground,0);
    std::array<GLShadowBlendMeshWriter,2> writers;
    std::array<glModel,2> second;
    std::array<glModelPacket,2> second_packets;
    if(input.resource)
    {
        nlMatrix4 world;world.SetIdentity();world.m41=input.x;
        input.resource->Draw(world);
    }
    for(unsigned i=0;i<(input.resource?0:input.volumes);++i)
    {
        auto* first=Volume(writers[i],input.x,input.front,input.back,glGetTexture("target/shadowvolume"));
        second_packets[i]=*first->packets;second[i]=*first;second[i].packets=&second_packets[i];
        AttachShadowVolumeModels(first,&second[i],&layers.Layer(eCLV_ShadowVolume),nullptr);
    }
    GLShadowBlendMeshWriter partition_writer,blend_writer;
    if(input.fill_partition>=0)
    {
        auto& partition=layers.Partition(input.fill_partition);
        nlMatrix4 identity,projection;identity.SetIdentity();glMatrixOrthographic(projection,2,2);
        sShadowPartitionCameras[input.fill_partition].Set(identity,projection);
        partition.m_Target=GLViewTarget_Mode8;
        glSetDefaultState(false);glSetRasterState(GLS_DepthTest,0);glSetRasterState(GLS_DepthWrite,0);
        glSetRasterState(GLS_Culling,0);glSetRasterState(GLS_ColourWrite,2);glSetRasterState(GLS_AlphaBlend,0);
        auto* mesh=Quad(partition_writer,2,2,glGetTexture("target/shadowvolume"),false);
        glModelSetRasterState(mesh,glHandleizeRasterState());partition.AttachModel(mesh,0);
    }
    if(input.sample_partition<0)
        RenderShadowVolumeBlend(&layers.Layer(eCLV_ShadowVolumeBlend));
    else
    {
        glSetDefaultState(false);glSetRasterState(GLS_DepthTest,0);glSetRasterState(GLS_DepthWrite,0);
        glSetRasterState(GLS_Culling,0);glSetRasterState(GLS_ColourWrite,1);glSetRasterState(GLS_AlphaBlend,1);
        auto* mesh=Quad(blend_writer,640,480,layers.PartitionTexture(input.sample_partition),true);
        glModelSetRasterState(mesh,glHandleizeRasterState());layers.Layer(eCLV_ShadowVolumeBlend).AttachModel(mesh,0);
    }
    RenderOriginalViews(0,{});GXDrawDone();
}
void PixelCase(const char* name,ShadowLayers& layers,glModel& ground,const FrameInput& input,unsigned shadow_mask)
{
    ColourSamples pixels{};
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(15);
    for(unsigned frame=0;frame<20;)
    {
        Check(std::chrono::steady_clock::now()<deadline,"Shadow pixel deadline exceeded");
        Check(Events(),"Shadow test window closed");
        if(!aurora_begin_frame()){SDL_Delay(1);continue;}
        Draw(layers,ground,input);
        if(frame==19)pixels=EndFrameAndReadColours();else aurora_end_frame();
        ++frame;
    }
    std::cout<<name<<": middle RGB "<<int(pixels[3][0])<<','<<int(pixels[4][0])<<','<<int(pixels[5][0])<<'\n';
    for(unsigned i=0;i<9;++i)
    {
        const int expected=shadow_mask&(1u<<i)?78:200; // 200 * (1 - original 155/255 opacity).
        for(unsigned c=0;c<3;++c)
            Check(std::abs(int(pixels[i][c])-expected)<=4,"Original shadow coverage/blend pixel mismatch");
    }
}
}
int main(int argc,char** argv)
{
    try
    {
        const bool window=argc>1&&std::string_view(argv[1])=="--window";
        unsigned demo_frames=argc>2&&std::string_view(argv[1])=="--demo-frames"?unsigned(std::stoul(argv[2])):0;
        const auto directory=std::filesystem::path(SDL_GetBasePath())/"shadow-test-data";
        std::filesystem::create_directories(directory);const auto path=directory.string();
        AuroraConfig cfg{};cfg.appName="Charged original shadow pipeline";cfg.userPath=cfg.cachePath=path.c_str();
        cfg.resourcesPath=SDL_GetBasePath();cfg.desiredBackend=BACKEND_VULKAN;cfg.enableBackendValidation=true;
        cfg.windowWidth=640;cfg.windowHeight=480;cfg.windowPosX=cfg.windowPosY=-1;cfg.vsync=true;
        cfg.logLevel=LOG_WARNING;cfg.logCallback=Log;cfg.mem1Size=MEM1_DEFAULT_SIZE;cfg.mem2Size=64*1024*1024;
        Session session;
        const auto info=aurora_initialize(argc,argv,&cfg);session.live=true;
        Check(info.backend==BACKEND_VULKAN&&info.window,"Shadow checks require Vulkan");
        ImGui::GetIO().IniFilename=ImGui::GetIO().LogFilename=nullptr;
        InitializeStartupOS();nlInitMemory();
        const auto free1=StandardAllocator.TotalFreeMemory(),free2=VirtualAllocator.TotalFreeMemory();
        glInitResourcePools();InitializeOriginalGraphicsMemory();InitializeOriginalGraphicsState();
        VIInit();VIConfigure(&GXNtsc480IntDf);
        alignas(32)std::array<unsigned char,65536> fifo{};GXInit(fifo.data(),fifo.size());session.gx=true;
        AuroraSetViewportPolicy(AURORA_VIEWPORT_FIT);SetGraphicsCacheInvalidator(Invalidate);
        MaterialPrograms programs;
        StaticInventory inventory(*glGetCurrentResourcePool(),{Ground(),ResourceVolume()},{White()},Drain);
        auto* ground=inventory.Model(1);
        StadiumShadowVolume resource(*inventory.Model(2));
        OriginalViews views(640,480,Drain);
        glGetBackBufferTarget().target->mClearColour={200,200,200,0};
        RLViewCamera camera;nlMatrix4 identity,projection;identity.SetIdentity();glMatrixOrthographicCentered(projection,2,2,0,1);
        camera.Set(identity,projection);
        ShadowLayers shadows(camera);
        PixelCase("No volume",shadows,*ground,{.volumes=0},0);
        PixelCase("Volume at left",shadows,*ground,{.x=-.5f},1u<<3);
        PixelCase("Volume at centre",shadows,*ground,{},1u<<4);
        PixelCase("Volume at right",shadows,*ground,{.x=.5f},1u<<5);
        PixelCase("Two overlapping volumes",shadows,*ground,{.volumes=2},1u<<4);
        PixelCase("Volume before receiver cancels",shadows,*ground,{.front=-.1f,.back=-.3f},0);
        PixelCase("Volume behind receiver is occluded",shadows,*ground,{.front=-.7f,.back=-.9f},0);
        PixelCase("Half-size atlas copy",shadows,*ground,{.volumes=0,.fill_partition=0,.sample_partition=0},511);
        PixelCase("Neighbour atlas target stays empty",shadows,*ground,{.volumes=0,.sample_partition=1},0);
        PixelCase("Last atlas rectangle",shadows,*ground,{.volumes=0,.fill_partition=10,.sample_partition=10},511);
        PixelCase("Disabled partition retains prior texture",shadows,*ground,{.volumes=0,.sample_partition=0},511);
        PixelCase("Indexed stadium resource",shadows,*ground,{.resource=&resource},1u<<4);
        PixelCase("Cloned stadium resource moved left",shadows,*ground,{.x=-.5f,.resource=&resource},1u<<3);
        PixelCase("Cloned stadium resource moved right",shadows,*ground,{.x=.5f,.resource=&resource},1u<<5);
        Check(inventory.Model(2)->packets->rasterState==0 && inventory.Model(2)->packets->matrix==0,
              "Original inventory was modified by stadium drawable submission");
        std::cout<<"Shadow pixel checks complete; starting visualization\n"<<std::flush;
        unsigned frames=0;
        const auto start=std::chrono::steady_clock::now();
        while(window||frames<demo_frames)
        {
            if(!Events())break;
            if(!aurora_begin_frame()){SDL_Delay(1);continue;}
            const float time=std::chrono::duration<float>(std::chrono::steady_clock::now()-start).count();
            Draw(shadows,*ground,{.x=.6f*std::sin(time)});
            ImGui::SetNextWindowPos({12,12});ImGui::Begin("Original shadow pass",nullptr,ImGuiWindowFlags_AlwaysAutoResize);
            ImGui::TextUnformatted("Generated moving volume / original A8 shadow blend");
            ImGui::TextUnformatted("Original character geometry and game scenes remain pending.");ImGui::End();
            if(!window && frames+1==demo_frames) EndFrameAndReadColours();
            else aurora_end_frame();
            ++frames;
        }
        shadows.Release();views.Release();resource.Release();inventory.Release();programs.Release();glShutdownMemory();
        Check(StandardAllocator.TotalFreeMemory()==free1&&VirtualAllocator.TotalFreeMemory()==free2,"Shadow GPU run leaked game arenas");
        ResetStartupFiles();ResetStartupMemory();aurora_shutdown();session.live=false;
        Check(!errors,"Shadow GPU backend errors");
        std::cout<<"Original shadow pixels, atlas copies and full arena recovery passed; demo frames "<<frames<<'\n';
        return 0;
    }
    catch(const std::exception& error){std::cerr<<"FAILED: "<<error.what()<<'\n';return 1;}
}
