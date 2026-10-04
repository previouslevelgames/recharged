#include "runtime/particle_controller_render.h"
#include "runtime/views.h"
#include "Game/Effects/ParticleBillboard.h"
#include "Game/GL/GLInventory.h"
#include "NL/gl/gl.h"
#include "NL/gl/glMemory.h"
#include "NL/gl/glTexture.h"
#include "NL/gl/glTextureManager.h"
#include "NL/glx/glxTexture.h"
#include <algorithm>
#include <cmath>
#include <optional>
#include <thread>

namespace mscharged
{
namespace
{
bool Linked(const GLResourcePool* pool)
{
    auto* head=glGetResourcePools();if(!head)return false;auto* item=head;
    do{if(item==pool)return true;item=item->m_next;}while(item!=head);return false;
}
bool Same(const resources::Texture& a,const resources::Texture& b)
{
    return a.id==b.id&&a.width==b.width&&a.height==b.height&&a.levels==b.levels&&a.game_format==b.game_format
        &&a.gx_format==b.gx_format&&a.bits==b.bits&&a.palette_entries==b.palette_entries&&a.pixels==b.pixels&&a.palette==b.palette;
}
}
struct ParticleControllerRenderer::Implementation
{
    struct Texture {std::shared_ptr<const resources::Texture> source;PlatTexture* registered=nullptr;std::uint16_t index=0xffff;};
    GLResourcePool& pool;ParticleControllers& controllers;void(*drain)();
    std::vector<ParticleControllers::RenderEntry> systems;
    std::map<std::uint32_t,Texture> textures;
    GLResourceMark mark=0;int level=0;
    std::thread::id thread=std::this_thread::get_id();bool busy=false,leased=false;
    std::optional<std::uint64_t> pending;
    Implementation(GLResourcePool& p,ParticleControllers& c,void(*d)()) : pool(p),controllers(c),drain(d)
    {
        if(!drain||!Linked(&pool)||!glGetTextureManager()||glNativeViewDispatchActive()||glIsFrameActive())
            throw std::logic_error("Controller renderer requires an idle initialized graphics pool and drain callback");
        systems=controllers.RetainRenderers();leased=true;
        try
        {
            for(const auto& s:systems)
            {
                auto texture=s.simulation->Texture();
                if(!texture||texture->id==0xffffffffU)throw std::invalid_argument("Missing authored controller texture");
                auto [it,inserted]=textures.try_emplace(texture->id,Texture{texture});
                if(!inserted&&!Same(*texture,*it->second.source))throw std::invalid_argument("Controller texture hash resolves conflicting retained bytes");
                if(glGetTextureIndex(texture->id)!=0xffff)throw std::invalid_argument("Controller texture is already registered outside this owner");
            }
            mark=pool.MarkResource();level=pool.m_level;
            for(auto& [id,t]:textures)
            {
                auto* memory=pool.Allocate(sizeof(PlatTexture),GLM_Header);
                t.registered=new(memory)PlatTexture;auto& p=*t.registered;const auto& texture=*t.source;
                p.m_Width=texture.width;p.m_Height=texture.height;p.m_Levels=p.m_MaxLevel=texture.levels;
                p.m_Format=static_cast<eGXTextureFormat>(texture.game_format);p.m_nPaletteEntries=texture.palette_entries;
                std::copy(texture.bits.begin(),texture.bits.end(),p.m_Bits);
                p.m_SwizzledData=const_cast<std::uint8_t*>(texture.pixels.data());
                p.m_PaletteData=texture.palette.empty()?nullptr:reinterpret_cast<u16*>(const_cast<std::uint8_t*>(texture.palette.data()));
                p.m_NativeDataBytes=texture.pixels.size();p.m_NativePaletteBytes=texture.palette.size();
                glRegisterTexture(id,&p,&pool);t.index=p.m_TextureIndex;
                if(t.index==0xffff||t.index>=glGetTextureManager()->mCapacity||glGetTextureManager()->mTextures[t.index]!=&p||glGetTextureIndex(id)!=t.index)
                    throw std::logic_error("Controller texture registration failed");
            }
        }
        catch(...)
        {
            if(mark){pool.ReleaseResource(mark);mark=0;}
            textures.clear();systems.clear();controllers.ReleaseRenderers();leased=false;throw;
        }
    }
    void CheckThread() const
    {if(thread!=std::this_thread::get_id()||busy)throw std::logic_error("Controller renderer requires its nonrecursive owner thread");}
    void CheckStorage() const
    {
        if(!mark||!Linked(&pool)||!glGetTextureManager()||pool.m_level<level||glNativeViewDispatchActive())
            throw std::logic_error("Controller renderer storage is inactive or dispatching");
        for(const auto& [id,t]:textures)
            if(pool.m_inventory->GetTexture(id)!=t.registered||t.index>=glGetTextureManager()->mCapacity||glGetTextureManager()->mTextures[t.index]!=t.registered||glGetTextureIndex(id)!=t.index)
                throw std::logic_error("Controller texture binding was replaced");
    }
    void Check() const{CheckThread();CheckStorage();}
    void Release()
    {
        CheckThread();if(!mark)return;CheckStorage();
        if(pending||glIsFrameActive()||pool.m_level!=level)throw std::logic_error("Finish controller frames and nested resource scopes before release");
        controllers.BeginRenderDrain();busy=true;
        const auto generation=glNativeFrameGeneration();
        try
        {
            drain();CheckStorage();
            if(glIsFrameActive()||generation!=glNativeFrameGeneration()||pool.m_level!=level)
                throw std::logic_error("Controller drain changed the original frame or resource scope");
            pool.ReleaseResource(mark);mark=0;textures.clear();systems.clear();
            controllers.EndRenderFrame();controllers.ReleaseRenderers();leased=false;busy=false;
        }
        catch(...){controllers.EndRenderFrame();busy=false;throw;}
    }
    ~Implementation(){try{Release();}catch(...){std::terminate();}}
};
ParticleControllerRenderer::ParticleControllerRenderer(GLResourcePool& p,ParticleControllers& c,void(*d)()) :impl_(std::make_unique<Implementation>(p,c,d)){}
ParticleControllerRenderer::~ParticleControllerRenderer()=default;
bool ParticleControllerRenderer::Active() const{impl_->CheckThread();return impl_->mark!=0;}
std::size_t ParticleControllerRenderer::Textures() const{impl_->Check();return impl_->textures.size();}
void ParticleControllerRenderer::Release(){impl_->Release();}
void ParticleControllerRenderer::FinishFrame()
{
    impl_->Check();if(!impl_->pending)return;
    if(glIsFrameActive()||*impl_->pending==glNativeFrameGeneration())throw std::logic_error("Controller frame must be sent or cancelled before finishing");
    impl_->busy=true;const auto generation=glNativeFrameGeneration();
    try
    {
        impl_->drain();impl_->CheckStorage();
        if(glIsFrameActive()||generation!=glNativeFrameGeneration())
            throw std::logic_error("Controller drain changed the original frame");
        impl_->controllers.EndRenderFrame();impl_->pending.reset();impl_->busy=false;
    }
    catch(...){impl_->busy=false;throw;}
}
unsigned ParticleControllerRenderer::Submit(GLView& view,bool visible,float aspect,bool allow_in_front)
{
    impl_->Check();
    if(!glIsFrameActive()||impl_->pending||!view.m_Interface||view.m_NativeIterating)throw std::logic_error("Controller submission requires one collecting original frame");
    if(!std::isfinite(aspect)||aspect<=0||aspect>16)throw std::invalid_argument("Invalid controller billboard aspect");
    if(!visible)return 0;
    auto entries=impl_->controllers.RenderEntries();
    impl_->controllers.BeginRenderFrame();impl_->pending=glNativeFrameGeneration();
    impl_->busy=true;struct Leave{bool& busy;~Leave(){busy=false;}}leave{impl_->busy};
    const auto generation=glNativeFrameGeneration();nlMatrix4 matrix;view.m_Interface->GetViewMatrix(matrix);
    impl_->CheckStorage();if(!glIsFrameActive()||generation!=glNativeFrameGeneration())throw std::logic_error("Controller camera callback changed frame");
    for(float x:matrix.e)if(!std::isfinite(x))throw std::invalid_argument("Nonfinite controller view matrix");
    nlVector3 right,up;matrix.GetColumn_(0,right);matrix.GetColumn_(1,up);nlVec3Scale(right,aspect);
    unsigned count=0;
    for(const auto& entry:entries)
    {
        if(!entry.visible)continue;
        const auto profile=entry.simulation->RenderProfile();
        if(profile.layer>0x7fffffffU)throw std::invalid_argument("Controller layer exceeds original signed sort range");
        std::vector<ParticleQuad> quads;
        try { quads=entry.simulation->Sample({right.x,right.y,right.z},{up.x,up.y,up.z}); }
        catch(...) { impl_->controllers.FailRenderFrame(); throw; }
        if(quads.empty())continue;if(quads.size()>65535/4)throw std::length_error("Controller particle mesh exceeds GX vertex limit");
        fxSetParticleRasterState(true,false,allow_in_front&&profile.in_front,false,profile.blend);
        GLTexturedColourMeshWriter mesh;mesh.Begin(int(quads.size()*4),GLP_QuadList,nullptr);
        for(const auto& q:quads)
        {
            ParticleReturn value{};for(unsigned i=0;i<4;++i){value.position[i]={q.position[i][0],q.position[i][1],q.position[i][2]};value.texcoord[i]={q.uv[i][0],q.uv[i][1]};value.c.c[i]=q.colour[i];}
            fxWriteParticleQuad(mesh,value,true);
        }
        const auto texture=entry.simulation->Texture();const auto& binding=impl_->textures.at(texture->id);
        auto* parameter=static_cast<glTextureBinding*>(mesh.GetModel()->packets->materialParameters);
        parameter->texture=texture->id;parameter->textureIndex=binding.index;parameter->SetWrapS(false);parameter->SetWrapT(false);parameter->unknown07=0;
        mesh.End();view.AttachModel(mesh.GetModel(),profile.layer);count+=quads.size();
    }
    return count;
}
}
