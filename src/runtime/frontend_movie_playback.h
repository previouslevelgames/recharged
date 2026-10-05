#pragma once
#include "runtime/thp_movie.h"
#include <string>

namespace mscharged
{
class FrontendMovieRenderer;
class FrontendMoviePlayback;
// Issued only by the real drawable after successful Aurora presentation and
// frame drain. Neither a decoded EOF nor a caller-supplied bool can create it.
class FrontendMoviePresentation
{
    resources::ThpMovieFrameHandle frame_;
    std::uint64_t sequence_=0;
    FrontendMoviePresentation(resources::ThpMovieFrameHandle,std::uint64_t);
    friend class FrontendMovieRenderer;
    friend class FrontendMoviePlayback;
public:
    using Handle=std::shared_ptr<const FrontendMoviePresentation>;
};
class FrontendMovieCompletion
{
    std::uint64_t generation_=0;
    std::string path_;
    std::uint32_t final_frame_=0;
    std::uint64_t audio_frames_=0,presentation_=0;
    FrontendMovieCompletion()=default;
    friend class FrontendMoviePlayback;
public:
    using Handle=std::shared_ptr<const FrontendMovieCompletion>;
    std::uint64_t Generation()const{return generation_;}
    const std::string& Path()const{return path_;}
    std::uint32_t FinalFrame()const{return final_frame_;}
    std::uint64_t AudioFrames()const{return audio_frames_;}
    std::uint64_t Presentation()const{return presentation_;}
};
struct FrontendMovieRequest
{
    std::uint64_t generation=0;
    std::string path;
    bool details_with_sound=true,start_with_sound=false,loop=false,synced=true;
};
struct FrontendMovieOptions
{
    // Explicit sound mode and source config selection. The source start sound
    // argument is unused; false never silences the actual THP audio track.
    bool mono=false;
    unsigned volume_percent=100,fade_in_ms=500;
    std::uint32_t device_id=0;
};
enum class FrontendMovieState { Loading,Playing,Draining,Complete,Failed,Cancelled };
struct FrontendMovieStatus
{
    FrontendMovieState state=FrontendMovieState::Loading;
    std::uint64_t next_retrace=0,submitted_audio_frames=0;
    unsigned published_frames=0;
    int queued_input_bytes=0,available_output_bytes=0;
    bool decoded_eof=false,final_presented=false;
};
// Real NL reader + SDL movie-only output. The explicit retrace clock is the
// original glx VI post-retrace domain (not THP header FPS or a draw counter).
// Caller advances one authority; skipped retraces decode once, without catchup.
// Fixed initial source volume ramp is applied once per queued sample; live
// volume changes and original AI/game-audio mixing remain unavailable. Mono
// uses the original non-null game mix branch with an explicit zero contribution.
// Complete proves exact final presentation and flushed SDL stream consumption,
// not physical device audibility. Destroy before NL/arenas/SDL shutdown.
class FrontendMoviePlayback
{
    struct Implementation;
    std::unique_ptr<Implementation> impl_;
public:
    FrontendMoviePlayback(FrontendMovieRequest,FrontendMovieOptions,std::uint64_t initial_retrace);
    ~FrontendMoviePlayback();
    FrontendMoviePlayback(const FrontendMoviePlayback&)=delete;
    FrontendMoviePlayback& operator=(const FrontendMoviePlayback&)=delete;
    void Advance(std::uint64_t retrace); // Services real NL; monotonic samples.
    void Acknowledge(const FrontendMoviePresentation::Handle&);
    resources::ThpMovieFrameHandle Current()const;
    const resources::ThpMovieInfo& Info()const;
    FrontendMovieStatus Status()const;
    FrontendMovieCompletion::Handle Completion()const;
    const FrontendMovieRequest& Request()const;
    const FrontendMovieOptions& Options()const;
    void Check()const;
    void Cancel();
};
}
