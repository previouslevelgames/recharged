#include "runtime/shadows.h"
#include "runtime/views.h"
#include "runtime/materials.h"
#include "runtime/graphics_memory.h"
#include "runtime/graphics_state.h"
#include "runtime/startup.h"
#include "Game/GL/GLShadowBlendMeshWriter.h"
#include "Game/GL/glModelBuilder.h"
#include "Game/Render/ShadowVolume.h"
#include "NL/gl/glMemoryInit.h"
#include "NL/gl/glMemory.h"
#include "NL/gl/gl.h"
#include "NL/gl/glMatrix.h"
#include "NL/gl/glState.h"
#include "NL/gl/glTextureManager.h"
#include "NL/glx/glxTarget.h"
#include "NL/glx/GXShadowVolumeMaterialProgram.h"
#include <cstring>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <vector>
using namespace mscharged;
namespace
{
unsigned checks = 0;
void Check(bool value, const char* message) { ++checks; if (!value) throw std::runtime_error(message); }
template<class E, class F> void Reject(F f)
{
    try { f(); } catch (const E&) { ++checks; return; }
    throw std::runtime_error("Invalid shadow operation was accepted");
}
void Invalidate() {}
void Fill(GLShadowBlendMeshWriter& writer, unsigned n)
{
    for (unsigned i = 0; i < n; ++i)
    {
        writer.Vertex(float(i), float(i + 1), -.5f);
        writer.Texcoord(float(i), float(i + 2));
        writer.Colour({17, 34, 51, 68});
    }
}
void Run(unsigned texture_count)
{
    MaterialPrograms programs;
    Reject<std::logic_error>([]{GXShadowVolumeMaterialProgram::Instance->Activate(nullptr);});
    const auto owner_free1=StandardAllocator.TotalFreeMemory(), owner_free2=VirtualAllocator.TotalFreeMemory();
    OriginalViews views(640, 480);
    RLViewCamera camera;
    nlMatrix4 identity, projection; identity.SetIdentity();
    glMatrixPerspective(projection, 1.0f, 4.0f/3.0f, .1f, 100);
    camera.Set(identity, projection);
    const auto free1=StandardAllocator.TotalFreeMemory(), free2=VirtualAllocator.TotalFreeMemory();
    if (texture_count < 12)
    {
        Reject<std::length_error>([&] { ShadowLayers failed(camera); });
        Check(!gRootView.HasChildren(), "Partial shadow setup retained views");
        Check(glGetTextureManager()->mFreeIndices->mCount == texture_count, "Partial target setup retained indices");
        Check(StandardAllocator.TotalFreeMemory()==free1 && VirtualAllocator.TotalFreeMemory()==free2,
              "Partial shadow setup leaked arenas");
        return;
    }
    {
        GLTargetInfo info; info.width=info.height=72; info.format=GLTargetFormat_A8;
        auto foreign=glCreateTarget("shadow_05", &info);
        Reject<std::logic_error>([&] { ShadowLayers duplicate(camera); });
        glValidateTarget(foreign);
        glDestroyTarget(&foreign);
        Check(!gRootView.HasChildren(), "Conflicting target setup retained views");
    }
    ShadowLayers layers(camera);
    Check(!layers.Layer(eCLV_ShadowVolume).m_Enabled,"Original combined alpha-write policy changed");
    Reject<std::logic_error>([&] { ShadowLayers duplicate(camera); });
    Reject<std::out_of_range>([&] { layers.Layer(eCLV_Characters); });
    Reject<std::out_of_range>([&] { layers.Partition(11); });
    auto alias=layers.Partition(0).GetRenderPair();
    for (unsigned i=0; i<11; ++i)
    {
        const auto& partition=layers.Partition(i);
        const auto& rect=partition.m_Viewport;
        const auto pair=partition.GetRenderPair();
        Check(rect.x==(i%4)*144 && rect.y==(i/4)*144 && rect.width==144 && rect.height==144,
              "Original shadow atlas rectangle changed");
        Check(pair.target->mWidth==72 && pair.target->mHeight==72 && pair.target->mFormat==GLTargetFormat_A8,
              "Original shadow target dimensions changed");
        Check(partition.m_Parent==&layers.Layer(eCLV_ShadowTexture) && partition.m_Target==0,
              "Partition ownership or initial scheduling changed");
        Check(layers.PartitionTexture(i)==pair.hash, "Partition texture hash mismatch");
    }
    ProjectedShadowParams p{};
    p.vLight={1, 2, 3, 0}; p.vPosition={0, 0, -3}; p.fRadius=.2f; p.fHeight=.5f;
    p.nVisibleInterval=2; p.nInvisibleInterval=5; p.nPartitionIndex=1;
    layers.PreparePartition(p);
    Check(layers.Partition(1).m_Target==GLViewTarget_Mode8, "Projection preparation did not schedule copy");
    const auto* projected=layers.Partition(1).m_Interface->GetProjectionMatrix();
    Check(std::abs(projected->m11-5)<.0001f, "Original light-camera orthographic scale changed");
    bool any_true=false, any_false=false;
    for(unsigned frame=0;frame<10;++frame)
    {
        const bool visible=layers.ShouldUpdate(p,camera,frame);
        Check(visible==((frame+1)%2==0), "Original visible shadow update staggering changed");
        any_true|=visible; any_false|=!visible;
    }
    Check(any_true && any_false,"Update schedule never advanced");
    p.vPosition.x=1000;
    for(unsigned frame=0;frame<10;++frame)
        Check(layers.ShouldUpdate(p,camera,frame)==((frame+1)%5==0),"Invisible shadow update interval changed");
    p.nVisibleInterval=0; Reject<std::invalid_argument>([&]{layers.PreparePartition(p);}); p.nVisibleInterval=2;
    p.fRadius=0; Reject<std::invalid_argument>([&]{layers.PreparePartition(p);}); p.fRadius=.2f;
    p.vLight={0,0,1,0}; Reject<std::invalid_argument>([&]{layers.PreparePartition(p);});
    layers.ResetPartitions();
    for(unsigned i=0;i<11;++i) Check(layers.Partition(i).m_Target==0,"Partition reset retained copy request");
    {
        GLShadowBlendMeshWriter writer;
        Reject<std::logic_error>([&]{writer.GetModel();});
        Reject<std::invalid_argument>([&]{writer.Begin(-1,0,nullptr);});
        Reject<std::invalid_argument>([&]{writer.Begin(65536,0,nullptr);});
        Reject<std::invalid_argument>([&]{writer.Begin(4,GLP_Num,nullptr);});
        Reject<std::invalid_argument>([&]{writer.Begin(4,0,glGetCurrentResourcePool());});
        writer.Begin(4,GLP_TriStrip,nullptr);
        Reject<std::invalid_argument>([&]{writer.Vertex(std::numeric_limits<float>::quiet_NaN(),0,0);});
        auto* model=writer.GetModel();
        auto* params=static_cast<GXShadowVolumeParameters*>(model->packets->materialParameters);
        Check(params && model->packets->numStreams==3 && !model->packets->displayList,"Procedural packet not initialized");
        Reject<std::logic_error>([&]{writer.Begin(4,GLP_TriStrip,nullptr);});
        Fill(writer,3); Reject<std::logic_error>([&]{writer.End();}); Fill(writer,1); writer.End();
        const unsigned char expected[]={17,34,51,68};
        Check(!std::memcmp(model->packets->streams[1].address,expected,4),"Native colour byte order changed");
        Check(model->packets->matrix==glGetCurrentMatrix(),"Procedural matrix handle was truncated");
        Reject<std::out_of_range>([&]{writer.Vertex(0,0,0);});
        Reject<std::logic_error>([&]{writer.End();});
        glplatFrameAllocNextFrame();
        Reject<std::logic_error>([&]{writer.GetModel();});
        Reject<std::bad_alloc>([&]{writer.Begin(65535,GLP_TriList,nullptr);});
        Reject<std::logic_error>([&]{writer.GetModel();});
        glplatFrameAllocNextFrame();
        writer.Begin(4,GLP_TriStrip,nullptr); Fill(writer,4); writer.End();
        glModel invalid{};
        Reject<std::invalid_argument>([&]{glCreateModel(&invalid,3,0,nullptr,3,0xbad);});
        Reject<std::invalid_argument>([&]{AttachShadowVolumeModels(writer.GetModel(),writer.GetModel(),&layers.Layer(eCLV_ShadowVolume),nullptr);});
    }
    {
        glplatFrameAllocNextFrame();
        GLShadowBlendMeshWriter writer;writer.Begin(4,GLP_TriStrip,nullptr);Fill(writer,4);writer.End();
        auto* source=writer.GetModel();u16 indices[]={0,1,2,3};source->packets->indexBuffer=indices;source->packets->numVertices=4;
        auto* parameters=static_cast<GXShadowVolumeParameters*>(source->packets->materialParameters);
        parameters->useFixedColour=1;
        const auto original_raster=source->packets->rasterState;
        const auto original_matrix=source->packets->matrix;
        auto& pool=*glGetCurrentResourcePool();const auto before=pool.GetFreeMemory();
        const auto marker=pool.MarkResource();
        auto* clone=glModelDupNoStreams(source,true,&pool);
        Check(clone!=source && clone->packets!=source->packets && clone->packets->materialParameters!=parameters,
              "Clone packets/material storage aliases the source");
        Check(clone->packets->streams==source->packets->streams && clone->packets->indexBuffer==indices,
              "No-stream clone failed to share geometry");
        static_cast<GXShadowVolumeParameters*>(clone->packets->materialParameters)->useFixedColour=0;
        clone->packets->rasterState=123;
        Check(parameters->useFixedColour==1 && source->packets->rasterState==original_raster,"Clone mutation changed source");
        auto* frame=glModelDupNoStreams(source,false,nullptr);
        Check(frame->packets!=source->packets && frame->packets->streams==source->packets->streams,"Frame clone lost native geometry");
        const auto unchanged=pool.GetFreeMemory();
        Reject<std::invalid_argument>([&]{glModelDupArrayNoStreams(source,0,true,&pool);});
        Reject<std::invalid_argument>([&]{glModelDupArrayNoStreams(source,4097,true,&pool);});
        Reject<std::invalid_argument>([&]{glModelDupNoStreams(source,false,&pool);});
        Reject<std::invalid_argument>([&]{glModelDupNoStreams(nullptr,true,&pool);});
        source->packets->skinnedVertices=1;
        Reject<std::invalid_argument>([&]{glModelDupNoStreams(source,true,&pool);});
        Reject<std::invalid_argument>([&]{StadiumShadowVolume invalid(*source);});
        source->packets->skinnedVertices=0;
        Check(pool.GetFreeMemory()==unchanged,"Invalid clone consumed destination memory");
        pool.ReleaseResource(marker);Check(pool.GetFreeMemory()==before,"Clone packet storage did not release");
        const auto mem1=StandardAllocator.TotalFreeMemory(),mem2=VirtualAllocator.TotalFreeMemory();
        {
            StadiumShadowVolume allocated(*source);
            allocated.Release();allocated.Release();
        }
        Check(StandardAllocator.TotalFreeMemory()==mem1 && VirtualAllocator.TotalFreeMemory()==mem2,
              "Stadium drawable teardown leaked its independent packet pool");
        {
            StadiumShadowVolume drawable(*source);
            drawable.Draw(identity);
            Reject<std::logic_error>([&]{drawable.Draw(identity);});
            Check(source->packets->rasterState==original_raster && source->packets->matrix==original_matrix,
                  "Stadium submission mutated source model");
            Check(layers.Layer(eCLV_ShadowVolume).m_Visible && layers.Layer(eCLV_ShadowVolumeBlend).m_Visible,
                  "Original stadium Draw did not enable both layers");
            // Discard pending CPU-only submissions before releasing their packets.
            glplatFrameAllocNextFrame();
            drawable.Release();drawable.Release();
            Reject<std::logic_error>([&]{drawable.Draw(identity);});
        }
        // First submission can grow the original view slot pools. They are
        // reclaimed with the graph and checked against owner_free below.
    }
    glSetCurrentMatrix(glGetIdentityMatrix());
    RenderShadowVolumeBlend(&layers.Layer(eCLV_ShadowVolumeBlend));
    Check(layers.Layer(eCLV_ShadowVolumeBlend).m_NativeFrame==glNativeFrameGeneration(),"Original blend writer did not submit");
    // Graph shutdown also destroys selected layer ownership while the owner object is still in scope.
    views.Release(); layers.Release();
    Reject<std::logic_error>([]{GXShadowVolumeMaterialProgram::Instance->Activate(nullptr);});
    Reject<std::invalid_argument>([&]{glValidateTarget(alias);});
    Check(!gRootView.HasChildren(),"Automatic shadow shutdown retained views");
    Check(StandardAllocator.TotalFreeMemory()==owner_free1 && VirtualAllocator.TotalFreeMemory()==owner_free2,
          "Shadow shutdown leaked persistent memory");
}
}
int main()
{
    try
    {
        std::vector<std::uint64_t> mem1(2*1024*1024),mem2(2*1024*1024);
        ResetStartupMemory();
        StandardAllocator.Initialize(mem1.data(),mem1.size()*8);
        VirtualAllocator.Initialize(mem2.data(),mem2.size()*8); gMemoryInitialized=1;
        const auto free1=StandardAllocator.TotalFreeMemory(),free2=VirtualAllocator.TotalFreeMemory();
        for(unsigned slots:{32u,8u,32u})
        {
            const GLMemoryRequirement requirements[]={{GLM_Header,65536},{GLM_VertexData,8192}};
            const GLMemoryConfig cfg{256*1024,1024*1024,requirements,2,slots};
            glInitMemory(&cfg); InitializeOriginalGraphicsState(); SetGraphicsCacheInvalidator(Invalidate);
            Run(slots); glShutdownMemory();
            Check(StandardAllocator.TotalFreeMemory()==free1 && VirtualAllocator.TotalFreeMemory()==free2,"Shadow restart leaked game arenas");
        }
        ResetStartupMemory();
        std::cout<<checks<<" original shadow layer, camera, mesh and ownership checks passed\n";
        return 0;
    }
    catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
