#include "runtime/frontend_movie_render.h"
#include "runtime/movie_draw_observer.h"
#include "runtime/views.h"
#include "Game/GL/MovieQuadSteps.h"
#include "Game/GL/GLInventory.h"
#include "NL/gl/glMemory.h"
#include "NL/gl/gl.h"
#include "NL/gl/glMatrix.h"
#include "NL/gl/glTextureManager.h"
#include "NL/gl/glTexture.h"
#include "NL/gl/glState.h"
#include "NL/glx/glxTexture.h"
#include <aurora/aurora.h>
#include <dolphin/gx.h>
#include <cmath>
#include <thread>
#include <optional>
#include <stdexcept>

extern bool gMovieYUVEnabled;
namespace mscharged
{
namespace
{
bool Linked(const GLResourcePool* pool)
{
    auto* first=glGetResourcePools();if(!first)return false;auto* item=first;
    do{if(item==pool)return true;item=item->m_next;}while(item!=first);return false;
}
void Require(bool value,const char* text){if(!value)throw std::logic_error(text);}
}
struct FrontendMovieRenderer::Implementation
{
    GLResourcePool& pool;void(*drain)();unsigned width,height;
    const std::thread::id thread=std::this_thread::get_id();
    std::array<unsigned long,3> hashes{glGetTexture("movie"),glGetTexture("movie_u"),glGetTexture("movie_v")};
    std::array<PlatTexture*,3> textures{};std::array<std::vector<std::uint8_t>,3> blank;
    GLResourceMark mark=0;int level=0;bool busy=false;
    resources::ThpMovieFrameHandle retained;
    std::optional<std::uint64_t> pending;std::uint64_t presentation=0;
    const glModelPacket* packet=nullptr;
    Implementation(GLResourcePool& p,unsigned w,unsigned h,void(*d)()):pool(p),drain(d),width(w),height(h)
    {
        Require(d&&Linked(&p)&&glGetTextureManager()&&!glIsFrameActive()&&!glNativeViewDispatchActive(),"Movie renderer needs idle real graphics storage");
        Require(w&&h&&w<=1920&&h<=1088&&w%16==0&&h%16==0,"Movie plane dimensions exceed decoded profile");
        Require(glGetMaterialProgram(0xec35caab)&&gMovieYUVEnabled,"Original YUV movie material is not available");
        for(auto hash:hashes)Require(glGetTextureIndex(hash)==0xffff,"Movie texture hash already belongs to another owner");
        blank[0].assign(std::size_t(w)*h,0x10);blank[1].assign(std::size_t(w)*h/4,0x80);blank[2]=blank[1];
        mark=p.MarkResource();level=p.m_level;
        try
        {
            for(unsigned i=0;i<3;++i)
            {
                auto* t=new(p.Allocate(sizeof(PlatTexture),GLM_Header))PlatTexture;textures[i]=t;
                t->m_Width=i?w/2:w;t->m_Height=i?h/2:h;t->m_Levels=t->m_MaxLevel=1;t->m_Format=GXTex_I8;
                // Resolve original glplatTextureGetNumBits I8 fallback once.
                t->m_Bits[0]=8;t->m_Bits[1]=t->m_Bits[2]=t->m_Bits[3]=0;
                t->m_SwizzledData=blank[i].data();t->m_NativeDataBytes=blank[i].size();
                glRegisterTexture(hashes[i],t,&p);
                Require(t->m_TextureIndex!=0xffff&&glGetTextureIndex(hashes[i])==t->m_TextureIndex,"Movie texture registration failed");
            }
        }
        catch(...){p.ReleaseResource(mark);mark=0;throw;}
    }
    void Thread()const{Require(thread==std::this_thread::get_id()&&!busy,"Movie renderer requires its nonrecursive owner thread");}
    void Storage()const
    {
        Require(mark&&Linked(&pool)&&glGetTextureManager()&&pool.m_level>=level&&!glNativeViewDispatchActive(),"Movie graphics storage is inactive");
        Require(gMovieYUVEnabled,"Movie YUV mode was changed externally");
        for(unsigned i=0;i<3;++i)
        {
            const auto* t=textures[i];const auto index=t->m_TextureIndex;
            Require(pool.m_inventory->GetTexture(hashes[i])==t&&index<glGetTextureManager()->mCapacity
                &&glGetTextureManager()->mTextures[index]==t&&glGetTextureIndex(hashes[i])==index,
                "Movie texture binding was replaced");
            const auto* bytes=retained?(i==0?&retained->y:i==1?&retained->u:&retained->v):&blank[i];
            Require(t->m_Width==(i?width/2:width)&&t->m_Height==(i?height/2:height)&&t->m_Format==GXTex_I8
                &&t->m_Levels==1&&t->m_MaxLevel==1&&t->m_nPaletteEntries==0&&!t->m_PaletteData
                &&t->m_SwizzledData==bytes->data()&&t->m_NativeDataBytes==bytes->size()&&t->m_Bits[3]==0,
                "Movie texture metadata or retained planes were changed");
        }
    }
    void Release()
    {
        Thread();if(!mark)return;Storage();Require(!pending&&!glIsFrameActive()&&pool.m_level==level,"Finish movie frames and nested pools before release");
        busy=true;try{drain();Storage();pool.ReleaseResource(mark);mark=0;retained.reset();busy=false;}
        catch(...){busy=false;throw;}
    }
    ~Implementation(){try{Release();}catch(...){std::terminate();}}
};
FrontendMovieRenderer::FrontendMovieRenderer(GLResourcePool& p,unsigned w,unsigned h,void(*d)()):impl_(std::make_unique<Implementation>(p,w,h,d)){}
FrontendMovieRenderer::~FrontendMovieRenderer()=default;
void FrontendMovieRenderer::Release(){impl_->Release();}
void FrontendMovieRenderer::Submit(GLView& view,resources::ThpMovieFrameHandle frame,const FrontendMovieQuad& quad)
{
    auto& s=*impl_;s.Thread();s.Storage();
    Require(glIsFrameActive()&&!s.pending&&view.m_Interface&&!view.m_NativeIterating,"Movie submission requires one collecting frame");
    Require(frame&&frame->width==s.width&&frame->height==s.height&&frame->y.size()==std::size_t(s.width)*s.height
        &&frame->u.size()==frame->y.size()/4&&frame->v.size()==frame->u.size(),"Movie output planes differ from retained texture storage");
    for(const auto& values:{quad.positions,quad.uv})for(const auto& point:values)for(auto f:point)
        Require(std::isfinite(f)&&std::abs(f)<=65536,"Movie quad has an invalid coordinate");
    s.busy=true;struct Leave{bool& b;~Leave(){b=false;}}leave{s.busy};
    s.pending=glNativeFrameGeneration();s.presentation=aurora_get_last_presentation().sequence;s.retained=std::move(frame);
    const std::array<const std::vector<std::uint8_t>*,3> planes{&s.retained->y,&s.retained->u,&s.retained->v};
    for(unsigned i=0;i<3;++i)s.textures[i]->m_SwizzledData=const_cast<std::uint8_t*>(planes[i]->data());
    GXInvalidateTexAll();
    // The original image caller supplies raster state; this movie-only view
    // uses opaque replace with no depth. YUV material deliberately emits alpha0.
    const auto raster=glHandleizeRasterState();glStateBundle saved;glStateSave(saved);glStateRestore(saved);
    struct Restore{const glStateBundle& state;unsigned long raster;~Restore(){for(unsigned i=0;i<GLS_Num;++i)
        glSetRasterState(static_cast<eGLState>(i),glGetRasterState(raster,static_cast<eGLState>(i)));glStateRestore(state);}}restore{saved,raster};
    glSetRasterState(GLS_AlphaBlend,0);glSetRasterState(GLS_AlphaTest,0);glSetRasterState(GLS_DepthTest,0);
    glSetRasterState(GLS_DepthWrite,0);glSetRasterState(GLS_Culling,0);glSetRasterState(GLS_ColourWrite,1);
    glSetCurrentRasterState(glHandleizeRasterState());
    nlMatrix4 identity;identity.SetIdentity();glSetCurrentMatrix(glAllocSetMatrix(identity));
    nlVector2 positions[4],uv[4];for(unsigned i=0;i<4;++i){positions[i]={quad.positions[i][0],quad.positions[i][1]};uv[i]={quad.uv[i][0],quad.uv[i][1]};}
    nlFloatColour tint;tint.c[0]=tint.c[1]=tint.c[2]=tint.c[3]=1;
    auto* model=BuildMovieImageQuad(&view,s.hashes[0],tint,positions,uv);Require(model&&model->numPackets==1,"Original movie quad failed");
    s.packet=model->packets;detail::BeginMovieDrawObservation(s.packet);
}
FrontendMoviePresentation::Handle FrontendMovieRenderer::FinishFrame()
{
    auto& s=*impl_;s.Thread();s.Storage();if(!s.pending)return{};
    Require(!glIsFrameActive()&&glNativeFrameGeneration()!=*s.pending,"Send or cancel movie frame before finishing");
    s.busy=true;struct Leave{bool& b;~Leave(){b=false;}}leave{s.busy};
    const auto generation=glNativeFrameGeneration();s.drain();s.Storage();
    Require(!glIsFrameActive()&&generation==glNativeFrameGeneration(),"Movie drain changed original frame");
    const auto presented=aurora_get_last_presentation();
    const bool drawn=s.packet&&detail::FinishMovieDrawObservation(s.packet);s.packet=nullptr;
    const bool exact=drawn&&generation==*s.pending+1&&presented.sequence==s.presentation+1;
    s.pending.reset();
    if(!exact)return{};
    return FrontendMoviePresentation::Handle(new FrontendMoviePresentation(s.retained,presented.sequence));
}
}
