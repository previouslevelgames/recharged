#include "resources/frontend_animation.h"
#include "Game/FE/FrontendAnimationSteps.h"
#include "Game/FE/FrontendSelectionSteps.h"
#include "Game/FE/FrontendInstanceSteps.h"
#include <algorithm>
#include <cmath>
#include <map>
#include <set>

namespace mscharged::resources
{
namespace
{
void Finite(float value)
{Require(std::isfinite(value)&&std::abs(value)<=1e7f,"Frontend animation scalar exceeds its finite profile");}
std::uint8_t Colour(float value)
{Require(std::isfinite(value)&&value>=0&&value<=255,"Frontend animated colour exceeds its byte range");return static_cast<std::uint8_t>(value);}
struct Index
{
    std::map<std::uint32_t,std::size_t> instances,slides,library,animations;
    explicit Index(const FrontendScene& scene)
    {
        Require(scene.instances.size()<=16384&&scene.slides.size()<=16384&&scene.library.size()<=16384
            &&scene.resources.size()<=16384&&scene.animations.size()<=16384,"Frontend animation graph exceeds its record budget");
        std::set<std::uint32_t> ids;
        const auto add=[&](const auto& input,auto& output){for(std::size_t i=0;i<input.size();++i)
        {Require(ids.insert(input[i].offset).second,"Duplicate frontend animation record ID");output.emplace(input[i].offset,i);}};
        add(scene.instances,instances);add(scene.slides,slides);add(scene.library,library);add(scene.animations,animations);
        for(const auto& resource:scene.resources)Require(ids.insert(resource.offset).second,"Duplicate frontend resource ID");
        std::size_t keys=0;
        for(const auto& animation:scene.animations)
        {
            Require(instances.contains(animation.target),"Frontend animation target is absent");
            Require(animation.cast<=1&&!animation.keys.empty()&&animation.keys.size()<=4096,"Unsupported or empty frontend key stream");
            Require(animation.keys.size()<=65536-keys,"Frontend animation key budget exceeded");keys+=animation.keys.size();
            for(std::size_t k=0;k<animation.keys.size();++k)
            {
                const auto& key=animation.keys[k];Require(ids.insert(key.offset).second,"Aliased frontend animation key");
                for(unsigned c=0;c<(animation.cast?3u:1u);++c)for(float value:key.channels[c])Finite(value);
                if(k)Require(key.channels[0][3]>animation.keys[k-1].channels[0][3],"Frontend key times are not strictly ordered");
                if(animation.cast)Require(key.channels[0][3]==key.channels[1][3]&&key.channels[0][3]==key.channels[2][3],"Frontend vector key times differ");
            }
        }
        std::set<std::uint32_t> owners;
        for(const auto& slide:scene.slides)
        {
            Finite(slide.start);Finite(slide.duration);Finite(slide.time);
            Require(slide.duration>=0&&slide.play_mode<=2&&slide.animated==!slide.animations.empty(),"Invalid frontend timeline metadata");
            for(auto id:slide.animations)Require(animations.contains(id)&&owners.insert(id).second,"Missing or multiply-owned frontend animation");
        }
        Require(owners.size()==scene.animations.size(),"Unowned frontend animation record");
    }
};
struct Step
{
    FrontendScene& scene;const Index& index;
    std::set<std::uint32_t> active;
    unsigned visits=0;std::size_t channels=0,sampled_keys=0;
    void Enter(std::uint32_t id,unsigned depth)
    {Require(depth<=64&&++visits<=65536&&active.insert(id).second,"Cyclic or excessive frontend timeline traversal");}
    void Sample(const FrontendAnimation& animation,float time)
    {
        ++channels;auto& target=scene.instances.at(index.instances.at(animation.target));
        const auto count=animation.keys.size();
        Require(count<=1048576-sampled_keys,"Frontend sampled-key work budget exceeded");sampled_keys+=count;
        if(animation.cast==0)
        {
            Require(animation.type>=6&&animation.type<=10,"Unsupported frontend scalar animation channel");
            std::vector<fAnimationKeyframe> ring(count);
            for(std::size_t i=0;i<count;++i)
            {
                const auto& k=animation.keys[i].channels[0];ring[i].pKeyFrameData={k[0],k[1],k[2],k[3]};
                ring[i].m_next=&ring[(i+1)%count];ring[i].m_prev=&ring[(i+count-1)%count];
            }
            float value=0;if(!FrontendSampleFloat(&ring.back(),time,value))return;Finite(value);
            if(animation.type==6)
            {
                if(value==-1)return; // Original scalar opacity sentinel suppresses its write.
                target.attributes.colour[3]=Colour(value);target.overload_flags|=16;
            }
            else{target.attributes.uv[animation.type-7]=value;target.overload_flags|=0x40u<<(animation.type-7);}
        }
        else
        {
            Require(animation.type>=1&&animation.type<=5,"Unsupported frontend vector animation channel");
            Require(count!=1||time<=animation.keys[0].channels[0][3]||animation.keys[0].channels[0][1]==-1,
                "Singleton frontend vector key cannot extrapolate a zero time interval");
            std::vector<v3AnimationKeyframe> ring(count);
            for(std::size_t i=0;i<count;++i)
            {
                auto& key=ring[i];FEAnimationKeyframe* out[]={&key.pKeyFrameDataX,&key.pKeyFrameDataY,&key.pKeyFrameDataZ};
                for(unsigned c=0;c<3;++c){const auto& k=animation.keys[i].channels[c];*out[c]={k[0],k[1],k[2],k[3]};}
                key.m_next=&ring[(i+1)%count];key.m_prev=&ring[(i+count-1)%count];
            }
            float value[3];FrontendSampleVector3(&ring.back(),AnimType(animation.type),time,value);for(float v:value)Finite(v);
            if(animation.type==5)
            {target.attributes.colour={Colour(value[0]),Colour(value[1]),Colour(value[2]),255};target.overload_flags|=16;}
            else
            {
                auto* out=animation.type==1?&target.attributes.position:animation.type==2?&target.attributes.rotation:
                    animation.type==3?&target.attributes.scale:&target.attributes.pivot;
                std::copy_n(value,3,out->begin());target.overload_flags|=1u<<(animation.type-1);
            }
        }
    }
    void Instance(std::uint32_t id,float delta,unsigned depth)
    {
        Enter(id,depth);Require(index.instances.contains(id),"Frontend timeline instance is absent");
        const auto& instance=scene.instances[index.instances.at(id)];
        if(instance.type==4)
        {
            Require(instance.library&&index.library.contains(*instance.library),"Frontend timeline component is absent");
            const auto& object=scene.library[index.library.at(*instance.library)];Require(object.type==3,"Frontend timeline component type differs");
            Require(!object.active_slide||std::find(object.slides.begin(),object.slides.end(),*object.active_slide)!=object.slides.end(),"Frontend component active slide is outside its ring");
            if(object.active_slide)Slide(*object.active_slide,delta,depth+1);
        }
        // Original TLSlide::UpdateAsset does not gate updates on visibility or
        // instance validity; those checks occur later in FERender.
        for(auto child:instance.children)Instance(child,delta,depth+1);
        active.erase(id);
    }
    void Slide(std::uint32_t id,float delta,unsigned depth)
    {
        Enter(id,depth);Require(index.slides.contains(id),"Frontend timeline slide is absent");auto& slide=scene.slides[index.slides.at(id)];
        delta=FrontendAdvanceSlideTime(slide.time,delta,slide.start,slide.duration,slide.play_mode,slide.frozen);Finite(slide.time);
        for(auto animation:slide.animations)Sample(scene.animations[index.animations.at(animation)],slide.time);
        for(auto child:slide.children)Instance(child,delta,depth+1);
        slide.animation_evaluated=true;active.erase(id);
    }
    void Run(float delta)
    {
        if(!scene.active_slide)return;
        Require(index.slides.contains(*scene.active_slide),"Frontend presentation slide is absent");
        auto& slide=scene.slides[index.slides.at(*scene.active_slide)];
        FrontendAdvanceSlideTime(scene.presentation_time,delta,slide.start,slide.duration,slide.play_mode,false);Finite(scene.presentation_time);
        slide.time=scene.presentation_time;
        Slide(slide.offset,delta,0); // Original presentation intentionally also calls slide.Update(delta).
    }
};
}
struct FrontendAnimationPlayback::Impl
{
    FrontendScene baseline,current;
    Index index;
    std::size_t channels=0;
    bool loading_widescreen=false;
    Impl(const FrontendScene& scene,FrontendReference selected):baseline(scene),index(baseline)
    {
        if(!selected)selected=baseline.active_slide;
        Require(!selected||std::find(baseline.presentation_slides.begin(),baseline.presentation_slides.end(),*selected)!=baseline.presentation_slides.end(),
            "Selected animated presentation slide is outside its ring");
        baseline.active_slide=selected;
        for(auto& slide:baseline.slides)slide.animation_evaluated=false;
        Reset();
    }
    void Reset()
    {
        auto next=baseline;next.presentation_time=0;Step step{next,index};step.Run(0);
        current=std::move(next);channels=step.channels;loading_widescreen=false;
    }
    void Advance(float delta)
    {
        Require(std::isfinite(delta)&&delta>=0&&delta<=60,"Frontend timeline delta exceeds its bounded profile");
        auto next=current;Step step{next,index};step.Run(delta);current=std::move(next);channels=step.channels;
    }
    FrontendReference Find(const std::vector<std::uint32_t>& ring,std::string_view name) const
    {
        const auto hash=FrontendLowerHash(name);
        for(auto id:ring)
        {
            Require(index.slides.contains(id),"Frontend selection references a missing slide");
            if(current.slides[index.slides.at(id)].hash==hash)return id;
        }
        return {};
    }
    bool SelectPresentation(std::string_view name,bool reset)
    {
        const auto selected=Find(current.presentation_slides,name);
        auto next=current;
        FrontendSelectPresentationTime(next.presentation_time,reset,next.active_slide!=selected);
        next.active_slide=selected;
        current=std::move(next);channels=0;return selected.has_value();
    }
    bool SelectComponent(std::uint32_t id,std::string_view name,bool reset,bool preserve)
    {
        Require(index.library.contains(id),"Frontend component selection ID is absent");
        const auto slot=index.library.at(id);
        Require(current.library[slot].type==3,"Frontend selection ID is not a component");
        const auto selected=Find(current.library[slot].slides,name);
        auto next=current;auto& component=next.library[slot];
        if(selected)FrontendSelectComponentTime(next.slides[index.slides.at(*selected)].time,
            reset,component.active_slide!=selected,preserve);
        component.active_slide=selected;
        Step step{next,index};if(selected)step.Slide(*selected,0,0);
        current=std::move(next);channels=step.channels;return selected.has_value();
    }
    FrontendLoadingSetup SetupLoadingScene(bool widescreen)
    {
        auto next=std::make_unique<Impl>(*this);
        struct Transition
        {
            Impl& owner;
            std::uint32_t library;
            bool& m_bVisible;
            void SetActiveSlide(const char* name,bool reset,bool preserve)
            {
                Require(owner.SelectComponent(library,name,reset,preserve),"Loading setup requires the authored widescreen slide");
            }
        };
        struct Presentation { FrontendReference m_currentSlide; } presentation{next->current.active_slide};
        struct Scene { Presentation* mPresentation;Transition* mTransitionComponent=nullptr;bool mWidescreen; } setup{&presentation,nullptr,next->loading_widescreen};
        std::optional<Transition> transition;FrontendLoadingSetup result;
        const auto find=[&](FrontendReference slide,const char* layer,const char* name)->Transition*
        {
            Require(slide.has_value(),"Loading setup requires an active presentation slide");
            const std::array<std::string_view,2> path{layer,name};
            const auto node=FindFrontendNode(next->current,{FrontendNodeKind::Slide,*slide},FrontendNamedPath(path),FrontendNodeType::Component);
            Require(node.has_value(),"Loading setup requires an authored no home component; no default substitute is supplied");
            auto& instance=next->current.instances.at(next->index.instances.at(node->id));
            Require(instance.library&&next->index.library.contains(*instance.library)
                &&next->current.library[next->index.library.at(*instance.library)].type==3,"Loading setup component library is unavailable");
            result.transition_component=node->id;
            transition.emplace(Transition{*next,*instance.library,instance.visible});return &*transition;
        };
        FrontendLoadingSceneSetupPrefix(setup,[&]{return widescreen;},find);
        next->loading_widescreen=setup.mWidescreen;result.widescreen=setup.mWidescreen;
        // The proxy's visibility reference was used before SetActiveSlide's
        // transactional graph replacement. No proxy data is read afterward.
        *this=std::move(*next);return result;
    }
};
FrontendAnimationPlayback::FrontendAnimationPlayback(const FrontendScene& scene,FrontendReference selected)
    :impl_(std::make_unique<Impl>(scene,selected)){}
FrontendAnimationPlayback::FrontendAnimationPlayback(std::unique_ptr<Impl> impl):impl_(std::move(impl)){}
FrontendAnimationPlayback::~FrontendAnimationPlayback()=default;
void FrontendAnimationPlayback::Advance(float delta){impl_->Advance(delta);}
void FrontendAnimationPlayback::Reset(){impl_->Reset();}
std::unique_ptr<FrontendAnimationPlayback> FrontendAnimationPlayback::Clone() const
{return std::unique_ptr<FrontendAnimationPlayback>(new FrontendAnimationPlayback(std::make_unique<Impl>(*impl_)));}
bool FrontendAnimationPlayback::SelectPresentation(std::string_view name,bool reset)
{return impl_->SelectPresentation(name,reset);}
bool FrontendAnimationPlayback::SelectComponent(std::uint32_t id,std::string_view name,bool reset,bool preserve)
{return impl_->SelectComponent(id,name,reset,preserve);}
void FrontendAnimationPlayback::Apply(std::span<const FrontendInstanceChange> changes)
{ApplyFrontendInstanceChanges(impl_->current,changes);impl_->channels=0;}
FrontendLoadingSetup FrontendAnimationPlayback::SetupLoadingScene(bool widescreen)
{return impl_->SetupLoadingScene(widescreen);}
const FrontendScene& FrontendAnimationPlayback::Scene() const{return impl_->current;}
float FrontendAnimationPlayback::PresentationTime() const{return impl_->current.presentation_time;}
std::size_t FrontendAnimationPlayback::ChannelsEvaluated() const{return impl_->channels;}
}
