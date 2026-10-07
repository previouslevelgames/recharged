#include "runtime/skin_render.h"
#include "runtime/skin_material.h"
#include "runtime/views.h"
#include "Game/SHierarchy.h"
#include "Game/GL/GLInventory.h"
#include "NL/gl/GLFontAtlas.h"
#include "NL/gl/gl.h"
#include "NL/gl/glMemory.h"
#include "NL/gl/glMatrix.h"
#include "NL/gl/glTexture.h"
#include "NL/gl/glTextureManager.h"
#include "NL/gl/glMaterialProgram.h"
#include "NL/glx/GXCharacterSkinCustomMaterialProgram.h"
#include "NL/glx/glxTexture.h"
#include "NL/nlString.h"
#include <algorithm>
#include <cmath>
#include <map>
#include <optional>
#include <set>
#include <thread>

namespace mscharged
{
namespace
{
constexpr std::uint32_t ShockTexture=0xe4457de5,MissingTexture=0xbea58af6;
bool Linked(const GLResourcePool* pool)
{
    auto* first=glGetResourcePools();if(!first)return false;auto* item=first;
    do{if(item==pool)return true;item=item->m_next;}while(item!=first);return false;
}
bool HashExists(std::uint32_t hash)
{
    auto* first=glGetResourcePools();if(!first)return false;auto* item=first;
    do{if(item->m_inventory->GetTexture(hash)||item->m_inventory->GetTextureAnim(hash))return true;item=item->m_next;}while(item!=first);return false;
}
resources::Texture MissingBindingTexture()
{
    // Original gl_FontStartup/glplatCreateFont medium atlas, genuinely required
    // by glx_BindTexture's missing-binding branch. Not an invented white image.
    resources::Texture out;out.id=MissingTexture;out.game_format=1;out.gx_format=5;out.levels=1;
    out.width=sFontTextureWidth[GLFONT_Medium];out.height=sFontTextureHeight[GLFONT_Medium];out.bits={255,255,255,255};
    if(out.width!=128||out.height!=256)throw std::logic_error("Original medium font dimensions changed");
    std::vector<unsigned short> image(std::size_t(out.width)*out.height);
    glBuildFixedFontImage(GLFONT_Medium,image.data());out.pixels.reserve(image.size()*2);
    // Original RGB5A3 swizzle: four-by-four tiles, each Wii word big endian.
    for(unsigned y=0;y<out.height;y+=4)for(unsigned x=0;x<out.width;x+=4)
        for(unsigned dy=0;dy<4;++dy)for(unsigned dx=0;dx<4;++dx)
        {const auto pixel=image[(y+dy)*out.width+x+dx];out.pixels.push_back(pixel>>8);out.pixels.push_back(pixel&255);}
    return out;
}
struct Geometry
{
    std::vector<std::array<float,3>> positions,normals;
    std::array<std::vector<std::array<std::int16_t,2>>,2> uv;
    std::vector<std::array<std::uint8_t,4>> bones;
    std::vector<std::array<float,4>> weights;
    std::array<std::uint32_t,2> textures;
};
}
struct SkinRenderer::Implementation
{
    struct Texture { resources::Texture source;PlatTexture* native=nullptr;std::uint16_t index=0xffff; };
    GLResourcePool& pool;RigidSkinAsset::Handle asset;void (*drain)();
    std::map<std::uint32_t,Texture> textures;
    std::vector<Geometry> geometry;
    SkinPoseFrame::Handle retained;
    std::optional<std::uint64_t> pending;
    std::thread::id thread=std::this_thread::get_id();
    GLResourceMark mark=0;int level=0;bool busy=false;
    Implementation(GLResourcePool& p,RigidSkinAsset::Handle a,std::span<const resources::Bytes> files,SkinRenderProfile profile,void(*d)())
        :pool(p),asset(std::move(a)),drain(d)
    {
        if(!drain||!asset||!Linked(&pool)||!glGetTextureManager()||glNativeViewDispatchActive()||glIsFrameActive()
            ||!glGetMaterialProgram(0x041c3281))
            throw std::logic_error("Skin renderer requires an idle graphics session, retained asset and real drain");
        if(files.empty()||files.size()>8)throw std::invalid_argument("Skin renderer requires one to eight real texture bundles");
        if(profile!=SkinRenderProfile::Authored&&profile!=SkinRenderProfile::BowserShock)
            throw std::invalid_argument("Unknown skin render profile");
        if(profile==SkinRenderProfile::BowserShock&&(asset->Data().hash!=0x99f34aae
            ||asset->Hierarchy()->Data().GetHashID()!=nlStringLowerHash("bowser")))
            throw std::invalid_argument("Source shock profile requires the authored Bowser shock model and hierarchy");
        std::map<std::uint32_t,resources::Texture> decoded;std::set<std::uint32_t> animations;std::size_t bytes=0;
        for(auto data:files)
        {
            bytes+=data.size();if(bytes>32*1024*1024)throw std::length_error("Skin texture inputs exceed 32 MiB");
            auto bundle=resources::ReadTextureBundle(data);
            for(auto& animation:bundle.animations)animations.insert(animation.id);
            for(auto& texture:bundle.textures)
                if(!decoded.emplace(texture.id,std::move(texture)).second)throw std::invalid_argument("Duplicate skin texture hash across source bundles");
        }
        if(profile==SkinRenderProfile::BowserShock)
        {
            if(decoded.contains(MissingTexture)||animations.contains(MissingTexture))
                throw std::invalid_argument("Original fixed-font fallback identity was supplied by another asset");
            decoded.emplace(MissingTexture,MissingBindingTexture());
        }
        geometry.reserve(asset->Data().packets.size());
        for(const auto& packet:asset->Data().packets)
        {
            Geometry value;
            for(unsigned i=0;i<2;++i)
            {
                auto hash=packet.material.textures[i].hash;
                if(profile==SkinRenderProfile::BowserShock)
                {
                    // CharacterLoader shock texture + DrawableCharacter's
                    // non-Mario CRP_Shock override changes diffuse only.
                    if(i==0)hash=ShockTexture;
                    else if(!decoded.contains(hash))hash=MissingTexture;
                }
                if(animations.contains(hash)||!decoded.contains(hash))
                    throw std::invalid_argument("Skin binding requires an actual static texture; animation/missing data is unavailable");
                value.textures[i]=hash;
            }
            for(const auto& vertex:packet.vertices)
            {
                value.positions.push_back(vertex.position);value.normals.push_back(vertex.normal);
                value.uv[0].push_back(vertex.uv[0]);value.uv[1].push_back(vertex.uv[1]);
                value.bones.push_back(vertex.bones);value.weights.push_back(vertex.weights);
            }
            geometry.push_back(std::move(value));
        }
        for(const auto& g:geometry)for(auto hash:g.textures)if(!textures.contains(hash))
        {
            if(hash==UINT32_MAX||HashExists(hash))throw std::invalid_argument("Skin texture hash collides with a live texture or animation");
            textures.emplace(hash,Texture{std::move(decoded.at(hash))});
        }
        mark=pool.MarkResource();level=pool.m_level;
        try
        {
            for(auto& [hash,t]:textures)
            {
                const auto& s=t.source;t.native=new(pool.Allocate(sizeof(PlatTexture),GLM_Header))PlatTexture;
                auto& n=*t.native;n.m_Width=s.width;n.m_Height=s.height;n.m_Levels=n.m_MaxLevel=s.levels;n.m_Format=static_cast<eGXTextureFormat>(s.game_format);
                n.m_nPaletteEntries=s.palette_entries;std::copy(s.bits.begin(),s.bits.end(),n.m_Bits);
                n.m_SwizzledData=const_cast<std::uint8_t*>(s.pixels.data());n.m_PaletteData=s.palette.empty()?nullptr:reinterpret_cast<u16*>(const_cast<std::uint8_t*>(s.palette.data()));
                n.m_NativeDataBytes=s.pixels.size();n.m_NativePaletteBytes=s.palette.size();glRegisterTexture(hash,&n,&pool);t.index=n.m_TextureIndex;
                if(t.index==0xffff||t.index>=glGetTextureManager()->mCapacity||glGetTextureManager()->mTextures[t.index]!=&n||glGetTextureIndex(hash)!=t.index)
                    throw std::logic_error("Skin texture registration failed to establish a genuine slot");
            }
        }
        catch(...){pool.ReleaseResource(mark);mark=0;throw;}
    }
    void Thread()const{if(thread!=std::this_thread::get_id()||busy)throw std::logic_error("Skin renderer requires its nonrecursive creating thread");}
    void Storage()const
    {
        if(!mark||!Linked(&pool)||!glGetTextureManager()||pool.m_level<level||glNativeViewDispatchActive())
            throw std::logic_error("Skin renderer storage is inactive or dispatching");
        for(const auto& [hash,t]:textures)
            if(pool.m_inventory->GetTexture(hash)!=t.native||t.index>=glGetTextureManager()->mCapacity
                ||glGetTextureManager()->mTextures[t.index]!=t.native||glGetTextureIndex(hash)!=t.index
                ||t.native->m_SwizzledData!=t.source.pixels.data()||t.native->m_NativeDataBytes!=t.source.pixels.size())
                throw std::logic_error("Skin texture ownership was replaced or mutated");
    }
    void Check()const{Thread();Storage();}
    void Release()
    {
        Thread();if(!mark)return;Storage();
        if(pending||glIsFrameActive()||pool.m_level!=level)throw std::logic_error("Finish skin frames and nested resource owners before release");
        busy=true;const auto generation=glNativeFrameGeneration();
        try
        {
            drain();Storage();if(glIsFrameActive()||generation!=glNativeFrameGeneration())throw std::logic_error("Skin drain changed the frame");
            pool.ReleaseResource(mark);mark=0;retained.reset();busy=false;
        }
        catch(...){busy=false;throw;}
    }
    ~Implementation(){try{Release();}catch(...){std::terminate();}}
};
SkinRenderer::SkinRenderer(GLResourcePool& pool,RigidSkinAsset::Handle asset,std::span<const resources::Bytes> files,SkinRenderProfile profile,void(*drain)())
    :impl_(std::make_unique<Implementation>(pool,std::move(asset),files,profile,drain)){}
SkinRenderer::~SkinRenderer()=default;
bool SkinRenderer::Active()const{impl_->Thread();return impl_->mark!=0;}
std::uint16_t SkinRenderer::TextureIndex(std::uint32_t hash)const{impl_->Check();return impl_->textures.at(hash).index;}
void SkinRenderer::Release(){impl_->Release();}
void SkinRenderer::FinishFrame()
{
    impl_->Check();if(!impl_->pending)return;
    if(glIsFrameActive()||*impl_->pending==glNativeFrameGeneration())throw std::logic_error("Send or cancel the original skin frame before finishing");
    impl_->busy=true;const auto generation=glNativeFrameGeneration();
    try
    {
        impl_->drain();impl_->Storage();if(glIsFrameActive()||generation!=glNativeFrameGeneration())throw std::logic_error("Skin drain changed the frame");
        impl_->pending.reset();impl_->retained.reset();impl_->busy=false;
    }
    catch(...){impl_->busy=false;throw;}
}
unsigned SkinRenderer::Submit(GLView& view,SkinPoseFrame::Handle frame,int layer)
{
    impl_->Check();
    if(!glIsFrameActive()||impl_->pending||!view.m_Interface||view.m_NativeIterating||layer<0)
        throw std::logic_error("Skin submission requires one collecting original frame");
    if(!frame||frame->asset!=impl_->asset||!frame->pose||frame->pose->hierarchy!=impl_->asset->Hierarchy()
        ||frame->packets.size()!=impl_->geometry.size())throw std::invalid_argument("Skin frame does not retain this exact qualified asset");
    for(unsigned p=0;p<frame->packets.size();++p)
    {
        if(frame->packets[p].size()!=impl_->asset->NodeMaps()[p].size())throw std::invalid_argument("Skin frame matrix count disagrees with its bone map");
        for(const auto& m:frame->packets[p])for(const auto& row:m.values)for(float x:row)
            if(!std::isfinite(x)||std::abs(x)>1e12f)throw std::invalid_argument("Invalid skin frame matrix");
    }
    impl_->busy=true;struct Leave{bool& busy;~Leave(){busy=false;}} leave{impl_->busy};
    impl_->pending=glNativeFrameGeneration();impl_->retained=std::move(frame);
    const auto& source=impl_->asset->Data();
    auto* model=new(glFrameAlloc(sizeof(glModel),GLM_Header))glModel{};
    model->id=source.hash;model->numPackets=source.packets.size();
    model->packets=static_cast<glModelPacket*>(glFrameAlloc(sizeof(glModelPacket)*source.packets.size(),GLM_Header));
    for(unsigned p=0;p<source.packets.size();++p)
    {
        auto& out=*new(model->packets+p)glModelPacket{};const auto& in=source.packets[p];auto& g=impl_->geometry[p];
        out.indexBuffer=const_cast<std::uint16_t*>(in.indices.data());out.numVertices=in.indices.size();out.numUniqueVertices=in.vertices.size();out.primType=in.primitive;out.numStreams=6;
        out.streams=static_cast<glModelStream*>(glFrameAlloc(sizeof(glModelStream)*6,GLM_Header));
        const std::array<void*,6> addresses{g.positions.data(),g.normals.data(),g.uv[0].data(),g.uv[1].data(),g.bones.data(),g.weights.data()};
        constexpr std::array<unsigned,6> ids{1,2,4,4,7,5},strides{12,12,4,4,4,16};
        for(unsigned s=0;s<6;++s)new(out.streams+s)glModelStream{addresses[s],static_cast<u8>(s),static_cast<u8>(strides[s]),static_cast<u8>(ids[s]),0};
        nlMatrix4 model_matrix;std::copy(in.matrix.begin(),in.matrix.end(),model_matrix.e);out.matrix=glAllocSetMatrix(model_matrix);out.rasterState=in.raster;
        out.materialProgram=glGetMaterialProgram(0x041c3281);
        auto* matrices=static_cast<float(*)[3][4]>(glFrameAlloc(impl_->retained->packets[p].size()*48,GLM_Matrix));
        for(unsigned b=0;b<impl_->retained->packets[p].size();++b)for(unsigned r=0;r<3;++r)for(unsigned c=0;c<4;++c)matrices[b][r][c]=impl_->retained->packets[p][b].values[r][c];
        auto* material=new(glFrameAlloc(sizeof(GXCharacterSkinCustomParameters),GLM_Header))GXCharacterSkinCustomParameters{};
        material->skinMatrices=matrices;material->skinMatrixBytes=impl_->retained->packets[p].size()*48;
        material->blendAmount=in.material.blend;material->alphaValue=in.material.alpha;material->shadowLevel=in.material.shadow_level;material->lightingEnabled=in.material.lighting_enabled;
        for(unsigned slot=0;slot<2;++slot)
        {
            auto& binding=slot?material->detailTexture:material->diffuseTexture;
            binding.texture=g.textures[slot];binding.textureIndex=impl_->textures.at(binding.texture).index;
            binding.flags=in.material.textures[slot].flags;binding.unknown07=0;
        }
        out.materialParameters=material;ValidateNativeSkinPacket(out);
        // Original Prepare derives alpha testing/blending/depth-write state
        // from the genuinely registered diffuse texture's channel metadata.
        static_cast<GLMaterialProgram*>(out.materialProgram)->Prepare(&out);
    }
    view.AttachModel(model,layer);return model->numPackets;
}
}
