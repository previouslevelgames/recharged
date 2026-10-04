#include "runtime/frontend_packets.h"
#include "runtime/frontend_font_packets.h"
#include "runtime/views.h"
#include "NL/gl/glMatrix.h"
#include "Game/FE/FrontendImageState.h"
#include "Game/GL/GLInventory.h"
#include "NL/gl/glDraw3.h"
#include "NL/gl/glMemory.h"
#include "NL/gl/glTexture.h"
#include "NL/gl/glTextureManager.h"
#include "NL/glx/glxTexture.h"
#include <algorithm>
#include <exception>
#include <optional>
#include <thread>
#include <variant>
// Original GLViewSort_None factory; compare identity to reject reordering views.
extern UnidentifiedPacketSorter* fn_802CEFC0();
namespace mscharged
{
namespace
{
using resources::Require;
bool Linked(const GLResourcePool* pool)
{
    const auto* first=glGetResourcePools(); if (!first) return false;
    auto* item=first; do { if (item==pool) return true; item=item->m_next; } while (item!=first);
    return false;
}
bool SameOwners(const FrontendSession::Handle& a,const FrontendSession::Handle& b)
{ return a && b && a->visuals==b->visuals && a->images==b->images; }
struct ImagePacket { glQuad3 quad; nlMatrix4 model; std::uint32_t texture,blend; };
using Packet=std::variant<detail::FontPackets,ImagePacket>;
nlMatrix4 Matrix(const std::array<float,16>& input)
{
    for (float value:input) detail::CheckFrontendCoordinate(value);
    Require(input[2]==0 && input[3]==0 && input[6]==0 && input[7]==0 && input[8]==0 && input[9]==0
        && input[11]==0 && input[14]==0 && input[15]==1,"Frontend packet transform must be planar affine");
    nlMatrix4 result; result.SetIdentity();
    result.e[0]=input[0]; result.e[1]=input[1]; result.e[4]=input[4]; result.e[5]=input[5];
    result.e[12]=input[12]; result.e[13]=input[13]; return result;
}
std::vector<Packet> PreparePackets(const FrontendSession::Handle& input)
{
    Require(input && input->visuals && input->images,"Frontend packets require retained visual and image owners");
    Require(input->layout.entries.size()<=4096,"Frontend packet layout exceeds 4096 entries");
    std::vector<Packet> packets; packets.reserve(input->layout.entries.size());
    std::size_t quads=0;
    for (const auto& entry:input->layout.entries)
    {
        if (const auto* text=std::get_if<resources::FrontendLayoutText>(&entry))
        {
            Require(text->layout.font && (text->layout.font==input->visuals->text || text->layout.font==input->visuals->heading),
                "Frontend text does not retain this generation's font");
            packets.emplace_back(detail::PrepareFontPackets(text->layout,Matrix(text->transform),text->colour));
            quads+=text->layout.quads.size();
        }
        else
        {
            const auto& image=std::get<resources::FrontendLayoutImage>(entry);
            Require(image.texture && image.blend<=7 && !resources::IsDynamicFrontendImage(image.texture->id),
                "Frontend packets require an ordinary image and original blend mode");
            const auto found=input->images->textures.find(image.texture->id);
            Require(found!=input->images->textures.end() && found->second==image.texture,
                "Frontend image does not retain this generation's texture");
            resources::ValidateFrontendImageTexture(*image.texture);
            ImagePacket p{}; p.model=Matrix(image.transform); p.texture=image.texture->id; p.blend=image.blend;
            for (unsigned i=0;i<4;++i)
            {
                const auto& v=image.vertices[i];
                for (float value:{v.x,v.y,v.u,v.v}) detail::CheckFrontendCoordinate(value);
                for (float value:{v.u,v.v}) Require(value*1024.f>=-32768.f && value*1024.f<32768.f,
                    "Frontend image UV exceeds original signed16 storage");
                p.quad.m_pos[i]={v.x,v.y,0}; p.quad.m_uv[i]={v.u,v.v};
            }
            p.quad.SetColour(nlColour{{image.colour[0],image.colour[1],image.colour[2],image.colour[3]}});
            packets.emplace_back(p); ++quads;
        }
        Require(quads<=16384,"Frontend frame exceeds 16384 quads");
    }
    return packets;
}
struct Generation
{
    struct Texture { std::shared_ptr<const resources::Texture> source; PlatTexture* native=nullptr; std::uint16_t index=0xffff; };
    std::shared_ptr<const FrontendVisualAssets> visuals;
    resources::FrontendImageCatalog::Handle images;
    std::unique_ptr<GLResourcePool,void(*)(GLResourcePool*)> pool{nullptr,glDestroyResourcePool};
    std::map<std::uint32_t,Texture> textures;
    bool submitted=false;
    Generation(const FrontendSession::Handle& input,const Generation* old) : visuals(input->visuals),images(input->images)
    {
        std::size_t bytes=0;
        const auto add=[&](std::shared_ptr<const resources::Texture> t) {
            Require(t && t->id!=0xffffffffU && !resources::IsDynamicFrontendImage(t->id),"Invalid frontend texture identity");
            resources::ValidateFrontendImageTexture(*t);
            const auto [found,inserted]=textures.emplace(t->id,Texture{t});
            Require(inserted || found->second.source==t,"Different frontend resources share one texture hash");
            if (inserted) bytes+=t->pixels.size()+t->palette.size();
            Require(textures.size()<=1024 && bytes<=64*1024*1024,"Frontend graphics exceed 1024 textures or 64 MiB");
        };
        std::map<std::uint32_t,std::shared_ptr<const resources::FrontendFont>> aliases;
        for (const auto& font:{visuals->text,visuals->heading})
        {
            Require(bool(font),"Frontend graphics require both retained fonts");
            resources::LayoutFrontendTextLine(font,u"");
            const auto [found,inserted]=aliases.emplace(font->alias,font);
            Require(inserted || found->second==font,"Conflicting frontend font aliases");
            if (!inserted) continue;
            for (const auto& page:font->pages)
            {
                Require(page.game_format==8 && page.gx_format==9 && page.levels==1,"Frontend font registration requires plain CI8 pages");
                add(std::shared_ptr<const resources::Texture>(font,&page));
            }
        }
        for (const auto& [hash,texture]:images->textures)
        { Require(texture && hash==texture->id,"Frontend catalog texture hash differs from its record"); add(texture); }
        // Every collision must belong solely to the exact retired generation.
        // Candidate registration is invisible until that old pool is destroyed.
        const auto* first=glGetResourcePools();
        if (first)
        {
            const auto* p=first;
            do
            {
                for (const auto& [hash,t]:textures)
                {
                    Require(!p->m_inventory->GetTextureAnim(hash),"Frontend texture hash collides with a texture animation");
                    if (p->m_inventory->GetTexture(hash))
                        Require(old && p==old->pool.get(),"Frontend texture hash collides with another graphics owner");
                }
                p=p->m_next;
            } while (p!=first);
        }
        const GLMemoryRequirement requirement{GLM_Header,std::max<std::size_t>(32768,textures.size()*(sizeof(PlatTexture)+32)+4096)};
        pool.reset(glCreateResourcePool(&requirement,1,"Frontend texture generation"));
        for (auto& [hash,t]:textures)
        {
            t.native=new(pool->Allocate(sizeof(PlatTexture),GLM_Header)) PlatTexture;
            auto& p=*t.native; const auto& s=*t.source;
            p.m_Width=s.width; p.m_Height=s.height; p.m_Levels=p.m_MaxLevel=s.levels;
            p.m_Format=static_cast<eGXTextureFormat>(s.game_format); p.m_nPaletteEntries=s.palette_entries;
            std::copy(s.bits.begin(),s.bits.end(),p.m_Bits);
            p.m_SwizzledData=const_cast<std::uint8_t*>(s.pixels.data());
            p.m_PaletteData=reinterpret_cast<u16*>(const_cast<std::uint8_t*>(s.palette.data()));
            p.m_NativeDataBytes=s.pixels.size(); p.m_NativePaletteBytes=s.palette.size();
            glRegisterTexture(hash,&p,pool.get()); t.index=p.m_TextureIndex;
            Require(t.index!=0xffff && t.index<glGetTextureManager()->mCapacity
                && glGetTextureManager()->mTextures[t.index]==&p,"Frontend texture registration failed");
        }
    }
    // Declare pool after textures? Explicitly destroy it while retained sources
    // still exist: inventory disposal accesses the native headers/texture data.
    ~Generation() { pool.reset(); }
    void Check(bool visible) const
    {
        if (!pool || !Linked(pool.get()) || pool->m_level!=0 || !glGetTextureManager())
            throw std::logic_error("Frontend graphics generation lost its original pool");
        for (const auto& [hash,t]:textures)
        {
            const auto& p=*t.native; const auto& s=*t.source;
            if (pool->m_inventory->GetTexture(hash)!=t.native || t.index>=glGetTextureManager()->mCapacity
                || glGetTextureManager()->mTextures[t.index]!=t.native || p.m_TextureIndex!=t.index
                || (visible && glGetTextureIndex(hash)!=t.index) || p.m_SwizzledData!=s.pixels.data()
                || p.m_PaletteData!=reinterpret_cast<const u16*>(s.palette.data())
                || p.m_NativeDataBytes!=s.pixels.size() || p.m_NativePaletteBytes!=s.palette.size()
                || p.m_Width!=s.width || p.m_Height!=s.height || p.m_Levels!=s.levels
                || p.m_Format!=s.game_format || p.m_nPaletteEntries!=s.palette_entries)
                throw std::logic_error("Frontend graphics texture ownership was replaced or mutated");
        }
    }
};
}
struct FrontendPacketRenderer::Implementation
{
    const std::thread::id thread=std::this_thread::get_id();
    void (*drain)();
    std::unique_ptr<Generation> generation;
    FrontendSession::Handle current;
    std::optional<std::uint64_t> pending;
    bool busy=false,failed=false,released=false;
    explicit Implementation(void (*d)()) : drain(d)
    { if (!drain || !glGetTextureManager() || glIsFrameActive() || glNativeViewDispatchActive())
        throw std::logic_error("Frontend packets require idle initialized graphics and a real drain callback"); }
    void Check() const
    {
        if (thread!=std::this_thread::get_id() || busy || released || glNativeViewDispatchActive())
            throw std::logic_error("Frontend packets require their nonrecursive live owner thread outside view dispatch");
        if (!glGetTextureManager()) throw std::logic_error("Frontend graphics services have been released");
        if (generation) generation->Check(true);
    }
    void Idle() const
    { Check(); if (glIsFrameActive() || pending) throw std::logic_error("Finish frontend packets before replacing graphics ownership"); }
    void Drain()
    {
        if (!generation || !generation->submitted) return; // Initial registration precedes deferred GXInit.
        const auto frame=glNativeFrameGeneration(); drain();
        if (glIsFrameActive() || frame!=glNativeFrameGeneration()) throw std::logic_error("Frontend drain changed frame ownership");
        if (generation) generation->Check(true);
    }
    void Release()
    {
        if (released) { if (thread!=std::this_thread::get_id() || busy) throw std::logic_error("Invalid frontend release thread"); return; }
        Idle(); busy=true;
        try { Drain(); generation.reset(); current.reset(); released=true; busy=false; }
        catch (...) { busy=false; throw; }
    }
    ~Implementation() { try { Release(); } catch (...) { std::terminate(); } }
};
FrontendPacketRenderer::FrontendPacketRenderer(void (*drain)()) : impl_(std::make_unique<Implementation>(drain)) {}
FrontendPacketRenderer::~FrontendPacketRenderer()=default;
void FrontendPacketRenderer::Prepare(FrontendSession::Handle input)
{
    auto& s=*impl_; s.Idle(); (void)PreparePackets(input);
    if (SameOwners(s.current,input)) { s.current=std::move(input); return; }
    s.busy=true;
    try
    {
        // Drain first. An exception cannot retire the previous visible frame.
        s.Drain();
        auto next=std::make_unique<Generation>(input,s.generation.get()); next->Check(false);
        if (s.generation) s.generation->Check(true);
        s.generation.reset(); // Original ring now resolves candidate authored hashes.
        s.generation=std::move(next); s.current=std::move(input); s.busy=false;
    }
    catch (...) { s.busy=false; throw; }
}
FrontendSession::Handle FrontendPacketRenderer::Current() const { impl_->Check(); return impl_->current; }
std::size_t FrontendPacketRenderer::Textures() const { impl_->Check(); return impl_->generation ? impl_->generation->textures.size() : 0; }
unsigned FrontendPacketRenderer::Submit(GLView& view,FrontendSession::Handle input)
{
    auto& s=*impl_; s.Check();
    if (!glIsFrameActive() || s.failed || (s.pending && *s.pending!=glNativeFrameGeneration())
        || !view.m_Interface || view.m_NativeIterating || view.m_CreateSorter!=fn_802CEFC0)
        throw std::logic_error("Frontend submission requires an unsorted collecting view and a finished previous frame");
    Require(SameOwners(s.current,input),"Prepare changed frontend resources while idle before submission");
    const auto packets=PreparePackets(input);
    s.busy=true;
    try
    {
        unsigned quads=0;
        if (!packets.empty()) { s.pending=glNativeFrameGeneration(); s.generation->submitted=true; }
        for (const auto& packet:packets)
        {
            if (const auto* text=std::get_if<detail::FontPackets>(&packet))
            { detail::AttachFontPackets(view,*text,0); quads+=text->quads.size(); }
            else
            {
                const auto& image=std::get<ImagePacket>(packet); detail::FrontendPacketState state;
                FrontendImageRasterState(true,image.blend);
                const auto matrix=glAllocSetMatrix(image.model);
                if (matrix==GL_INVALID_MATRIX) throw std::runtime_error("Frontend image matrix allocation failed");
                glSetCurrentTexture(image.texture,GLTT_Diffuse); glSetCurrentMatrix(matrix);
                const auto* model=image.quad.GetModel();
                if (!model) throw std::runtime_error("Original frontend image packet allocation failed");
                view.AttachModel(model,0); ++quads;
            }
        }
        s.current=std::move(input); s.busy=false; return quads;
    }
    catch (...) { s.failed=true; s.busy=false; throw; }
}
void FrontendPacketRenderer::FinishFrame()
{
    auto& s=*impl_; s.Check(); if (!s.pending) return;
    if (glIsFrameActive() || *s.pending==glNativeFrameGeneration())
        throw std::logic_error("Send or cancel frontend packets before finishing their frame");
    s.busy=true;
    try { s.Drain(); s.pending.reset(); s.failed=false; s.busy=false; }
    catch (...) { s.busy=false; throw; }
}
void FrontendPacketRenderer::Release() { impl_->Release(); }
}
