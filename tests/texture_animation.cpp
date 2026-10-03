#include "runtime/static_inventory.h"
#include "runtime/materials.h"
#include "runtime/graphics_state.h"
#include "runtime/graphics_memory.h"
#include "runtime/startup.h"
#include "Game/GL/GLInventory.h"
#include "Game/GL/GLTextureAnim.h"
#include "NL/MemAlloc.h"
#include "NL/nlMemory.h"
#include "NL/gl/glMemoryInit.h"
#include "NL/gl/glTexture.h"
#include "NL/gl/glTextureManager.h"
#include "NL/glx/glxTexture.h"
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <vector>

using namespace mscharged;
namespace
{
void Check(bool value,const char* message) { if (!value) throw std::runtime_error(message); }
template<class Error=std::invalid_argument,class Action> void Reject(Action action)
{ try { action(); } catch(const Error&) { return; } throw std::runtime_error("Invalid animation operation succeeded"); }
resources::Texture Texture(unsigned id)
{
    resources::Texture t; t.id=id; t.width=t.height=4; t.levels=1;
    t.game_format=3;t.gx_format=6;t.bits={8,8,8,8};t.pixels.resize(64,id);return t;
}
resources::StaticModel Model()
{
    resources::Packet p; p.material.program=0x21db4385;p.material.textures[0]={20,3};
    p.vertices={{{-1,-1,0},{0,0}},{{1,-1,0},{1,0}},{{0,1,0},{0,1}}};p.indices={0,1,2};return {1,{p}};
}
unsigned drains=0;
void Drain() { ++drains; }
void Session()
{
    const GLMemoryRequirement req[]={{GLM_Header,65536},{GLM_VertexData,4096},{GLM_TextureData,4096}};
    const GLMemoryConfig config{32,32,req,3,8};
    glInitMemory(&config);InitializeOriginalGraphicsState();
    auto& pool=*glGetCurrentResourcePool();auto& manager=*glGetTextureManager();
    MaterialPrograms materials;
    const auto mem1=StandardAllocator.TotalFreeMemory(),mem2=VirtualAllocator.TotalFreeMemory();
    const auto free=pool.GetFreeMemory();
    const auto model=Model(); const std::vector<resources::Texture> textures{Texture(10),Texture(11),Texture(12)};
    resources::TextureAnimation input{20,0,0,false,0,{{10,.25f},{11,.5f},{12,.75f}}};
    auto recovered=[&]{ Check(pool.GetFreeMemory()==free && manager.mFreeIndices->mCount==8
        && !glGetTextureAnim(20) && !glx_GetTex(10) && StandardAllocator.TotalFreeMemory()==mem1
        && VirtualAllocator.TotalFreeMemory()==mem2,"Animation rollback leaked nodes, slots or arena storage"); };
    glTextureBinding cached(20);
    {
        StaticInventory batch(pool,{model},textures,Drain,{input});
        auto* anim=glGetTextureAnim(20);Check(anim && anim==pool.m_inventory->GetTextureAnim(20),"Original animation lookup");
        const auto alias=anim->m_textureIndex;
        auto state=[&](int frame,float elapsed,int direction) {
            Check(anim->m_nFrame==frame && anim->m_fTime==elapsed && anim->m_nPlayDir==direction,"Original playback state differs");
            Check(manager.GetTexture(&cached)==pool.m_inventory->GetTexture(10+frame)
                && cached.textureIndex==alias && glGetTextureIndex(20)==alias,"Stable animation alias failed to refresh");
        };
        state(0,0,0);Check(manager.mFreeIndices->mCount==4 && !glx_GetTex(20),"Animation alias is separate from static texture lookup");
        Check(anim->m_pAnimTex!=reinterpret_cast<const GLAnimTex*>(input.frames.data())
            && anim->m_NativeFrameCount==3,"Native frame array ownership");
        pool.m_inventory->UpdateTextureAnims(.125f);state(0,.125f,0);
        pool.m_inventory->UpdateTextureAnims(.125f);state(1,0,0);
        anim->Update(100);state(2,0,0); // Exactly one frame; overshoot is discarded.
        anim->Update(.75f);state(0,0,0);
        anim->m_bPaused=1;anim->Update(.5f);state(0,0,0);anim->m_bPaused=0;
        anim->m_ePlayMode=GLAnimMode_PingPong;anim->m_nPlayDir=1;
        for (auto expected : {1,2,1,0,1}) {anim->Update(10);Check(anim->m_nFrame==expected,"Ping-pong endpoint reversal");}
        state(1,0,1);anim->m_nFrame=0;anim->m_nPlayDir=0;manager.RefreshTextureAnim(anim);
        anim->Update(10);state(1,0,1); // Original zero direction takes the backward branch.
        anim->m_ePlayMode=GLAnimMode_Hold;
        anim->Update(10);state(2,0,1);anim->Update(10);state(2,0,1);
        anim->m_ePlayMode=GLAnimMode_Loop;anim->m_nFrame=0;anim->m_nPlayDir=0;manager.RefreshTextureAnim(anim);
        for (float dt : {-1.f,std::numeric_limits<float>::infinity(),std::numeric_limits<float>::quiet_NaN()})
        { Reject([&]{pool.m_inventory->UpdateTextureAnims(dt);});Reject([&]{anim->Update(dt);});state(0,0,0); }
        anim->m_fTime=std::numeric_limits<float>::max();Reject([&]{anim->Update(std::numeric_limits<float>::max());});anim->m_fTime=0;
        Reject<std::out_of_range>([&]{anim->GetTexture(3);});Reject<std::out_of_range>([&]{anim->SetTexture(-1,{});});
        anim->m_nNumTextures=4;Reject([&]{anim->Update(0);});anim->m_nNumTextures=3;
        anim->m_nFrame=3;Reject([&]{anim->Update(0);});anim->m_nFrame=0;
        const auto index=anim->m_pAnimTex[0].m_TexHandle;
        anim->m_pAnimTex[0].m_TexHandle=alias;Reject([&]{anim->Update(0);});anim->m_pAnimTex[0].m_TexHandle=index;
        Reject([&]{manager.RegisterTextureAnim(anim);});
        {
            alignas(32) unsigned char small[64]{};
            MemoryAllocator empty{};empty.Initialize(small,sizeof(small));
            void* held=empty.Allocate(sizeof(small)-alignof(FreeBlockList),8,false);
            auto pending=*anim;pending.m_textureIndex=0xFFFF;pending.m_uHashID=21;
            const auto slots=manager.mFreeIndices->mCount;
            {
                ScopedGameAllocator allocator(empty);
                Reject<std::bad_alloc>([&]{pool.m_inventory->AddTextureAnim(21,&pending);});
            }
            Check(!glGetTextureAnim(21) && pending.m_textureIndex==0xFFFF
                && manager.mFreeIndices->mCount==slots,"Animation node OOM consumed an alias slot");
            empty.Free(held);
        }
        Reject<std::logic_error>([&]{glReleaseTexture(pool.m_inventory->GetTexture(10));});
        Reject<std::logic_error>([]{glShutdownTextureManager();});
        auto lower=anim;
        {
            auto nested=input;nested.paused=true;
            StaticInventory shadow(pool,{model},textures,Drain,{nested});
            anim=glGetTextureAnim(20);Check(anim!=lower && manager.GetTexture(&cached)==pool.m_inventory->GetTexture(10),"Layered animation shadowing");
            shadow.Release();
        }
        anim=glGetTextureAnim(20);Check(anim==lower,"Animation rollback lost lower layer");state(0,0,0);
        batch.Release();batch.Release();Check(!manager.GetTexture(&cached),"Released cached animation still resolves");
    }
    recovered();
    for (unsigned mode=0;mode<3;++mode)
    {
        auto single=input;single.mode=mode;single.frames.resize(1);single.elapsed=.125f;
        StaticInventory batch(pool,{model},textures,Drain,{single});auto* anim=glGetTextureAnim(20);
        anim->Update(10);Check(anim->m_nFrame==0 && anim->m_fTime==.125f,"Single-frame early return changed");
    }
    recovered();
    {
        auto zero=input;zero.frames[0].duration=0;
        StaticInventory batch(pool,{model},textures,Drain,{zero});auto* anim=glGetTextureAnim(20);
        anim->Update(0);Check(anim->m_nFrame==1 && anim->m_fTime==0,"Zero-duration frame must advance once even at dt=0");
    }
    recovered();
    for (unsigned invalid=0;invalid<8;++invalid)
    {
        auto bad=input;
        switch(invalid) {
        case 0:bad.frames.clear();break;case 1:bad.frames[0].texture=99;break;
        case 2:bad.frames[0].texture=20;break;case 3:bad.frames[0].duration=-1;break;
        case 4:bad.id=10;break;case 5:bad.mode=3;break;case 6:bad.direction=2;break;
        case 7:bad.elapsed=std::numeric_limits<float>::quiet_NaN();break;
        }
        Reject([&]{StaticInventory fail(pool,{model},textures,Drain,{bad});});recovered();
    }
    auto bad_model=model;bad_model.packets[0].material.program=0xdeadbeef;
    Reject<std::runtime_error>([&]{StaticInventory fail(pool,{bad_model},textures,Drain,{input});});recovered();
    {
        std::vector<resources::TextureAnimation> many;
        for(unsigned i=0;i<6;++i) { auto a=input;a.id=20+i;many.push_back(a); }
        Reject<std::length_error>([&]{StaticInventory fail(pool,{model},textures,Drain,many);});recovered();
    }
    materials.Release();glShutdownMemory();
    Check(!glGetTextureAnim(20) && !glGetTextureManager(),"Animation shutdown left global lookup state");
}
}
int main()
{
    try {
        std::vector<std::uint64_t> standard(1024*1024),virtual_arena(1024*1024);
        ResetStartupMemory();StandardAllocator.Initialize(standard.data(),standard.size()*8);
        VirtualAllocator.Initialize(virtual_arena.data(),virtual_arena.size()*8);gMemoryInitialized=1;
        const auto a=StandardAllocator.TotalFreeMemory(),b=VirtualAllocator.TotalFreeMemory();
        for(unsigned i=0;i<3;++i) { Session();Check(StandardAllocator.TotalFreeMemory()==a && VirtualAllocator.TotalFreeMemory()==b,"Repeated animation session leaked arenas"); }
        ResetStartupMemory();
        std::cout<<"Texture animation: original timing, modes, native ownership, alias refresh, layered rollback and failure cleanup passed\n";return 0;
    } catch(const std::exception& e) {std::cerr<<"FAILED: "<<e.what()<<'\n';return 1;}
}
