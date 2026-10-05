#include "runtime/boot_effects.h"
#include "runtime/effects_vertex.h"
#include "runtime/views.h"
#include "Game/GL/GLInventory.h"
#include "NL/gl/gl.h"
#include "NL/gl/glMemory.h"
#include <map>
#include <thread>

namespace mscharged
{
namespace
{
void Require(bool condition,const char* message)
{if(!condition)throw std::logic_error(message);}
bool Linked(const GLResourcePool* pool)
{
    const auto* first=glGetResourcePools();if(!first)return false;
    auto* p=first;do{if(p==pool)return true;p=p->m_next;}while(p!=first);return false;
}
}
struct BootEffectsResources::Implementation
{
    const std::thread::id thread=std::this_thread::get_id();
    BootEffectsState state=BootEffectsState::Idle;
    GLResourcePool* pool=nullptr;
    GLInventory* inventory=nullptr;
    int base_level=0;
    bool busy=false;
    std::unique_ptr<ParticleFileLoad> load;
    EffectsRegistry::Handle registry;
    std::unique_ptr<EffectsVertexResources> geometry;
    BootEffectsCounts counts;
    void Check() const
    {
        Require(thread==std::this_thread::get_id()&&!busy,
            "Boot effects require their nonrecursive NL/graphics owner thread");
    }
    void Pool() const
    {
        Require(pool&&Linked(pool)&&pool->m_inventory==inventory,
            "Captured persistent effects pool/inventory was released or replaced");
        const int level=base_level+(geometry?1:0);
        Require(pool->m_level==level&&inventory->m_nLevel==level,
            "Persistent effects resource marker changed externally");
    }
    struct Guard{Implementation& s;explicit Guard(Implementation& state):s(state){s.busy=true;}~Guard(){s.busy=false;}};
    void Start(GLResourcePool& target)
    {
        Check();Require(state==BootEffectsState::Idle||state==BootEffectsState::Released,
            "Boot effects begin was repeated before release");
        Require(Linked(&target)&&target.m_inventory&&target.m_level==target.m_inventory->m_nLevel,
            "Boot effects need a linked persistent inventory");
        Require(!glIsFrameActive()&&!glNativeViewDispatchActive(),"Begin boot effects outside a collecting frame");
        Guard guard(*this);
        pool=&target;inventory=target.m_inventory;base_level=target.m_level;
        counts={};state=BootEffectsState::Loading;
        try
        {
            glFinish(); // Genuine selected host drain; also verifies a live frame provider.
            load=std::make_unique<ParticleFileLoad>();
        }
        catch(...){state=BootEffectsState::Failed;throw;}
    }
    bool Finish()
    {
        Check();Require(state==BootEffectsState::Loading||state==BootEffectsState::Registered,
            "Boot effects finalize needs an admitted live load");Pool();
        Require(!glIsFrameActive()&&!glNativeViewDispatchActive(),"Finalize boot effects outside a collecting frame");
        if(state==BootEffectsState::Registered)return true;
        Guard guard(*this);
        try
        {
            load->Poll();counts.completed_files=load->CompletedFiles();
            if(load->State()==ParticleFileState::Loading)return false;
            auto files=load->Result(); // Failed/cancelled is never readiness.
            auto next_registry=EffectsRegistry::FromFiles(files);
            if(!next_registry->UnavailableGroups().empty())
                throw resources::UnsupportedResource("Boot effects require unprovided user-effect registration factories");
            const auto models=resources::ReadEffectsGeometry((*files)[ParticleFileKind::Geometry]);
            // Source geometry textures register during their callback; nonresident
            // bundle textures follow in FinishLoading. Existing registry verifies
            // repeated hashes have identical complete metadata and bytes.
            const auto first=resources::ReadTextureBundle((*files)[ParticleFileKind::Textures]);
            const auto second=resources::ReadEffectsTextureBundle((*files)[ParticleFileKind::NonResident]);
            resources::TextureBundle textures;
            std::map<std::uint32_t,bool> seen;
            for(const auto* source:{&first,&second})for(const auto& texture:source->textures)
                if(seen.emplace(texture.id,true).second)textures.textures.push_back(texture);
            auto next_geometry=std::make_unique<EffectsVertexResources>(*pool,models,textures,[]{glFinish();});
            // Publish retained ownership before a fallible frame operation so
            // Cancel can unwind it if the actual frame provider refuses discard.
            geometry=std::move(next_geometry);registry=std::move(next_registry);
            counts={registry->Templates(),registry->RegisteredGroups(),registry->Textures(),
                models.models.size(),models.animations.size(),4};
            glDiscardFrame(1);
            load.reset();state=BootEffectsState::Registered;return true;
        }
        catch(...){state=BootEffectsState::Failed;throw;}
    }
    void Release()
    {
        Check();if(!pool){state=BootEffectsState::Released;return;}
        Pool();Require(!glIsFrameActive()&&!glNativeViewDispatchActive(),"Drain frames before releasing boot effects");
        Guard guard(*this);
        // Callback records stay alive until actual DVD/NL cancellation drains.
        if(load){load->Cancel();load.reset();}
        if(geometry){geometry->Release();geometry.reset();}
        registry.reset();pool=nullptr;inventory=nullptr;state=BootEffectsState::Released;
    }
    ~Implementation(){try{Release();}catch(...){std::terminate();}}
};
BootEffectsResources::BootEffectsResources():impl_(std::make_shared<Implementation>()){}
BootEffectsResources::~BootEffectsResources()=default;
BootEffectsBinding::Handle BootEffectsResources::Binding()
{
    impl_->Check();if(auto result=binding_.lock())return result;
    const auto retained=impl_;
    auto result=BootEffectsBinding::Handle(new BootEffectsBinding(
        [retained](GLResourcePool& pool){retained->Start(pool);},
        [retained]{return retained->Finish();},[retained]{retained->Release();}));
    binding_=result;return result;
}
BootEffectsState BootEffectsResources::State() const{impl_->Check();return impl_->state;}
BootEffectsCounts BootEffectsResources::Counts() const{impl_->Check();return impl_->counts;}
const GLResourcePool* BootEffectsResources::Pool() const{impl_->Check();if(impl_->pool)impl_->Pool();return impl_->pool;}
EffectsRegistry::Handle BootEffectsResources::Registry() const
{impl_->Check();Require(impl_->state==BootEffectsState::Registered,"Boot effects are not registered");impl_->Pool();return impl_->registry;}
}
