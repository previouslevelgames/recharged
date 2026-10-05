#include "runtime/effects_vertex.h"
#include "runtime/static_inventory.h"
#include "runtime/materials.h"
#include "runtime/graphics_memory.h"
#include "runtime/views.h"
#include "Game/GL/GLVertexAnim.h"
#include "Game/GL/GLInventory.h"
#include "NL/gl/gl.h"
#include "NL/gl/glMemory.h"
#include "NL/gl/glTexture.h"
#include "NL/gl/glTextureManager.h"
#include "NL/glx/glxTexture.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <thread>

namespace mscharged
{
namespace
{
void Require(bool condition, const char* message)
{ if (!condition) throw std::logic_error(message); }
bool Linked(const GLResourcePool* pool)
{
    const auto* first=glGetResourcePools();if(!first)return false;
    auto* current=first;do{if(current==pool)return true;current=current->m_next;}while(current!=first);
    return false;
}
struct CurrentPool
{
    GLResourcePool* previous=glGetCurrentResourcePool();
    explicit CurrentPool(GLResourcePool* next){glSetCurrentResourcePool(next);}
    ~CurrentPool(){glSetCurrentResourcePool(previous);}
};
void Validate(const resources::EffectsGeometry& geometry,const resources::TextureBundle& textures)
{
    Require(!geometry.models.empty()&&geometry.models.size()<=4096
        &&!geometry.animations.empty()&&geometry.animations.size()<=4096,"Invalid effects geometry collection size");
    Require(textures.animations.empty(),"Effects vertex profile does not schedule texture animations");
    Require(textures.textures.size()<=4096,"Effects texture count exceeds its budget");
    std::size_t texture_bytes=0;
    std::set<std::uint32_t> texture_ids,model_ids,animation_ids;
    for(const auto& texture:textures.textures)
    {
        const unsigned formats[]={4,5,14,6,1,0,1,3,9};
        Require(texture.width&&texture.width<=1024&&texture.height&&texture.height<=1024
            &&texture.game_format<std::size(formats)&&texture.gx_format==formats[texture.game_format],
            "Invalid effects texture dimensions/format");
        unsigned levels=1;for(unsigned size=std::max(texture.width,texture.height);size>1;size>>=1)++levels;
        Require(texture.levels&&texture.levels<=levels&&texture.palette.size()==2u*texture.palette_entries
            &&(texture.game_format==8?(texture.palette_entries&&texture.palette_entries<=256):!texture.palette_entries),
            "Invalid effects texture mip/palette metadata");
        std::size_t size=0;const unsigned format=texture.game_format;
        for(unsigned level=0;level<texture.levels;++level)
        {
            const unsigned width=std::max(1u,unsigned(texture.width)>>level),height=std::max(1u,unsigned(texture.height)>>level);
            const unsigned bw=(format==2||format==4||format==5||format==6||format==8)?8:4;
            const unsigned bh=(format==2||format==5)?8:4;
            size+=((width+bw-1)/bw)*((height+bh-1)/bh)*(format==3?64:32);
        }
        Require(texture.pixels.size()==size&&size+texture.palette.size()<=resources::MaximumAssetBytes-texture_bytes,
            "Effects texture tiled extent exceeds its retained bytes/budget");
        texture_bytes+=size+texture.palette.size();
        Require(texture.id!=0xffffffffU&&texture_ids.insert(texture.id).second,"Duplicate or global-white effects texture ID");
        Require(glGetTextureIndex(texture.id)==0xffff,"Effects texture or animation hash is already registered");
    }
    std::size_t total_vertices=0;
    for(const auto& model:geometry.models)
    {
        Require(model_ids.insert(model.id).second&&!model.packets.empty()&&model.packets.size()<=4096,"Invalid effects model or duplicate ID");
        for(const auto id:MaterialLookupTextures(model))
            Require(texture_ids.contains(id),"Effects model references a texture absent from its retained batch");
        for(const auto& packet:model.packets)
        {
            Require(packet.material.program==0x19065bf6||packet.material.program==0xee9d919d,
                "Effects vertex profile requires original float-textured or constant-colour material");
            Require(!packet.vertices.empty()&&packet.vertices.size()<=65535&&!packet.indices.empty()
                &&packet.indices.size()<=65535,"Invalid effects packet vertex/index count");
            for(const auto& vertex:packet.vertices)
            {
                for(float component:vertex.position)Require(std::isfinite(component)&&std::abs(component)<=1e7f,"Invalid effects base position");
                for(float component:vertex.uv)Require(std::isfinite(component)&&std::abs(component)<=1e7f,"Invalid effects base UV");
            }
            for(auto index:packet.indices)Require(index<packet.vertices.size(),"Effects model index exceeds retained vertices");
        }
    }
    for(const auto& animation:geometry.animations)
    {
        Require(animation_ids.insert(animation.model).second,"Duplicate effects animation ID");
        const auto model=std::find_if(geometry.models.begin(),geometry.models.end(),[&](const auto& m){return m.id==animation.model;});
        Require(model!=geometry.models.end(),"Effects animation has no retained model");
        Require(animation.frames&&animation.frames<=4096&&animation.vertices&&animation.stride==12
            &&animation.streams==std::vector<std::uint32_t>{1},"Unsupported effects position-animation dimensions/streams");
        std::size_t vertices=0;for(const auto& packet:model->packets)vertices+=packet.vertices.size();
        Require(vertices==animation.vertices&&std::uint64_t(animation.frames)*vertices==animation.positions.size(),
            "Effects animation does not match its packet/frame vertex layout");
        Require(animation.positions.size()<=1024*1024-total_vertices,"Effects animation data exceeds its retained budget");
        total_vertices+=animation.positions.size();
        for(const auto& position:animation.positions)for(float value:position)
            Require(std::isfinite(value)&&std::abs(value)<=1e7f,"Invalid effects animated position");
    }
}
}
struct EffectsVertexResources::Implementation
{
    struct Record{GLVertexAnim* animation;glModel* model;u8* vertices;unsigned frames,count;};
    struct Texture{std::uint32_t id;PlatTexture* texture;u16 index;};
    std::map<std::uint32_t,Record> records;
    std::vector<Texture> textures;
    GLResourcePool* pool=nullptr;
    std::unique_ptr<StaticInventory> inventory;
    std::function<void()> drain;
    std::thread::id thread=std::this_thread::get_id();
    std::optional<std::uint64_t> pending;
    bool busy=false;
    Implementation(const resources::EffectsGeometry& geometry,const resources::TextureBundle& source,
        std::function<void()> callback,EffectsVertexMemory memory):drain(std::move(callback))
    {
        Require(drain&&glGetTextureManager()&&!glIsFrameActive()&&!glNativeViewDispatchActive(),
            "Effects vertex resources require idle graphics memory and a real drain callback");
        constexpr std::size_t maximum=resources::MaximumAssetBytes;
        Require(memory.headers&&memory.geometry&&memory.textures&&memory.headers<=maximum
            &&memory.geometry<=maximum&&memory.textures<=maximum,"Invalid effects vertex resource pool budgets");
        Validate(geometry,source);textures.reserve(source.textures.size());
        const GLMemoryRequirement requirements[]={{GLM_Header,static_cast<unsigned long>(memory.headers)},
            {GLM_VertexData,static_cast<unsigned long>(memory.geometry)},
            {GLM_TextureData,static_cast<unsigned long>(memory.textures)}};
        ScopedGameAllocator allocation(VirtualAllocator);
        try
        {
            pool=glCreateResourcePool(requirements,3,"Effects vertex resources");CurrentPool selected(pool);
            inventory=std::make_unique<StaticInventory>(*pool,geometry.models,source.textures);
            for(const auto& texture:source.textures)
            {
                auto* native=pool->m_inventory->GetTexture(texture.id);auto* manager=glGetTextureManager();
                Require(native&&native->m_TextureIndex<manager->mCapacity&&manager->mTextures[native->m_TextureIndex]==native
                    &&glGetTextureIndex(texture.id)==native->m_TextureIndex,"Effects texture registration did not establish its binding");
                textures.push_back({texture.id,native,native->m_TextureIndex});
            }
            for(const auto& source:geometry.animations)
            {
                // Original constructor reads a host-order six-word record. The
                // disk reader has already kept the Wii layout separate.
                const u32 header[]={source.model,source.frames,source.vertices,source.stride,source.unknown,1};
                const s32 stream=1;
                auto* animation=new (8,false) GLVertexAnim(header,&stream);
                try
                {
                    animation->m_pModel=inventory->Model(source.model);
                    auto* vertices=static_cast<u8*>(pool->Allocate(source.positions.size()*sizeof(source.positions[0]),GLM_VertexData));
                    std::memcpy(vertices,source.positions.data(),source.positions.size()*sizeof(source.positions[0]));
                    animation->m_pVertices=vertices;
                    auto [entry,inserted]=records.emplace(source.model,Record{animation,animation->m_pModel,vertices,source.frames,source.vertices});
                    Require(inserted,"Duplicate effects animation publication");
                    try{pool->m_inventory->AddVertexAnim(source.model,animation);}
                    catch(...){records.erase(entry);throw;}
                }
                catch(...){nlDeleteGameObject(animation);throw;}
            }
        }
        catch(...){inventory.reset();if(pool)glDestroyResourcePool(pool);pool=nullptr;throw;}
    }
    void Thread() const
    {Require(thread==std::this_thread::get_id()&&!busy,"Effects vertex resources require their nonrecursive owner thread");}
    void Check() const
    {
        Thread();Require(pool&&Linked(pool)&&glGetTextureManager()&&pool->m_level==1&&pool->m_inventory->m_nLevel==1,
            "Effects vertex resource pool was released or its ownership level changed");
        Require(!glNativeViewDispatchActive(),"Effects vertex resources cannot mutate during view dispatch");
        for(const auto& texture:textures)
            Require(pool->m_inventory->GetTexture(texture.id)==texture.texture&&texture.index<glGetTextureManager()->mCapacity
                &&glGetTextureManager()->mTextures[texture.index]==texture.texture&&glGetTextureIndex(texture.id)==texture.index,
                "Effects texture binding was replaced or released");
        for(const auto& [id,record]:records)
        {
            const auto* a=record.animation;
            Require(pool->m_inventory->GetVertexAnim(id)==a&&pool->m_inventory->GetModel(id)==record.model
                &&a->m_pModel==record.model&&a->m_pVertices==record.vertices&&a->m_nNumFrames==record.frames
                &&a->m_nNumVertices==record.count&&a->m_nVertexStride==12&&a->m_nNumAnimatedStreams==1
                &&a->m_pAnimatedStreamIDs&&a->m_pAnimatedStreamIDs[0]==1,"Effects animation native binding changed");
            Require(std::isfinite(a->m_fFrame)&&a->m_fFrame>=0&&a->m_fFrame<a->m_nNumFrames
                &&std::isfinite(a->m_fTimeScale)&&a->m_fTimeScale>=0&&a->m_fFrameRate==30
                &&(a->m_eMode==GLVAnimMode_Loop||a->m_eMode==GLVAnimMode_Hold),"Effects animation playback state is invalid");
        }
    }
    void Idle() const{Check();Require(!pending&&!glIsFrameActive(),"Finish effects vertex frames before changing playback or ownership");}
    GLVertexAnim& Find(std::uint32_t id) const
    {const auto found=records.find(id);if(found==records.end())throw std::out_of_range("Effects vertex animation ID is absent");return *found->second.animation;}
    void Release()
    {
        Thread();if(!pool)return;Idle();busy=true;
        try{drain();inventory.reset();records.clear();textures.clear();glDestroyResourcePool(pool);pool=nullptr;drain={};busy=false;}
        catch(...){busy=false;throw;}
    }
    ~Implementation(){try{Release();}catch(...){std::terminate();}}
};
EffectsVertexResources::EffectsVertexResources(const resources::EffectsGeometry& geometry,const resources::TextureBundle& textures,
    std::function<void()> drain,EffectsVertexMemory memory):impl_(std::make_unique<Implementation>(geometry,textures,std::move(drain),memory)){}
EffectsVertexResources::~EffectsVertexResources()=default;
bool EffectsVertexResources::Active() const{impl_->Thread();return impl_->pool;}
std::size_t EffectsVertexResources::Size() const{impl_->Check();return impl_->records.size();}
EffectsVertexState EffectsVertexResources::State(std::uint32_t id) const
{impl_->Check();const auto& a=impl_->Find(id);return{a.m_nNumFrames,a.m_fFrame,a.m_fTimeScale,static_cast<EffectsVertexMode>(a.m_eMode),a.m_bDone};}
void EffectsVertexResources::Configure(std::uint32_t id,EffectsVertexMode mode,float speed)
{
    impl_->Idle();auto& a=impl_->Find(id);
    Require((mode==EffectsVertexMode::Loop||mode==EffectsVertexMode::Hold)&&std::isfinite(speed)&&speed>=0,
        "Invalid effects animation mode or speed");
    a.m_eMode=static_cast<eGLVertAnimMode>(mode);a.m_fTimeScale=speed;
}
void EffectsVertexResources::Reset(std::uint32_t id){impl_->Idle();auto& a=impl_->Find(id);a.m_fFrame=0;a.m_bDone=false;}
void EffectsVertexResources::Update(float delta)
{
    impl_->Idle();Require(std::isfinite(delta)&&delta>=0,"Invalid effects vertex animation delta");
    // Preflight every animation before any advances. Preserve source float
    // multiplication order and its reset-to-zero overshoot behavior.
    for(const auto& [id,record]:impl_->records)
    {
        const auto& a=*record.animation;if(a.m_bDone)continue;
        const double scaled=double(a.m_fTimeScale)*delta;
        Require(scaled<=std::numeric_limits<float>::max(),"Effects animation scaled delta overflows");
        const float step=a.m_fTimeScale*delta;
        const double advance=double(a.m_fFrameRate)*step;
        Require(advance<=std::numeric_limits<float>::max()&&double(a.m_fFrame)+static_cast<float>(advance)<=std::numeric_limits<float>::max(),
            "Effects animation frame advance overflows");
    }
    for(const auto& [id,record]:impl_->records)record.animation->Update(delta);
}
glModel* EffectsVertexResources::Model(std::uint32_t id,int frame)
{
    impl_->Check();auto& animation=impl_->Find(id);
    Require(frame==-1||(frame>=0&&unsigned(frame)<animation.m_nNumFrames),"Invalid explicit effects animation frame");
    Require(glIsFrameActive()&&(!impl_->pending||*impl_->pending==glNativeFrameGeneration()),
        "Effects frame binding requires a collecting frame and drained previous generation");
    impl_->pending=glNativeFrameGeneration();return animation.GetModel(frame);
}
void EffectsVertexResources::FinishFrame()
{
    impl_->Check();if(!impl_->pending)return;
    Require(!glIsFrameActive()&&*impl_->pending!=glNativeFrameGeneration(),"Send or cancel the effects frame before finishing");
    impl_->busy=true;try{impl_->drain();impl_->pending.reset();impl_->busy=false;}catch(...){impl_->busy=false;throw;}
}
void EffectsVertexResources::Release(){impl_->Release();}
}
