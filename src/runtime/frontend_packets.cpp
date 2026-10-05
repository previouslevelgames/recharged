#include "runtime/frontend_packets.h"
#include "runtime/frontend_font_packets.h"
#include "runtime/frontend_movie_render.h"
#include "Game/FE/FrontendImageSteps.h"
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
#include <cmath>
#include <exception>
#include <optional>
#include <thread>
#include <variant>
// Original GLViewSort_None factory; compare identity to reject reordering views.
extern GLPacketSorter* CreateUnsortedPacketSorter();
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
struct MoviePacket { FrontendMovieQuad quad; };
struct PendingMoviePacket {}; // Deliberate nondraw entry; never a static image fallback.
struct PreparedPackets
{
    std::vector<std::variant<detail::FontPackets,ImagePacket,MoviePacket,PendingMoviePacket>> packets;
    FrontendMovieEntryStatus movies;
};
using Packet=std::variant<detail::FontPackets,ImagePacket,MoviePacket,PendingMoviePacket>;
nlMatrix4 Matrix(const std::array<float,16>& input)
{
    for (float value:input) detail::CheckFrontendCoordinate(value);
    Require(input[2]==0 && input[3]==0 && input[6]==0 && input[7]==0 && input[8]==0 && input[9]==0
        && input[11]==0 && input[14]==0 && input[15]==1,"Frontend packet transform must be planar affine");
    nlMatrix4 result; result.SetIdentity();
    result.e[0]=input[0]; result.e[1]=input[1]; result.e[4]=input[4]; result.e[5]=input[5];
    result.e[12]=input[12]; result.e[13]=input[13]; return result;
}
PreparedPackets PreparePackets(const FrontendSession::Handle& input,
    const FrontendMovieImageBinding::Handle& binding,const FrontendMovieImageBinding::Handle& retired)
{
    Require(input && input->visuals && input->images,"Frontend packets require retained visual and image owners");
    Require(input->layout.entries.size()<=4096,"Frontend packet layout exceeds 4096 entries");
    PreparedPackets prepared; auto& packets=prepared.packets; packets.reserve(input->layout.entries.size());
    unsigned movie_count=0;
    std::size_t quads=0;
    for (const auto& entry:input->layout.entries)
    {
        if (const auto* text=std::get_if<resources::FrontendLayoutText>(&entry))
        {
            Require(text->layout.font && (text->layout.font==input->visuals->text || text->layout.font==input->visuals->heading),
                "Frontend text does not retain this generation's font");
            packets.emplace_back(detail::PrepareFontPackets(text->layout,Matrix(text->transform),text->colour,text->scissor));
            quads+=text->layout.quads.size();
        }
        else if(const auto* movie=std::get_if<resources::FrontendLayoutMovie>(&entry))
        {
            Require(++movie_count==1,"Only one authored movie instance per frame is qualified");
            const auto node=std::find_if(input->graph.instances.begin(),input->graph.instances.end(),
                [&](const auto& item){return item.offset==movie->instance;});
            const auto resource=std::find_if(input->graph.resources.begin(),input->graph.resources.end(),
                [&](const auto& item){return item.offset==movie->resource;});
            Require(node!=input->graph.instances.end()&&node->type==2&&node->resource==movie->resource
                &&resource!=input->graph.resources.end()&&resource->type==0&&resource->native_movie==movie->image,
                "Movie entry differs from its retained graph instance/resource");
            const std::array<std::string_view,2> names{"Layer","movie"};
            const auto selected=resources::FindFrontendNode(input->graph,{},resources::FrontendNamedPath(names),resources::FrontendNodeType::Image);
            Require(selected&&selected->id==movie->instance,"Movie entry is not the current authored Layer/movie");
            MoviePacket packet;packet.quad.transform=movie->transform;packet.quad.colour=movie->colour;
            (void)Matrix(movie->transform);
            for(float value:movie->transform)Require(std::abs(value)<=65536,"Movie matrix exceeds its checked drawable profile");
            for(float value:movie->colour)Require(std::isfinite(value)&&value>=0&&value<=1,"Invalid movie float tint");
            for(float value:movie->uv){detail::CheckFrontendCoordinate(value);Require(std::abs(value)<=65536,"Movie UV exceeds its checked drawable profile");}
            // The resource name is unchanged by SetTextureHandle. Only a
            // renderer-created opaque image can identify an installed epoch.
            if(!movie->image)
            {
                Require(resource->hash==resources::FrontendNameHash("movie"),"Unbound movie entry has no authored movie identity");
                // A known failing provider is never turned into a pending success.
                if(binding&&SameOwners(input,binding->SourceFrame())&&input->request.path==binding->SourceFrame()->request.path
                    &&input->graph.id==binding->SourceFrame()->graph.id&&movie->resource==binding->Resource())binding->Playback()->Check();
                ++prepared.movies.awaiting_binding;packets.emplace_back(PendingMoviePacket{});continue;
            }
            const auto known=binding&&movie->image==binding->Image()?binding:
                retired&&movie->image==retired->Image()?retired:FrontendMovieImageBinding::Handle{};
            Require(bool(known),"Movie entry has stale or foreign registration metadata");
            const auto& source=known->SourceFrame();
            Require(SameOwners(input,source)&&input->request.path==source->request.path&&input->graph.id==source->graph.id
                &&movie->resource==known->Resource(),"Frontend movie entry belongs to another retained scene");
            const auto original_node=std::find_if(source->graph.instances.begin(),source->graph.instances.end(),
                [&](const auto& item){return item.offset==node->offset;});
            const auto original_resource=std::find_if(source->graph.resources.begin(),source->graph.resources.end(),
                [&](const auto& item){return item.offset==resource->offset;});
            Require(original_node!=source->graph.instances.end()&&original_node->type==node->type
                &&original_node->resource==node->resource&&original_node->hash==node->hash&&original_node->library==node->library
                &&original_resource!=source->graph.resources.end()&&original_resource->type==resource->type
                &&original_resource->hash==resource->hash&&original_resource->file_block==resource->file_block,
                "Movie authored instance/resource provenance changed");
            known->Playback()->Check();
            if(known->Playback()->Status().state==FrontendMovieState::Cancelled)
            {
                // Credits selects its next instance while the same resource
                // still holds the cancelled NLG epoch. Preserve that nondraw
                // position across idle retirement; retain no old GPU lease.
                ++prepared.movies.cancelled;packets.emplace_back(PendingMoviePacket{});continue;
            }
            Require(known==binding&&known->Active()&&movie->instance==known->Instance(),
                "Movie entry has inactive or mismatched registration metadata");
            if(!known->Playback()->Current())
            {++prepared.movies.awaiting_frame;packets.emplace_back(PendingMoviePacket{});continue;}
            ++prepared.movies.ready;
            struct UV{const std::array<float,4>& v;float GetUVX()const{return v[0];}float GetUVY()const{return v[1];}
                float GetUVWidth()const{return v[2];}float GetUVHeight()const{return v[3];}}uv{movie->uv};
            for(float value:movie->uv)detail::CheckFrontendCoordinate(value);
            nlVector2 coordinates[4];FrontendImageUV(&uv,movie->image->Width(),movie->image->Height(),coordinates);
            for(unsigned i=0;i<4;++i)
            {
                detail::CheckFrontendCoordinate(coordinates[i].x);detail::CheckFrontendCoordinate(coordinates[i].y);
                Require(std::abs(coordinates[i].x)<=65536&&std::abs(coordinates[i].y)<=65536,"Movie UV exceeds its checked drawable profile");
                packet.quad.positions[i]={FrontendImageQuadPositions[i].x,FrontendImageQuadPositions[i].y};
                packet.quad.uv[i]={coordinates[i].x,coordinates[i].y};
            }
            packet.quad.source_image_state=true;packets.emplace_back(std::move(packet));++quads;

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
    return prepared;
}
struct Generation
{
    struct Texture { std::shared_ptr<const resources::Texture> source; PlatTexture* native=nullptr; std::uint16_t index=0xffff; };
    std::shared_ptr<const FrontendVisualAssets> visuals;
    resources::FrontendImageCatalog::Handle images;
    std::unique_ptr<GLResourcePool,void(*)(GLResourcePool*)> pool{nullptr,glDestroyResourcePool};
    std::map<std::uint32_t,Texture> textures;
    bool submitted=false;int expected_level=0;
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
        if (!pool || !Linked(pool.get()) || pool->m_level!=expected_level || !glGetTextureManager())
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
    FrontendMovieImageBinding::Handle movie_binding,retired_movie;
    FrontendMoviePacketStatus movie_status;
    std::shared_ptr<FrontendMovieRenderer> movie_renderer;
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
        if(movie_binding)Require(movie_binding->Active(),"Frontend movie binding was retired externally");
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
    void RetireMovie()
    {
        Idle();if(!movie_binding)return;busy=true;
        try
        {
            Drain();movie_renderer->Release();movie_renderer.reset();
            movie_binding->lifetime_->active=false;movie_binding->lifetime_->graphics.reset();
            retired_movie.reset();
            if(movie_binding->Playback()->Status().state==FrontendMovieState::Cancelled)retired_movie=std::move(movie_binding);
            else movie_binding.reset();
            generation->expected_level=0;generation->Check(true);busy=false;
        }
        catch(...){busy=false;throw;}
    }
    void Release()
    {
        if (released) { if (thread!=std::this_thread::get_id() || busy) throw std::logic_error("Invalid frontend release thread"); return; }
        RetireMovie();Idle(); busy=true;
        try { Drain(); generation.reset(); current.reset(); retired_movie.reset(); released=true; busy=false; }
        catch (...) { busy=false; throw; }
    }
    ~Implementation() { try { Release(); } catch (...) { std::terminate(); } }
};
FrontendPacketRenderer::FrontendPacketRenderer(void (*drain)()) : impl_(std::make_unique<Implementation>(drain)) {}
FrontendPacketRenderer::~FrontendPacketRenderer()=default;
void FrontendPacketRenderer::Prepare(FrontendSession::Handle input)
{
    auto& s=*impl_; s.Idle(); (void)PreparePackets(input,s.movie_binding,s.retired_movie);
    if (SameOwners(s.current,input)) { s.current=std::move(input); return; }
    Require(!s.movie_binding,"Retire movie registration before replacing frontend resources");
    s.busy=true;
    try
    {
        // Drain first. An exception cannot retire the previous visible frame.
        s.Drain();
        auto next=std::make_unique<Generation>(input,s.generation.get()); next->Check(false);
        if (s.generation) s.generation->Check(true);
        s.generation.reset(); // Original ring now resolves candidate authored hashes.
        s.generation=std::move(next); s.current=std::move(input); s.retired_movie.reset(); s.busy=false;
    }
    catch (...) { s.busy=false; throw; }
}
FrontendMovieImageBinding::Handle FrontendPacketRenderer::BindMovie(std::shared_ptr<FrontendSession> session,
    std::uint32_t instance,std::shared_ptr<FrontendMoviePlayback> playback)
{
    auto& s=*impl_;s.Idle();Require(!s.movie_binding&&session&&playback,"Movie binding requires one live concrete provider");
    const auto frame=session->Current();Require(frame&&SameOwners(frame,s.current),"Prepare exact scene resources before movie binding");
    playback->Check();Require(playback->Status().state!=FrontendMovieState::Cancelled,"Cannot bind a cancelled movie");
    const std::array<std::string_view,2> path{"Layer","movie"};
    const auto selected=resources::FindFrontendNode(frame->graph,{},resources::FrontendNamedPath(path),resources::FrontendNodeType::Image);
    Require(selected&&selected->id==instance,"Movie instance differs from original active Layer/movie lookup");
    const auto node=std::find_if(frame->graph.instances.begin(),frame->graph.instances.end(),[&](const auto& item){return item.offset==instance;});
    Require(node!=frame->graph.instances.end()&&node->resource,"Authored movie instance lacks its resource");
    const auto resource=std::find_if(frame->graph.resources.begin(),frame->graph.resources.end(),[&](const auto& item){return item.offset==*node->resource;});
    Require(resource!=frame->graph.resources.end()&&resource->type==0,"Authored movie resource is not a texture");
    auto binding=std::shared_ptr<FrontendMovieImageBinding>(new FrontendMovieImageBinding);
    auto image=std::shared_ptr<resources::FrontendMovieImage>(new resources::FrontendMovieImage);
    image->width_=playback->Info().width;image->height_=playback->Info().height;image->instance_=instance;image->resource_=resource->offset;
    binding->playback_=std::move(playback);binding->session_=std::move(session);binding->frame_=frame;
    binding->instance_=instance;binding->resource_=resource->offset;binding->image_=std::move(image);
    binding->lifetime_=std::make_shared<FrontendMovieImageBinding::Lifetime>();
    s.busy=true;
    try
    {
        s.Drain();auto renderer=std::make_shared<FrontendMovieRenderer>(*s.generation->pool,binding->image_->Width(),binding->image_->Height(),s.drain);
        s.generation->expected_level=s.generation->pool->m_level;
        binding->lifetime_->graphics=renderer;s.movie_renderer=std::move(renderer);s.movie_binding=binding;s.busy=false;return binding;
    }
    catch(...){s.busy=false;throw;}
}
void FrontendPacketRenderer::RetireMovie(){impl_->RetireMovie();}
FrontendSession::Handle FrontendPacketRenderer::Current() const { impl_->Check(); return impl_->current; }
std::size_t FrontendPacketRenderer::Textures() const { impl_->Check(); return impl_->generation ? impl_->generation->textures.size() : 0; }
FrontendMoviePacketStatus FrontendPacketRenderer::MovieStatus() const
{
    auto& s=*impl_;s.Check();auto status=s.movie_status;
    status.current=s.current?PreparePackets(s.current,s.movie_binding,s.retired_movie).movies:FrontendMovieEntryStatus{};
    return status;
}
unsigned FrontendPacketRenderer::Submit(GLView& view,FrontendSession::Handle input)
{
    auto& s=*impl_; s.Check();
    if (!glIsFrameActive() || s.failed || (s.pending && *s.pending!=glNativeFrameGeneration())
        || !view.m_Interface || view.m_NativeIterating || view.m_CreateSorter!=CreateUnsortedPacketSorter)
        throw std::logic_error("Frontend submission requires an unsorted collecting view and a finished previous frame");
    Require(SameOwners(s.current,input),"Prepare changed frontend resources while idle before submission");
    const auto prepared=PreparePackets(input,s.movie_binding,s.retired_movie);
    const auto& packets=prepared.packets;
    s.busy=true;
    try
    {
        unsigned quads=0,movie_packets=0;
        if (!packets.empty()) { s.pending=glNativeFrameGeneration(); s.generation->submitted=true; }
        for (const auto& packet:packets)
        {
            if (const auto* text=std::get_if<detail::FontPackets>(&packet))
            { detail::AttachFontPackets(view,*text,0); quads+=text->quads.size(); }
            else if(const auto* movie=std::get_if<MoviePacket>(&packet))
            {
                s.movie_binding->Playback()->Check();
                const auto status=s.movie_binding->Playback()->Status().state;
                Require(status!=FrontendMovieState::Cancelled&&status!=FrontendMovieState::Failed,"Movie packet provider is inactive");
                // Before the first real decode, registration exists but no
                // movie frame is claimed or given a presentation receipt.
                if(auto frame=s.movie_binding->Playback()->Current())
                {s.movie_renderer->Submit(view,std::move(frame),movie->quad);++quads;++movie_packets;}
            }
            else if(std::holds_alternative<PendingMoviePacket>(packet)) continue;
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
        if(s.movie_status.frame_generation!=glNativeFrameGeneration())
        {s.movie_status={};s.movie_status.frame_generation=glNativeFrameGeneration();}
        s.movie_status.frame.awaiting_binding+=prepared.movies.awaiting_binding;
        s.movie_status.frame.awaiting_frame+=prepared.movies.awaiting_frame;
        s.movie_status.frame.cancelled+=prepared.movies.cancelled;
        s.movie_status.frame.ready+=prepared.movies.ready;
        s.movie_status.submitted_packets+=movie_packets;
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
    try
    {
        s.Drain();
        if(s.movie_renderer)
        {
            auto receipt=s.movie_renderer->FinishFrame();
            if(receipt&&s.movie_binding->Playback()->Status().state!=FrontendMovieState::Cancelled)
                s.movie_binding->Playback()->Acknowledge(receipt);
        }
        s.pending.reset();s.failed=false;s.busy=false;
    }
    catch (...) { s.busy=false; throw; }
}
void FrontendPacketRenderer::Release() { impl_->Release(); }
}
