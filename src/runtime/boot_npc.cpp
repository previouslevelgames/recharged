#include "runtime/boot_npc.h"
#include "runtime/whole_file.h"
#include "runtime/graphics_memory.h"
#include "runtime/views.h"
#include "resources/compressed_asset.h"
#include "resources/chunk_reader.h"
#include "resources/texture_bundle.h"
#include "Game/Render/NPCLoadSteps.h"
#include "Game/SHierarchy.h"
#include "Game/GL/GLInventory.h"
#include "Game/GL/GLTextureAnim.h"
#include "NL/gl/gl.h"
#include "NL/gl/glMemory.h"
#include "NL/gl/glMatrix.h"
#include "NL/gl/glTexture.h"
#include "NL/gl/glTextureManager.h"
#include "NL/gl/glMaterialProgram.h"
#include "NL/glx/GXCharacterSkinCustomMaterialProgram.h"
#include "NL/glx/glxTexture.h"
#include "NL/nlFile.h"
#include "NL/nlFileGC.h"
#include "NL/nlMemory.h"
#include "NL/nlString.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <limits>
#include <thread>

namespace mscharged
{
namespace
{
void Require(bool condition,const char* message){if(!condition)throw std::logic_error(message);}
bool Linked(const GLResourcePool* pool)
{const auto* first=glGetResourcePools();if(!first)return false;auto* p=first;do{if(p==pool)return true;p=p->m_next;}while(p!=first);return false;}
std::string Path(const char* format,const std::string& name)
{char output[256];const auto n=std::snprintf(output,sizeof(output),format,name.c_str(),name.c_str());if(n<0||n>=int(sizeof(output)))throw std::length_error("NPC source path exceeds its buffer");return output;}
void Name(std::string_view name)
{
    if(name.empty()||name.size()>=40||!std::all_of(name.begin(),name.end(),[](unsigned char c){return(c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='_';}))
        throw std::invalid_argument("NPC template name must fit its original 40-byte field without path components");
}
struct FreeBuffer{void operator()(void* p)const noexcept{nlFree(p);}};
template<class T>T* Array(GLResourcePool& pool,std::size_t count,eGLMemory kind)
{
    if(!count||count>resources::MaximumAssetBytes/sizeof(T))throw std::length_error("NPC native array exceeds its checked capacity");
    auto* p=static_cast<T*>(pool.Allocate(count*sizeof(T),kind));for(std::size_t i=0;i<count;++i)new(p+i)T{};return p;
}
std::string Hex(std::uint32_t value){char out[11];std::snprintf(out,sizeof(out),"0x%08x",value);return out;}
}
struct BootNpcResources::Implementation
{
    struct Record
    {
        std::string name;bool persistent=false,loaded=false;
        bool mAnimationLoadStarted=false,mAnimationsLoaded=false,mHierarchyLoaded=false,mTexturesLoaded=false;
        unsigned long modelID=~0UL;
        GLResourcePool* pool=nullptr;GLInventory* inventory=nullptr;GLResourceMark mark=0;int level=0;
        BootNpcTemplate::Handle published;
        glModel* native=nullptr;
    };
    struct Iterator
    {
        const std::vector<Record*>* values;std::size_t index=0;
        bool hasNext()const{return index<values->size();}Record* operator*()const{return values->at(index);}void next(){++index;}
    };
    struct Request
    {
        Implementation* owner;unsigned index,token=0;bool admitted=false,complete=false;
        std::vector<std::uint8_t> bytes;
    };
    std::vector<std::unique_ptr<Record>> records;
    std::array<std::vector<Record*>,2> ordered;
    std::vector<Record*> registrations;
    std::array<Request,4> requests{{{this,0},{this,1},{this,2},{this,3}}};
    Record* pending=nullptr;GLResourcePool* persistent_pool=nullptr;
    BootNpcState state=BootNpcState::Idle;std::exception_ptr error;
    const std::thread::id thread=std::this_thread::get_id();bool busy=false;
    std::size_t bytes=0;
    void Check()const{Require(thread==std::this_thread::get_id()&&!busy,"NPC templates require their nonrecursive NL/GL owner thread");}
    struct Guard{Implementation& s;explicit Guard(Implementation& value):s(value){s.busy=true;}~Guard(){s.busy=false;}};
    void Idle()const{Require(!glIsFrameActive()&&!glNativeViewDispatchActive(),"NPC resources require an idle drained frame");}
    void Storage(const Record& record,bool top=false)const
    {
        Require(record.pool&&Linked(record.pool)&&record.pool->m_inventory==record.inventory,
            "NPC captured graphics inventory was released or replaced");
        Require(record.pool->m_level==record.inventory->m_nLevel&&(!record.mark||record.pool->m_level>=record.level)
            &&(!top||record.pool->m_level==record.level),"NPC graphics resource markers changed externally");
        if(record.loaded)Require(record.pool->m_inventory->GetModel(record.modelID)==record.native,
            "NPC native model registration was replaced");
    }
    void Create(GLResourcePool& pool,std::string_view name,bool persistent)
    {
        Check();Idle();Name(name);
        Require(state!=BootNpcState::Selected&&state!=BootNpcState::Loading&&state!=BootNpcState::Failed,
            "Create NPC templates outside an active or failed load");
        Require(Linked(&pool)&&pool.m_inventory,"NPC persistent resource pool is unavailable");
        Require(!persistent_pool||persistent_pool==&pool,"NPC templates cannot change persistent pool identity");
        if(records.size()>=64)throw std::length_error("NPC template profile exceeds 64 retained records");
        auto value=std::make_unique<Record>();value->name=name;value->persistent=persistent;
        // Reserve before publishing either list, preserving original insertion
        // order even if native host allocation fails.
        auto& list=ordered[persistent?0:1];list.reserve(list.size()+1);records.reserve(records.size()+1);
        list.push_back(value.get());records.push_back(std::move(value));persistent_pool=&pool;state=BootNpcState::Idle;
    }
    bool Select()
    {
        Check();Idle();Require(state!=BootNpcState::Loading&&state!=BootNpcState::Failed,
            "NPC selection cannot replace pending or failed callback ownership");
        pending=nullptr;
        for(auto& list:ordered)if(SelectPendingNPCFromList(Iterator{&list},pending)){state=BootNpcState::Selected;return true;}
        state=BootNpcState::Registered;return false;
    }
    void CancelRequests()
    {for(auto& r:requests){if(r.token)nlCancelEntireFileLoad(r.token,nullptr);r.token=0;}}
    static void Complete(void* data,unsigned long size,void* context)
    {
        auto& r=*static_cast<Request*>(context);auto& s=*r.owner;std::unique_ptr<void,FreeBuffer> owned(data);
        Require(s.thread==std::this_thread::get_id(),"NPC completion arrived on the wrong thread");
        r.token=0;r.complete=true;if(s.error)return;
        try
        {
            Require(s.pending&&s.state==BootNpcState::Loading&&r.admitted,"Stale NPC completion identity");
            Require(data&&size&&size<=resources::MaximumAssetBytes,"NPC admitted read failed or exceeded its extent");
            if(size>32*1024*1024-s.bytes)throw std::length_error("NPC batch exceeds 32 MiB retained bytes");
            s.bytes+=size;r.bytes.assign(static_cast<const std::uint8_t*>(data),static_cast<const std::uint8_t*>(data)+size);
        }
        catch(...){s.error=std::current_exception();}
    }
    void Begin()
    {
        Check();Idle();Require(state==BootNpcState::Selected&&pending,"Select the next original NPC template before Begin");
        Require(gMemoryInitialized&&nlFileSystemReady(),"NPC loading requires real NL files and game arenas");
        Guard guard(*this);auto& record=*pending;record.pool=record.persistent?persistent_pool:glGetCurrentResourcePool();
        Require(Linked(record.pool)&&record.pool->m_inventory,"NPC captured pool is unavailable");record.inventory=record.pool->m_inventory;
        record.level=record.pool->m_level;Storage(record,true);
        const std::array<std::string,4> paths{Path(NPCAnimationPathFormat(),record.name),Path(NPCHierarchyPathFormat(),record.name),
            Path(NPCTexturesPathFormat(),record.name),Path(NPCModelsPathFormat(),record.name)};
        bytes=0;error={};for(auto& r:requests){r.bytes.clear();r.admitted=r.complete=false;r.token=0;}
        state=BootNpcState::Loading;
        try
        {
            std::array<std::size_t,4> lengths{};std::size_t total=0;
            for(unsigned i=0;i<4;++i)
            {
                std::unique_ptr<nlFile> file(nlOpen(paths[i].c_str()));
                if(!file){if(i==0)continue;throw std::runtime_error("NPC "+record.name+" missing required file: "+paths[i]);}
                lengths[i]=nlFileSize(file.get(),nullptr);
                if(!lengths[i]||lengths[i]>resources::MaximumAssetBytes||lengths[i]>32*1024*1024-total)
                    throw std::length_error("NPC "+record.name+" invalid admitted file size: "+paths[i]);
                total+=lengths[i];
            }
            record.mAnimationLoadStarted=lengths[0]!=0;
            ScopedGameAllocator scope(VirtualAllocator);
            for(auto& r:requests)
            {
                if(!lengths[r.index])continue;r.admitted=true;
                // Original hierarchy/SAnim buffers use StandardAllocator, GL
                // request buffers use the explicit virtual scope. No allocator
                // is left pushed while unrelated NL callbacks run.
                r.token=nlLoadEntireFileAsync(paths[r.index].c_str(),Complete,&r,32,AllocateStart,nullptr,0,
                    r.index<2?&StandardAllocator:&VirtualAllocator);
                if(!r.token&&!r.complete)throw std::runtime_error("NPC "+record.name+" file admission failed");
                if(error)std::rethrow_exception(error);
            }
        }
        catch(...){error=std::current_exception();state=BootNpcState::Failed;CancelRequests();throw;}
    }
    void RegisterTexture(GLResourcePool& pool,const resources::Texture& source)
    {
        if(source.id==UINT32_MAX||glGetTextureIndex(source.id)!=0xffff)throw std::invalid_argument("NPC texture hash collides with an existing registration");
        auto* n=Array<PlatTexture>(pool,1,GLM_Header);n->m_Width=source.width;n->m_Height=source.height;
        n->m_Levels=n->m_MaxLevel=source.levels;n->m_Format=static_cast<eGXTextureFormat>(source.game_format);
        n->m_nPaletteEntries=source.palette_entries;std::copy(source.bits.begin(),source.bits.end(),n->m_Bits);
        n->m_SwizzledData=Array<unsigned char>(pool,source.pixels.size(),GLM_TextureData);
        std::memcpy(n->m_SwizzledData,source.pixels.data(),source.pixels.size());n->m_NativeDataBytes=source.pixels.size();n->m_NativePaletteBytes=source.palette.size();
        if(!source.palette.empty()){n->m_PaletteData=Array<u16>(pool,source.palette_entries,GLM_TextureData);std::memcpy(n->m_PaletteData,source.palette.data(),source.palette.size());}
        glRegisterTexture(source.id,n,&pool);
        Require(n->m_TextureIndex!=0xffff&&glGetTextureIndex(source.id)==n->m_TextureIndex,"NPC texture registration did not establish an original slot");
    }
    void RegisterAnimation(GLResourcePool& pool,const resources::TextureAnimation& source)
    {
        if(glGetTextureIndex(source.id)!=0xffff)throw std::invalid_argument("NPC texture animation hash collides with an existing registration");
        if(!glGetTextureManager()->mFreeIndices->mCount)throw std::length_error("NPC texture animation manager is full");
        auto* n=Array<GLTextureAnim>(pool,1,GLM_Header);n->m_nFrame=0;n->m_uHashID=source.id;
        n->m_nNumTextures=n->m_NativeFrameCount=source.frames.size();n->m_ePlayMode=static_cast<eGLTexAnimMode>(source.mode);
        n->m_nPlayDir=source.direction;n->m_bPaused=source.paused;n->m_fTime=source.elapsed;n->m_textureIndex=0xffff;
        n->m_pAnimTex=Array<GLAnimTex>(pool,source.frames.size(),GLM_Header);
        for(unsigned i=0;i<source.frames.size();++i)
        {
            auto* texture=pool.m_inventory->GetTexture(source.frames[i].texture);
            Require(texture&&texture->m_TextureIndex!=0xffff,"NPC texture animation frame lacks its actual texture");
            n->SetTexture(i,{texture->m_TextureIndex,source.frames[i].duration});
        }
        pool.m_inventory->AddTextureAnim(source.id,n);glGetTextureManager()->RegisterTextureAnim(n);
    }
    glModel* RegisterModel(GLResourcePool& pool,const resources::RigidSkinModel& source)
    {
        if(source.hash==UINT32_MAX||pool.m_inventory->GetModel(source.hash))throw std::invalid_argument("NPC model hash is reserved or already registered");
        auto* program=glGetMaterialProgram(0x041c3281);Require(program,"Original CharacterSkinCustom material provider is unavailable");
        auto* model=Array<glModel>(pool,1,GLM_Header);model->id=source.hash;model->numPackets=source.packets.size();
        model->packets=Array<glModelPacket>(pool,model->numPackets,GLM_Header);
        for(unsigned p=0;p<source.packets.size();++p)
        {
            const auto& in=source.packets[p];auto& out=model->packets[p];out.numVertices=in.indices.size();out.numUniqueVertices=in.vertices.size();
            out.primType=in.primitive;out.rasterState=in.raster;out.numStreams=6;out.streams=Array<glModelStream>(pool,6,GLM_Header);
            out.indexBuffer=Array<u16>(pool,in.indices.size(),GLM_IndexData);std::copy(in.indices.begin(),in.indices.end(),out.indexBuffer);
            constexpr unsigned ids[]{1,2,4,4,7,5},strides[]{12,12,4,4,4,16};
            for(unsigned s=0;s<6;++s)
            {
                auto& stream=out.streams[s];stream.id=ids[s];stream.index=s;stream.stride=strides[s];
                auto* dst=Array<unsigned char>(pool,in.vertices.size()*strides[s],GLM_VertexData);stream.address=dst;
                for(unsigned v=0;v<in.vertices.size();++v)
                {
                    const auto& vertex=in.vertices[v];const void* src=s==0?static_cast<const void*>(vertex.position.data()):s==1?vertex.normal.data():
                        s<4?static_cast<const void*>(vertex.uv[s-2].data()):s==4?static_cast<const void*>(vertex.bones.data()):vertex.weights.data();
                    std::memcpy(dst+v*strides[s],src,strides[s]);
                }
            }
            auto* matrix=Array<nlMatrix4>(pool,1,GLM_Matrix);std::copy(in.matrix.begin(),in.matrix.end(),matrix->e);out.matrix=reinterpret_cast<glMatrixHandle>(matrix);
            out.materialProgram=program;auto* material=Array<GXCharacterSkinCustomParameters>(pool,1,GLM_Header);
            // Source skin setup fills these only for an actual posed draw.
            // Null is explicit unavailable pose data, never identity success.
            material->skinMatrices=nullptr;material->skinMatricesSize=0;material->blendAmount=in.material.blend;material->alphaValue=in.material.alpha;
            material->shadowLevel=in.material.shadow_level;material->lightingEnabled=in.material.lighting_enabled;
            for(unsigned i=0;i<2;++i){auto& t=i?material->detailTexture:material->diffuseTexture;t.texture=in.material.textures[i].hash;t.textureIndex=0xffff;t.flags=in.material.textures[i].flags;t.unknown07=0;}
            out.materialParameters=material;
        }
        pool.m_inventory->AddModel(source.hash,model);return model;
    }
    bool Finish()
    {
        Check();Idle();Require(pending&&state==BootNpcState::Loading,"NPC finalize needs its exact admitted request");Storage(*pending,true);Guard guard(*this);
        try
        {
            if(error)std::rethrow_exception(error);
            for(const auto& r:requests)if(r.admitted&&!r.complete)return false;
            auto result=std::make_shared<BootNpcTemplate>();auto& record=*pending;result->name=record.name;result->persistent=record.persistent;result->animation_admitted=record.mAnimationLoadStarted;
            result->hierarchy=HierarchyAsset::Decode(requests[1].bytes);
            Require(result->hierarchy->Data().GetHashID()==nlStringLowerHash(record.name.c_str()),"NPC hierarchy does not match the original lowercase template hash");
            if(record.mAnimationLoadStarted)
            {
                const auto decoded=resources::InflateAsset(requests[0].bytes);SAnimAssets animations(decoded);
                for(std::size_t i=0;i<animations.Size();++i)result->animations.push_back(animations.At(i));
            }
            const auto textures=resources::ReadTextureBundle(requests[2].bytes);
            try{result->skin=RigidSkinAsset::Decode(requests[3].bytes,result->hierarchy);}
            catch(const resources::UnsupportedResource& failure)
            {
                // Include the exact source packet/program identity without
                // guessing a render-equivalent material or dropping its model.
                const auto root=resources::ReadChunk(requests[3].bytes,0,requests[3].bytes.size());std::string programs;
                if(root.id==0x8001b000)
                    for(auto at=std::size_t(root.payload.data()-requests[3].bytes.data()),end=at+root.payload.size();at<end;)
                    {const auto c=resources::ReadChunk(requests[3].bytes,at,end);if(c.id==0x1b004)for(std::size_t p=0;p+48<=c.payload.size();p+=48)programs+=" "+Hex(resources::U32(c.payload,p+16));at=c.next;}
                throw resources::UnsupportedResource("NPC "+record.name+" material/program"+programs+": "+failure.what());
            }
            // Validate all retained native records before a marked registration.
            result->textures.reserve(textures.textures.size()+textures.animations.size());
            for(const auto& t:textures.textures)result->textures.push_back(t.id);for(const auto& t:textures.animations)result->textures.push_back(t.id);
            registrations.reserve(registrations.size()+1);
            glFinish();ScopedGameAllocator scope(VirtualAllocator);auto& pool=*record.pool;
            record.mark=pool.MarkResource();record.level=pool.m_level;
            try
            {
                for(const auto& t:textures.textures)RegisterTexture(pool,t);
                for(const auto& t:textures.animations)RegisterAnimation(pool,t);
                record.native=RegisterModel(pool,result->skin->Data());
            }
            catch(...){pool.ReleaseResource(record.mark);record.mark=0;record.native=nullptr;record.level=pool.m_level;throw;}
            registrations.push_back(&record);
            record.modelID=result->skin->Data().hash;record.mAnimationsLoaded=record.mAnimationLoadStarted;
            record.mHierarchyLoaded=record.mTexturesLoaded=true;
            Require(NPCResourcesFinished(record.mAnimationLoadStarted,record.mAnimationsLoaded,record.mHierarchyLoaded,record.mTexturesLoaded,record.modelID),
                "NPC registration did not satisfy the shared source completion condition");
            record.published=std::move(result);record.loaded=true;
            for(auto& r:requests)r.bytes.clear();state=BootNpcState::Registered;return true;
        }
        catch(...){error=std::current_exception();state=BootNpcState::Failed;throw;}
    }
    void Release()
    {
        Check();Idle();Guard guard(*this);CancelRequests();
        // Validate every pool's unwind order before retiring any registration;
        // a foreign nested marker must not partially release another pool.
        std::array<std::pair<GLResourcePool*,int>,64> levels{};std::size_t count=0;
        for(auto it=registrations.rbegin();it!=registrations.rend();++it)
        {
            const auto& record=**it;if(!record.mark)continue;Storage(record);
            auto at=std::find_if(levels.begin(),levels.begin()+count,[&](const auto& value){return value.first==record.pool;});
            if(at==levels.begin()+count){*at={record.pool,record.pool->m_level};++count;}
            Require(at->second==record.level,"NPC graphics resource markers changed externally");--at->second;
        }
        // All registered records drain together before any marker or borrowed
        // stream pointer is released. Failure preserves every prior record.
        if(std::any_of(records.begin(),records.end(),[](const auto& r){return r->mark!=0;}))glFinish();
        for(auto it=registrations.rbegin();it!=registrations.rend();++it)
        {
            auto& record=**it;if(!record.mark)continue;Storage(record,true);
            record.pool->ReleaseResource(record.mark);record.mark=0;record.native=nullptr;
        }
        for(auto& r:requests){r.bytes.clear();r.admitted=r.complete=false;}
        registrations.clear();records.clear();ordered[0].clear();ordered[1].clear();pending=nullptr;persistent_pool=nullptr;error={};state=BootNpcState::Released;
    }
    const Record& Find(std::string_view name)const
    {
        Check();Name(name);const std::string copy(name);
        for(const auto& list:ordered)for(auto* r:list)if(nlStrICmp(r->name.c_str(),copy.c_str())==0)
        {Require(r->loaded&&r->published,"NPC template has not completed real resource registration");Storage(*r);return *r;}
        throw std::out_of_range("NPC template name is absent");
    }
    ~Implementation(){try{Release();}catch(...){std::terminate();}}
};
BootNpcResources::BootNpcResources():impl_(std::make_shared<Implementation>()){}
BootNpcResources::~BootNpcResources()=default;
BootNpcBinding::Handle BootNpcResources::Binding()
{
    impl_->Check();if(auto result=binding_.lock())return result;const auto s=impl_;
    auto result=BootNpcBinding::Handle(new BootNpcBinding([s](GLResourcePool& p,std::string_view name,bool persistent){s->Create(p,name,persistent);},
        [s]{return s->Select();},[s]{s->Begin();},[s]{return s->Finish();},[s]{s->Release();}));binding_=result;return result;
}
BootNpcState BootNpcResources::State()const{impl_->Check();return impl_->state;}
std::size_t BootNpcResources::Created()const{impl_->Check();return impl_->records.size();}
std::size_t BootNpcResources::Loaded()const{impl_->Check();return std::count_if(impl_->records.begin(),impl_->records.end(),[](const auto& r){return r->loaded;});}
std::string BootNpcResources::PendingName()const{impl_->Check();return impl_->pending?impl_->pending->name:std::string{};}
BootNpcTemplate::Handle BootNpcResources::Find(std::string_view name)const{return impl_->Find(name).published;}
const glModel* BootNpcResources::Model(std::string_view name)const{return impl_->Find(name).native;}
}
