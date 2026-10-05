#pragma once
#include "runtime/frontend_movie_playback.h"
#include "runtime/frontend_session.h"
#include "resources/frontend_movie_image.h"
#include <thread>
#include <stdexcept>
namespace mscharged
{
class FrontendPacketRenderer;
// Opaque receipt of actual movie/u/v registration for one source-authored
// Layer/movie instance. CPU consumers can retain and validate this identity
// without linking GX. Only the actual packet renderer can create or retire it.
// A binding is not playback completion or proof that a frame was presented.
class FrontendMovieImageBinding final
{
    friend class FrontendPacketRenderer;
    struct Lifetime
    {
        const std::thread::id thread=std::this_thread::get_id();
        bool active=true;
        std::shared_ptr<void> graphics;
    };
    std::shared_ptr<Lifetime> lifetime_;
    std::shared_ptr<FrontendMoviePlayback> playback_;
    std::shared_ptr<FrontendSession> session_;
    FrontendSession::Handle frame_;
    std::shared_ptr<const resources::FrontendMovieImage> image_;
    std::uint32_t instance_=0,resource_=0;
    FrontendMovieImageBinding()=default;
public:
    using Handle=std::shared_ptr<const FrontendMovieImageBinding>;
    FrontendMovieImageBinding(const FrontendMovieImageBinding&)=delete;
    FrontendMovieImageBinding& operator=(const FrontendMovieImageBinding&)=delete;
    const std::shared_ptr<FrontendMoviePlayback>& Playback()const{return playback_;}
    const std::shared_ptr<FrontendSession>& Session()const{return session_;}
    const FrontendSession::Handle& SourceFrame()const{return frame_;}
    std::uint64_t Generation()const{return playback_->Request().generation;}
    const std::string& Path()const{return playback_->Request().path;}
    std::uint32_t Instance()const{return instance_;}
    std::uint32_t Resource()const{return resource_;}
    const std::shared_ptr<const resources::FrontendMovieImage>& Image()const{return image_;}
    bool Active()const
    {
        if(lifetime_->thread!=std::this_thread::get_id())throw std::logic_error("Movie binding requires its owner thread");
        return lifetime_->active;
    }
};
// Applies the original runtime handle/dimension change to a caller-owned graph
// candidate. The original resource name hash and instance pointer stay intact.
void ApplyFrontendMovieBinding(resources::FrontendScene&,const FrontendMovieImageBinding::Handle&);
void ApplyFrontendMovieBinding(resources::FrontendAnimationPlayback&,const FrontendMovieImageBinding::Handle&);
}
