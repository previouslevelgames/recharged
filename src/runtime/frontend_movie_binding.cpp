#include "runtime/frontend_movie_binding.h"
#include "resources/frontend_animation.h"
#include <algorithm>
namespace mscharged
{
void ApplyFrontendMovieBinding(resources::FrontendScene& graph,const FrontendMovieImageBinding::Handle& binding)
{
    resources::Require(binding&&binding->Active(),"Movie swap requires an active actual renderer binding");
    binding->Playback()->Check();
    const auto state=binding->Playback()->Status().state;
    resources::Require(state!=FrontendMovieState::Cancelled&&state!=FrontendMovieState::Failed,"Movie swap provider is not live");
    const auto& source=binding->SourceFrame()->graph;
    const auto instance=[&](const resources::FrontendScene& scene)->const resources::FrontendInstance&{
        const auto found=std::find_if(scene.instances.begin(),scene.instances.end(),[&](const auto& v){return v.offset==binding->Instance();});
        resources::Require(found!=scene.instances.end()&&found->type==2&&found->resource==binding->Resource(),"Movie instance/resource identity changed");return *found;
    };
    const auto& original=instance(source);const auto& current=instance(graph);
    resources::Require(original.hash==current.hash&&original.library==current.library,"Movie authored instance identity changed");
    const auto find_resource=[&](const resources::FrontendScene& scene){return std::find_if(scene.resources.begin(),scene.resources.end(),[&](const auto& v){return v.offset==binding->Resource();});};
    const auto old=find_resource(source),next=find_resource(graph);
    resources::Require(old!=source.resources.end()&&next!=graph.resources.end()&&old->type==0&&next->type==0
        &&old->hash==next->hash&&old->file_block==next->file_block,"Movie source resource identity changed");
    resources::Require(next->native_movie==old->native_movie||next->native_movie==binding->Image(),"Movie resource binding changed after registration admission");
    // FETextureResource::SetTextureHandle updates runtime handle and dimensions,
    // not FEResourceHandle's name/hash or an instance's resource pointer.
    graph.resources[std::size_t(next-graph.resources.begin())].native_movie=binding->Image();
}
void ApplyFrontendMovieBinding(resources::FrontendAnimationPlayback& playback,const FrontendMovieImageBinding::Handle& binding)
{
    // Validate the entire source identity on a temporary graph before mutating
    // the retained animation clone's sole runtime texture field.
    auto candidate=playback.Scene();ApplyFrontendMovieBinding(candidate,binding);
    playback.BindMovieResource(binding->Resource(),binding->Image());
}

}
