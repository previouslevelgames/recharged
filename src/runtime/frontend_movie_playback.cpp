#include "runtime/frontend_movie_playback.h"
#include "Game/Sys/MovieClockSteps.h"
#include "RVL_SDK/thp/THPMovieSteps.h"
#include <SDL3/SDL.h>
#include "NL/nlFileGC.h"
#include <array>
#include <exception>
#include <limits>
#include <stdexcept>
#include <thread>

namespace mscharged
{
namespace
{
constexpr unsigned short volume_table[]={
#include "RVL_SDK/thp/THPVolumeTable.inc"
};
static_assert(std::size(volume_table)==128);
void Require(bool value,const char* text){if(!value)throw std::invalid_argument(text);}
std::runtime_error Error(const char* text){return std::runtime_error(std::string(text)+": "+SDL_GetError());}
struct Volume
{
    long rampCount=0;
    float curVolume=0,targetVolume=0,deltaVolume=0;
};
}
FrontendMoviePresentation::FrontendMoviePresentation(resources::ThpMovieFrameHandle frame,std::uint64_t sequence)
    :frame_(std::move(frame)),sequence_(sequence){}
struct FrontendMoviePlayback::Implementation
{
    std::thread::id thread=std::this_thread::get_id();
    FrontendMovieRequest request;
    FrontendMovieOptions options;
    std::unique_ptr<ThpMovie> movie;
    resources::ThpMovieInfo info;
    resources::ThpMovieFrameHandle current;
    FrontendMovieCompletion::Handle completion;
    FrontendMovieStatus status;
    std::exception_ptr failure;
    SDL_AudioStream* stream=nullptr;
    bool initialized=false,started=false,flushed=false,end_due=false,busy=false;
    std::uint64_t clock=0,last_presentation=0;
    Volume volume;
    Implementation(FrontendMovieRequest r,FrontendMovieOptions o,std::uint64_t initial)
        :request(std::move(r)),options(o),clock(initial)
    {
        Require(request.generation&&!request.path.empty(),"Movie playback requires exact request identity");
        Require(request.synced&&!request.loop,"Only original synced nonlooping movies are qualified");
        Require(options.volume_percent<=100&&options.fade_in_ms<=60000,"Movie volume/fade exceeds its source profile");
        Require(initial<=std::numeric_limits<std::uint64_t>::max()-2,"Movie retrace counter is exhausted");
        status.next_retrace=initial;
        try
        {
            movie=std::make_unique<ThpMovie>(request.path);info=movie->Info();
            if(info.channels)
            {
                Require(info.sample_rate==32000||info.sample_rate==48000,"Movie output requires an original32/48kHz mixer rate");
                if(!SDL_InitSubSystem(SDL_INIT_AUDIO))throw Error("Movie audio initialization failed");
                initialized=true;
                const SDL_AudioSpec spec{SDL_AUDIO_S16,2,static_cast<int>(info.sample_rate)};
                stream=SDL_OpenAudioDeviceStream(options.device_id?options.device_id:SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK,&spec,nullptr,nullptr);
                if(!stream)throw Error("Movie audio stream creation failed");
                // SHMoviePlayer first sets zero, then target127*config/100 with
                // source fade duration. No artificial interpretation of bSound.
                const float gain=float(options.volume_percent)/100.0f;
                THPMovieSetVolume(volume,0,0,info.sample_rate/1000);
                THPMovieSetVolume(volume,static_cast<long>(127.0f*gain),options.fade_in_ms,info.sample_rate/1000);
            }
            movie->BeginNext();
        }
        catch(...){Close();throw;}
    }
    void Thread()const{if(thread!=std::this_thread::get_id())throw std::logic_error("Movie playback requires its owner thread");}
    void Close()noexcept
    {
        if(stream){SDL_PauseAudioStreamDevice(stream);SDL_ClearAudioStream(stream);SDL_DestroyAudioStream(stream);stream=nullptr;}
        if(movie){try{movie->Cancel();}catch(...){std::terminate();}movie.reset();}
        if(initialized){SDL_QuitSubSystem(SDL_INIT_AUDIO);initialized=false;}
    }
    ~Implementation(){if(thread!=std::this_thread::get_id())std::terminate();Close();}
    void Query()
    {
        if(!stream){status.queued_input_bytes=status.available_output_bytes=0;return;}
        status.queued_input_bytes=SDL_GetAudioStreamQueued(stream);
        status.available_output_bytes=SDL_GetAudioStreamAvailable(stream);
        if(status.queued_input_bytes<0||status.available_output_bytes<0)throw Error("Movie audio stream query failed");
    }
    void Complete()
    {
        if(!end_due||!status.final_presented||status.queued_input_bytes||status.available_output_bytes)return;
        auto receipt=std::shared_ptr<FrontendMovieCompletion>(new FrontendMovieCompletion);
        receipt->generation_=request.generation;receipt->path_=request.path;
        receipt->final_frame_=current->index;receipt->audio_frames_=status.submitted_audio_frames;
        receipt->presentation_=last_presentation;completion=std::move(receipt);status.state=FrontendMovieState::Complete;
    }
    void Advance(std::uint64_t retrace)
    {
        Thread();
        if(busy||nlGetCurrentAsyncRead())throw std::logic_error("Recursive movie playback mutation during NL service is unsupported");
        if(failure)std::rethrow_exception(failure);
        if(status.state==FrontendMovieState::Cancelled)throw std::logic_error("Movie playback was cancelled");
        Require(retrace>=clock&&retrace<=std::numeric_limits<std::uint64_t>::max()-2,"Movie retrace counter moved backwards or exhausted");
        clock=retrace;
        if(status.state==FrontendMovieState::Complete)return;
        busy=true;struct Leave{bool& value;~Leave(){value=false;}}leave{busy};
        try
        {
            movie->Service();movie->RethrowFailure();Query();
            if(status.decoded_eof)
            {
                if(MovieDecodeDue(clock,status.next_retrace))end_due=true;
                status.state=FrontendMovieState::Draining;Complete();return;
            }
            const auto ready=movie->Current();
            if(!ready||ready==current||!MovieDecodeDue(clock,status.next_retrace))return;
            // Bounded native backpressure: never discard an authored audio block
            // or treat original Decode's buffer-full result as a decoded frame.
            const std::uint64_t budget=std::uint64_t(info.max_audio_samples)*6*4;
            if(stream&&std::uint64_t(status.queued_input_bytes)+ready->pcm_right_left.size()*2>budget)return;
            Volume next=volume;
            std::vector<std::int16_t> samples;
            if(stream)
            {
                samples.resize(ready->pcm_right_left.size());
                const short silent[2]={0,0};
                for(std::size_t i=0;i<samples.size();i+=2)
                {
                    // Source floating accumulation may overshoot a target by
                    // rounding, but its table index must remain in-bounds.
                    Volume stepped=next;
                    if(stepped.rampCount){--stepped.rampCount;stepped.curVolume+=stepped.deltaVolume;}
                    else stepped.curVolume=stepped.targetVolume;
                    Require(stepped.curVolume>=0&&stepped.curVolume<128,"Movie source volume ramp exceeds its table");
                    const auto vol=THPMovieNextVolume(next,volume_table);
                    short pair[2];THPMovieMixPair(pair,silent,ready->pcm_right_left.data()+i,vol,options.mono);
                    samples[i]=pair[1];samples[i+1]=pair[0]; // SDK R,L -> SDL L,R.
                }
                if(!SDL_PutAudioStreamData(stream,samples.data(),static_cast<int>(samples.size()*sizeof(samples[0]))))
                    throw Error("Movie audio submission failed");
                if(!started)
                {
                    if(!SDL_ResumeAudioStreamDevice(stream))throw Error("Movie audio start failed");
                    started=true;
                }
            }
            volume=next;current=ready;++status.published_frames;status.submitted_audio_frames+=ready->audio_samples;
            status.next_retrace=MovieNextDecode(clock);status.state=FrontendMovieState::Playing;
            status.decoded_eof=movie->State()==ThpMovieState::EndOfStream;
            if(status.decoded_eof)
            {
                if(stream&&!SDL_FlushAudioStream(stream))throw Error("Movie audio flush failed");
                flushed=true;
            }
            else movie->BeginNext();
        }
        catch(...){failure=std::current_exception();status.state=FrontendMovieState::Failed;completion.reset();Close();throw;}
    }
};
FrontendMoviePlayback::FrontendMoviePlayback(FrontendMovieRequest r,FrontendMovieOptions o,std::uint64_t initial)
    :impl_(std::make_unique<Implementation>(std::move(r),o,initial)){}
FrontendMoviePlayback::~FrontendMoviePlayback()=default;
void FrontendMoviePlayback::Advance(std::uint64_t retrace){impl_->Advance(retrace);}
void FrontendMoviePlayback::Check()const{impl_->Thread();if(impl_->failure)std::rethrow_exception(impl_->failure);}
resources::ThpMovieFrameHandle FrontendMoviePlayback::Current()const{impl_->Thread();return impl_->current;}
const resources::ThpMovieInfo& FrontendMoviePlayback::Info()const{impl_->Thread();return impl_->info;}
const FrontendMovieRequest& FrontendMoviePlayback::Request()const{impl_->Thread();return impl_->request;}
const FrontendMovieOptions& FrontendMoviePlayback::Options()const{impl_->Thread();return impl_->options;}
FrontendMovieStatus FrontendMoviePlayback::Status()const{impl_->Thread();return impl_->status;}
FrontendMovieCompletion::Handle FrontendMoviePlayback::Completion()const{impl_->Thread();return impl_->completion;}
void FrontendMoviePlayback::Acknowledge(const FrontendMoviePresentation::Handle& receipt)
{
    Check();Require(!impl_->busy&&!nlGetCurrentAsyncRead(),"Movie acknowledgment during NL service is unsupported");
    Require(impl_->status.state!=FrontendMovieState::Cancelled&&receipt&&receipt->frame_==impl_->current
        &&receipt->sequence_>impl_->last_presentation,"Movie presentation does not belong to current retained output");
    impl_->last_presentation=receipt->sequence_;
    if(impl_->status.decoded_eof)impl_->status.final_presented=true;
}
void FrontendMoviePlayback::Cancel()
{
    impl_->Thread();Require(!impl_->busy&&!nlGetCurrentAsyncRead(),"Movie cancellation during NL service is unsupported");impl_->Close();impl_->completion.reset();impl_->status.state=FrontendMovieState::Cancelled;
}
}
