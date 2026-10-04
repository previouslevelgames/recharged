#include "runtime/animation_pose.h"
#include "runtime/pose_accumulator.h"
#include "runtime/graphics_memory.h"
#include "Game/SAnimPoseSteps.h"
#include <cmath>
#include <stdexcept>
#include <thread>

namespace mscharged
{
namespace
{
void Require(bool okay, const char* message) { if (!okay) throw std::invalid_argument(message); }
// The original accumulator ignores negligible weights before consuming decoded
// quaternion/translation/scale values. Preserve that order in the checked sink.
struct AccumulatorSink
{
    PoseAccumulator& pose;
    const cSHierarchy* m_BaseSHierarchy;
    void BlendRot(int n, const nlQuaternion* q, float w, bool mirror)
    { if (std::abs(w) >= .001f) pose.BlendRotation(n,{q->x,q->y,q->z,q->w},w,mirror); }
    void BlendRotAroundZ(int n, unsigned short angle, float w)
    { pose.BlendAngle(n,angle,w); }
    void BlendScale(int n, const nlVector3* v, float w, bool mirror)
    { if (std::abs(w) >= .001f) pose.BlendScale(n,{v->x,v->y,v->z},w,mirror); }
    void BlendTrans(int n, const nlVector3* v, float w, bool mirror)
    { if (std::abs(w) >= .001f) pose.BlendTranslation(n,{v->x,v->y,v->z},w,mirror); }
    void MultiplyScale(int n, const nlVector3* v, float w)
    { if (std::abs(w) >= .001f) pose.MultiplyScale(n,{v->x,v->y,v->z},w); }
    void BlendRotIdentity(int n, float w) { pose.BlendRotationIdentity(n,w); }
    void BlendScaleIdentity(int n, float w) { pose.BlendScaleIdentity(n,w); }
    void BlendTransIdentity(int n, float w) { pose.BlendTranslationIdentity(n,w); }
};
nlMatrix4 Identity()
{ nlMatrix4 m{}; m.m11=m.m22=m.m33=m.m44=1; return m; }
}
struct AnimationPose::Implementation
{
    AnimationBundle::Handle bundle;
    std::array<std::unique_ptr<PoseAccumulator>,2> scratch;
    AnimationPoseFrame::Handle current;
    std::thread::id thread=std::this_thread::get_id();
    unsigned next=0;
    explicit Implementation(AnimationBundle::Handle value):bundle(std::move(value))
    {
        Require(bool(bundle),"Animation pose requires a retained animation bundle");
        for (auto& owner:scratch) owner=std::make_unique<PoseAccumulator>(bundle->Hierarchy(),false);
    }
    void Thread() const
    { if (thread!=std::this_thread::get_id()) throw std::logic_error("Animation pose requires its creating thread"); }
    void Check() const
    {
        Thread();
        if (!bundle || !gMemoryInitialized) throw std::logic_error("Animation pose or its game arenas have been released");
    }
    void Release()
    {
        Thread(); if (!bundle) return; Check();
        for (auto& owner:scratch) owner.reset();
        current.reset(); bundle.reset();
    }
    ~Implementation() { try { Release(); } catch (...) { std::terminate(); } }
};
AnimationPose::AnimationPose(AnimationBundle::Handle bundle):impl_(std::make_unique<Implementation>(std::move(bundle))) {}
AnimationPose::~AnimationPose()=default;
void AnimationPose::Release() { impl_->Release(); }
AnimationPoseFrame::Handle AnimationPose::Current() const { impl_->Check(); return impl_->current; }
void AnimationPose::Reset()
{
    impl_->Check();
    for (auto& owner:impl_->scratch) owner->Reset();
    impl_->current.reset(); impl_->next=0;
}
AnimationPoseFrame::Handle AnimationPose::Sample(std::span<const AnimationPoseLayer> layers,const nlMatrix4& world,float scale)
{
    impl_->Check();
    Require(layers.size()<=256,"Animation pose exceeds 256 ordered layers");
    // Validate every external descriptor before starting original accumulation.
    for (const auto& layer:layers)
    {
        Require(layer.animation<impl_->bundle->Size(),"Animation layer index is outside its retained bundle");
        const auto& animation=impl_->bundle->At(layer.animation)->Data();
        SAnimPoseCheck(layer.time,layer.weight,animation.m_nNumKeys);
        Require(layer.scale_only || animation.m_nNumMorphChannels==0,
            "Morph-bearing full animation poses are not yet qualified");
    }
    auto& accumulator=*impl_->scratch[impl_->next];
    accumulator.Reset();
    const auto hierarchy=impl_->bundle->Hierarchy();
    AccumulatorSink sink{accumulator,&hierarchy->Data()};
    // Original cPN_SAnimController evaluates each layer's nodes in source order.
    for (const auto& layer:layers)
    {
        const auto& animation=impl_->bundle->At(layer.animation)->Data();
        for (unsigned node=0;node<accumulator.Nodes();++node)
        {
            const auto mapped=impl_->bundle->MappedNode(layer.animation,node,layer.mirror);
            if (mapped)
            {
                if (layer.scale_only)
                    SAnimPoseBlendScaleMultiply(animation,node,*mapped,layer.time,layer.weight,&sink);
                else
                {
                    SAnimPoseBlendRot(animation,node,*mapped,layer.time,layer.weight,&sink,layer.mirror);
                    SAnimPoseBlendScale(animation,node,*mapped,layer.time,layer.weight,&sink,layer.mirror);
                    SAnimPoseBlendTrans(animation,node,*mapped,layer.time,layer.weight,&sink,layer.mirror);
                }
            }
            else if (!layer.scale_only)
            {
                const auto& offset=SAnimPoseUnmapped(node,layer.weight,&sink);
                accumulator.SetAnimationTranslation(node,offset);
            }
        }
    }
    accumulator.Build(world,scale);
    auto next=std::make_shared<AnimationPoseFrame>();
    next->hierarchy=hierarchy;
    const unsigned count=accumulator.Nodes();
    next->matrices.reserve(count); next->previous.reserve(count); next->quaternions.reserve(count);
    for (unsigned node=0;node<count;++node)
    {
        next->matrices.push_back(accumulator.Matrix(node));
        next->previous.push_back(impl_->current?impl_->current->matrices[node]:Identity());
        next->quaternions.push_back(accumulator.Quaternion(node));
    }
    impl_->current=next; impl_->next^=1;
    return next;
}
}
