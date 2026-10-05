#include "runtime/effects_vertex.h"
#include "runtime/materials.h"
#include "runtime/frames.h"
#include "runtime/views.h"
#include "runtime/startup.h"
#include "runtime/graphics_state.h"
#include "runtime/graphics_memory.h"
#include "Game/GL/GLInventory.h"
#include "Game/GL/GLVertexAnim.h"
#include "NL/gl/gl.h"
#include "NL/gl/glMemory.h"
#include "NL/gl/glMemoryInit.h"
#include "NL/gl/glTextureManager.h"
#include "NL/glx/glxTexture.h"
#include <bit>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <source_location>
#include <thread>
using namespace mscharged;
namespace
{
unsigned checks=0;
void Check(bool value,const char* message){++checks;if(!value)throw std::runtime_error(message);}
template<class F>void Reject(F f,std::source_location at=std::source_location::current())
{++checks;try{f();}catch(const std::exception&){return;}throw std::runtime_error("Invalid effects operation accepted at "+std::to_string(at.line()));}
void Same(float a,float b){Check(std::bit_cast<unsigned>(a)==std::bit_cast<unsigned>(b),"Original float bits differ");}
void CpuInvalidate(){} // CPU frame fixture: no GPU references exist.
struct Backend:FrameBackend
{bool Acquire()override{return true;}void Render()override{}void Finish(bool)override{}void Drain()override{}void WaitIdle()override{}void Cancel()noexcept override{}};
void Begin(OriginalFrames& frames){Check(frames.Acquire(),"CPU frame acquisition failed");glBeginFrame();}
GLResourcePool* Resource(std::uint32_t id)
{
    auto* first=glGetResourcePools();auto* p=first;
    do{if(p->m_inventory->GetVertexAnim(id))return p;p=p->m_next;}while(p!=first);
    return nullptr;
}
resources::EffectsGeometry Geometry(unsigned packets=2)
{
    resources::EffectsGeometry g;resources::StaticModel m{0x10203040,{}};
    for(unsigned p=0;p<packets;++p)
    {
        resources::Packet packet;packet.primitive=0;packet.material.program=p%2?0xee9d919d:0x19065bf6;
        packet.material.textures[0].texture=0x12345678;packet.material.specular_colour={.25,.5,.75,1};
        for(unsigned v=0;v<3;++v){resources::Vertex vertex{};vertex.position={float(p),float(v),-5};vertex.uv={float(v)/2,float(p)};vertex.colour={11,22,33,44};packet.vertices.push_back(vertex);}
        packet.indices={2,0,1};m.packets.push_back(packet);
    }
    g.models.push_back(m);resources::EffectsVertexAnimation a{m.id,3,3*packets,12,1,{1},{}};
    for(unsigned f=0;f<a.frames;++f)for(unsigned v=0;v<a.vertices;++v)a.positions.push_back({float(100*f+v),float(v)-.25f,-float(f)-.5f});
    g.animations.push_back(a);return g;
}
resources::TextureBundle Textures()
{
    resources::Texture t;t.id=0x12345678;t.width=t.height=4;t.levels=1;t.game_format=3;t.gx_format=6;t.bits={8,8,8,8};t.pixels.resize(64,0xff);
    return {{t},{}};
}
void VerifyModel(glModel* sample,const resources::StaticModel& model,const resources::EffectsVertexAnimation& animation,unsigned frame)
{
    Check(sample&&sample->id==model.id&&sample->numPackets==model.packets.size(),"Actual model binding lost authored identity");
    std::size_t offset=frame*animation.vertices;
    for(unsigned p=0;p<sample->numPackets;++p)
    {
        auto& packet=sample->packets[p];const auto& input=model.packets[p];
        Check(packet.numUniqueVertices==input.vertices.size()&&packet.numVertices==input.indices.size(),"Frame clone changed vertex/index count");
        for(unsigned i=0;i<input.indices.size();++i)Check(packet.indexBuffer[i]==input.indices[i],"Frame clone changed authored index");
        auto* positions=glFindModelStream(&packet,1);auto* uvs=glFindModelStream(&packet,4);
        Check(positions&&positions->stride==12&&uvs&&uvs->stride==8,"Animated position/static UV stream layout differs");
        const auto* values=static_cast<const float*>(positions->address);const auto* uv=static_cast<const float*>(uvs->address);
        for(unsigned v=0;v<input.vertices.size();++v)
        {
            for(unsigned axis=0;axis<3;++axis)Same(values[v*3+axis],animation.positions[offset+v][axis]);
            Same(uv[v*2],input.vertices[v].uv[0]);Same(uv[v*2+1],input.vertices[v].uv[1]);
        }
        offset+=input.vertices.size();
        auto* binding=static_cast<glTextureBinding*>(packet.materialParameters);
        Check(binding->texture==input.material.textures[0].texture&&glGetTextureManager()->GetTexture(binding),"Frame clone lost real texture resolution");
    }
}
void Generated(OriginalFrames& frames)
{
    auto g=Geometry();auto textures=Textures();const auto a=StandardAllocator.TotalFreeMemory(),b=VirtualAllocator.TotalFreeMemory();
    const auto slots=glGetTextureManager()->mFreeIndices->mCount;auto* current=glGetCurrentResourcePool();
    unsigned drains=0;bool fail_drain=false;EffectsVertexResources* active=nullptr;
    auto drain=[&]{++drains;if(active){Reject([&]{active->Release();});Reject([&]{active->Update(0);});}if(fail_drain)throw std::runtime_error("Controlled drain failure");};
    auto recovered=[&]{Check(StandardAllocator.TotalFreeMemory()==a&&VirtualAllocator.TotalFreeMemory()==b&&glGetTextureManager()->mFreeIndices->mCount==slots&&glGetCurrentResourcePool()==current,"Effects resources leaked arenas/slots/current pool");};
    Reject([&]{EffectsVertexResources value(g,textures,{});});recovered();
    for(unsigned fault=0;fault<10;++fault)
    {
        auto bad=g;auto tex=textures;
        switch(fault)
        {
        case 0:bad.animations[0].frames=0;break;case 1:bad.animations[0].positions.pop_back();break;
        case 2:bad.animations[0].streams={2};break;case 3:bad.animations[0].positions[0][1]=NAN;break;
        case 4:bad.models[0].packets[0].indices[0]=3;break;case 5:bad.animations[0].model=123;break;
        case 6:tex.textures.clear();break;case 7:tex.textures[0].pixels.pop_back();break;
        case 8:bad.animations.push_back(bad.animations[0]);break;case 9:tex.textures[0].gx_format=0;break;
        }
        Reject([&]{EffectsVertexResources value(bad,tex,drain);});recovered();
    }
    for(auto memory:{EffectsVertexMemory{32,4096,4096},EffectsVertexMemory{4096,32,32}})
    {Reject([&]{EffectsVertexResources value(g,textures,drain,memory);});recovered();}
    {
        EffectsVertexResources value(g,textures,drain);active=&value;
        Check(value.Active()&&value.Size()==1,"Effects inventory was not published");
        auto* pool=Resource(g.models[0].id);Check(pool&&pool->m_inventory->m_nLevel==1,"Effects animation not in actual marked GL inventory");
        auto* animation=pool->m_inventory->GetVertexAnim(g.models[0].id);auto* base=pool->m_inventory->GetModel(g.models[0].id);
        Check(animation&&animation->m_pModel==base&&animation->m_nNumAnimatedStreams==1&&animation->m_pAnimatedStreamIDs[0]==1,"Original constructor/Add/Get binding differs");
        Reject([&]{EffectsVertexResources duplicate(g,textures,drain);});Reject([&]{value.State(999);});
        Reject([&]{value.Configure(g.models[0].id,EffectsVertexMode(99));});
        for(float invalid:{-1.f,INFINITY,NAN}){Reject([&]{value.Update(invalid);});Reject([&]{value.Configure(g.models[0].id,EffectsVertexMode::Loop,invalid);});}
        std::thread wrong([&]{Reject([&]{value.Update(0);});Reject([&]{value.Release();});});wrong.join();
        for(float delta:{0.f,std::nextafter(1.f/30,0.f),1.f/30,std::nextafter(1.f/30,1.f),.25f})
        {
            value.Reset(g.models[0].id);float expected=30.f*(1.f*delta);if(expected>=3)expected=0;
            value.Update(delta);Same(value.State(g.models[0].id).frame,expected);
        }
        value.Configure(g.models[0].id,EffectsVertexMode::Hold,2);value.Reset(g.models[0].id);value.Update(.1f);
        Check(value.State(g.models[0].id).done,"Hold endpoint did not finish");Same(value.State(g.models[0].id).frame,2);
        value.Configure(g.models[0].id,EffectsVertexMode::Loop);value.Update(1);Same(value.State(g.models[0].id).frame,2);
        value.Reset(g.models[0].id);Check(!value.State(g.models[0].id).done,"Reset did not clear original done state");
        value.Configure(g.models[0].id,EffectsVertexMode::Loop,std::numeric_limits<float>::max());Reject([&]{value.Update(1);});Same(value.State(g.models[0].id).frame,0);
        value.Configure(g.models[0].id,EffectsVertexMode::Loop,0);value.Update(std::numeric_limits<float>::max());Same(value.State(g.models[0].id).frame,0);
        value.Configure(g.models[0].id,EffectsVertexMode::Loop);value.Update(1.f/30);Same(value.State(g.models[0].id).frame,1);
        const auto* base_streams=base->packets[0].streams;const auto* base_position=base_streams[0].address;
        Begin(frames);auto* first=value.Model(g.models[0].id,0);auto* last=value.Model(g.models[0].id,2);auto* selected=value.Model(g.models[0].id);
        VerifyModel(first,g.models[0],g.animations[0],0);VerifyModel(last,g.models[0],g.animations[0],2);VerifyModel(selected,g.models[0],g.animations[0],1);
        Check(first->packets!=last->packets&&first->packets[0].streams!=last->packets[0].streams
            &&first->packets[0].streams!=base_streams&&base->packets[0].streams==base_streams&&base_streams[0].address==base_position,
            "Animated clone rebound source or another clone's shared stream descriptors");
        Check(first->packets[0].indexBuffer==base->packets[0].indexBuffer&&first->packets[0].materialParameters!=base->packets[0].materialParameters,
            "Original index borrowing/material parameter copy changed");
        Reject([&]{value.Model(g.models[0].id,3);});Reject([&]{value.Model(g.models[0].id,-2);});
        Reject([&]{value.Release();});Reject([&]{value.Update(0);});Reject([&]{value.FinishFrame();});
        glEndFrame();glSendFrame();fail_drain=true;Reject([&]{value.FinishFrame();});Reject([&]{value.Release();});
        fail_drain=false;value.FinishFrame();value.FinishFrame();
        Begin(frames);glFrameAlloc(262144-64,GLM_Header);Reject([&]{value.Model(g.models[0].id);});
        frames.Cancel();value.FinishFrame();Begin(frames);VerifyModel(value.Model(g.models[0].id),g.models[0],g.animations[0],1);frames.Cancel();value.FinishFrame();
        const auto mark=pool->MarkResource();Reject([&]{value.Update(0);});Reject([&]{value.Release();});pool->ReleaseResource(mark);
        fail_drain=true;Reject([&]{value.Release();});Check(value.Active(),"Failed drain retired visible resources");
        fail_drain=false;value.Release();value.Release();Check(!value.Active(),"Effects owner remained active after release");Reject([&]{value.State(g.models[0].id);});active=nullptr;
    }
    recovered();Check(drains>=4,"Effects lifetime drain callbacks were skipped");
}
struct Hash
{
    std::uint64_t value=0xcbf29ce484222325;
    void Byte(std::uint8_t b){value=(value^b)*0x100000001b3;}
    void Float(float f){auto v=std::bit_cast<std::uint32_t>(f);for(unsigned i=0;i<4;++i)Byte(v>>(24-8*i));}
};
std::vector<std::uint8_t> Read(const char* filename)
{std::ifstream f(filename,std::ios::binary);Check(bool(f),"Owned asset could not be read");return{std::istreambuf_iterator<char>(f),{}};}
void Owned(OriginalFrames& frames,const char* geometry,const char* textures,const char* oracle_path)
{
    auto data=resources::ReadEffectsGeometry(Read(geometry));auto texture_data=resources::ReadTextureBundle(Read(textures));
    EffectsVertexResources value(data,texture_data,[]{ });std::ifstream oracle(oracle_path);Check(bool(oracle),"Owned raw-byte oracle absent");
    for(unsigned i=0;i<data.models.size();++i)
    {
        std::uint32_t id,program,texture;std::size_t frame_count,vertices,metadata_size;std::uint64_t positions,metadata,base,uv,indices;
        Check(bool(oracle>>std::hex>>id>>program>>texture>>positions>>metadata>>base>>uv>>indices>>std::dec>>frame_count>>vertices>>metadata_size),"Owned oracle record absent");
        const auto& model=data.models[i];const auto& animation=data.animations[i];
        Check(id==model.id&&id==animation.model&&frame_count==animation.frames&&vertices==animation.vertices,"Owned model/animation association differs");
        Hash hash,index_hash,uv_hash;for(unsigned f=0;f<animation.frames;++f)
        {
            Begin(frames);auto* sample=value.Model(id,f);VerifyModel(sample,model,animation,f);
            for(unsigned p=0;p<sample->numPackets;++p)
            {
                auto& packet=sample->packets[p];const auto* pos=static_cast<const float*>(glFindModelStream(&packet,1)->address);
                for(unsigned v=0;v<packet.numUniqueVertices*3;++v)hash.Float(pos[v]);
                if(f==0)
                {
                    for(unsigned n=0;n<packet.numVertices;++n){index_hash.Byte(packet.indexBuffer[n]>>8);index_hash.Byte(packet.indexBuffer[n]);}
                    const auto* coords=static_cast<const float*>(glFindModelStream(&packet,4)->address);
                    for(unsigned n=0;n<packet.numUniqueVertices*2;++n)uv_hash.Float(coords[n]);
                }
            }
            frames.Cancel();value.FinishFrame();
        }
        Check(hash.value==positions&&index_hash.value==indices&&uv_hash.value==uv,"Actual original GetModel position/index/UV fingerprints differ from independent raw source");
        value.Configure(id,EffectsVertexMode::Hold);value.Reset(id);value.Update(float(animation.frames)/30+1);
        Check(value.State(id).done,"Owned hold animation did not finish");Same(value.State(id).frame,animation.frames-1);
    }
    std::string extra;Check(!(oracle>>extra)&&value.Size()==11,"Owned animation/oracle count differs");
    std::cout<<"All 11 owned GL vertex animations bound and sampled through original GetModel\n";
}
}
int main(int argc,char** argv)
{
    try
    {
        Check(argc==1||(argc==5&&std::string_view(argv[1])=="--owned"),"Use effects_vertex_tests [--owned GEOMETRY TEXTURES ORACLE]");
        std::vector<std::uint64_t> standard(4*1024*1024),virtual_arena(8*1024*1024);ResetStartupMemory();
        StandardAllocator.Initialize(standard.data(),standard.size()*8);VirtualAllocator.Initialize(virtual_arena.data(),virtual_arena.size()*8);gMemoryInitialized=1;
        const auto a=StandardAllocator.TotalFreeMemory(),b=VirtualAllocator.TotalFreeMemory();
        for(unsigned repeat=0;repeat<3;++repeat)
        {
            const GLMemoryRequirement requirements[]={{GLM_Header,65536},{GLM_VertexData,65536},{GLM_TextureData,65536}};
            const GLMemoryConfig config{262144,262144,requirements,3,512};glInitResourcePools();glInitMemory(&config);
            InitializeOriginalGraphicsState();SetGraphicsCacheInvalidator(CpuInvalidate);MaterialPrograms materials;
            OriginalViews views(640,480,CpuInvalidate);Backend backend;OriginalFrames frames(backend);
            Generated(frames);if(argc==5)Owned(frames,argv[2],argv[3],argv[4]);
            frames.Release();views.Release();materials.Release();glShutdownMemory();
            Check(StandardAllocator.TotalFreeMemory()==a&&VirtualAllocator.TotalFreeMemory()==b,"Repeated effects resource session leaked native arenas");
        }
        ResetStartupMemory();std::cout<<checks<<" effects vertex checks passed\n";
    }
    catch(const std::exception& e){std::cerr<<"FAILED: "<<e.what()<<" (check "<<checks<<")\n";return 1;}
}
