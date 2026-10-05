#include "runtime/thp_movie.h"
#include "NL/nlFileGC.h"
#include "NL/MemAlloc.h"
#include <algorithm>
#include <exception>
#include <thread>
#include <stdexcept>

namespace mscharged
{
namespace
{
void Check(bool ok,const char* text){if(!ok)throw std::logic_error(text);}
struct BufferFree{void operator()(std::uint8_t* p)const noexcept{if(p)VirtualAllocator.Free(p);}};
using Buffer=std::unique_ptr<std::uint8_t,BufferFree>;
}
struct ThpMovie::Implementation
{
    const std::thread::id thread=std::this_thread::get_id();
    std::unique_ptr<nlFile> file;
    resources::ThpMovieInfo info;
    Buffer buffer;
    Frame current;
    std::uint32_t index=0,offset=0,size=0,previous=0;
    std::uint64_t sample=0;
    unsigned reads=0;
    bool completed=false,busy=false;
    ThpMovieState state=ThpMovieState::Ready;
    std::exception_ptr error;
    void Thread()const{Check(thread==std::this_thread::get_id(),"THP reader requires its NL owner thread");}
    void Mutable()const{Thread();Check(!busy&&!nlGetCurrentAsyncRead(),"THP mutation inside NL service/callback is unsupported");}
    static void Complete(nlFile* file,void* data,unsigned count,nlFileAsyncParam token)
    {
        auto& s=*reinterpret_cast<Implementation*>(token);
        try{s.Thread();Check(s.state==ThpMovieState::Reading&&!s.completed&&file==s.file.get()&&data==s.buffer.get()&&count==s.size,"THP frame callback identity differs");s.completed=true;}
        catch(...){if(!s.error)s.error=std::current_exception();}
    }
    void Drain(){if(file)nlCancelPendingAsyncReads(file.get(),nullptr);buffer.reset();file.reset();}
    void Fail(std::exception_ptr value){if(!error)error=value;Drain();state=ThpMovieState::Failed;}
    explicit Implementation(std::string path)
    {
        Mutable();Check(gMemoryInitialized&&nlFileSystemReady(),"THP requires real initialized NL memory/files");
        Check(!path.empty()&&path.size()<256&&path.find('\0')==std::string::npos,"Invalid THP disc path");
        file.reset(nlOpen(path.c_str()));Check(bool(file),"THP disc file is missing");
        const auto length=nlFileSize(file.get(),nullptr);Check(length>=48&&length<=1024u*1024*1024,"THP disc size exceeds profile");
        const auto bytes=std::min(length,4096u);Buffer header(static_cast<std::uint8_t*>(VirtualAllocator.Allocate(bytes,32,false)));
        nlRead(file.get(),header.get(),bytes,bytes);++reads;info=resources::ReadThpMovieInfo({header.get(),bytes},length);
        offset=info.data_offset;size=info.first_frame_bytes;previous=info.data_offset+info.data_bytes-info.last_frame_offset;
    }
    void Begin()
    {
        Check(state==ThpMovieState::Ready&&file&&nlFileSystemReady(),"THP cannot queue another frame in this state");
        try
        {
            buffer.reset(static_cast<std::uint8_t*>(VirtualAllocator.Allocate(size,32,false)));completed=false;state=ThpMovieState::Reading;
            nlSeek(file.get(),offset,0);++reads;
            const auto token=nlReadAsync(file.get(),buffer.get(),size,Complete,reinterpret_cast<nlFileAsyncParam>(this),size);
            Check(token||completed,"THP frame read was not queued");
        }
        catch(...){Fail(std::current_exception());throw;}
    }
    void Poll()
    {
        if(state!=ThpMovieState::Reading)return;
        try
        {
            if(error)std::rethrow_exception(error);
            Check(nlFileSystemReady(),"THP NL services stopped");
            if(!completed){Check(nlAsyncReadsPending(file.get()),"THP frame read failed or was cancelled");return;}
            auto next=resources::DecodeThpMovieFrame(info,{buffer.get(),size},index,offset,previous,sample);
            // All decoding/allocation is private until both video and PCM pass.
            sample+=next->audio_samples;offset+=size;previous=size;size=next->next_frame_bytes;++index;
            current=std::move(next);buffer.reset();
            state=index==info.frame_count?ThpMovieState::EndOfStream:ThpMovieState::Ready;
            if(state==ThpMovieState::EndOfStream)file.reset();
        }
        catch(...){Fail(std::current_exception());}
    }
};
ThpMovie::ThpMovie(std::string path):impl_(std::make_unique<Implementation>(std::move(path))){}
ThpMovie::~ThpMovie(){try{Cancel();}catch(...){std::terminate();}}
const resources::ThpMovieInfo& ThpMovie::Info()const{impl_->Thread();return impl_->info;}
void ThpMovie::BeginNext(){impl_->Mutable();impl_->Begin();}
void ThpMovie::Poll(){impl_->Mutable();impl_->Poll();}
void ThpMovie::Service()
{
    auto& s=*impl_;s.Mutable();if(s.state!=ThpMovieState::Reading)return;
    s.busy=true;
    try{nlServiceFileSystem();}catch(...){s.busy=false;s.Fail(std::current_exception());throw;}
    s.busy=false;s.Poll();
}
void ThpMovie::Cancel(){auto& s=*impl_;s.Mutable();if(s.state==ThpMovieState::Cancelled)return;s.Drain();s.state=ThpMovieState::Cancelled;}
void ThpMovie::RethrowFailure()const{impl_->Thread();if(impl_->error)std::rethrow_exception(impl_->error);}
ThpMovieState ThpMovie::State()const{impl_->Thread();return impl_->state;}
ThpMovie::Frame ThpMovie::Current()const{impl_->Thread();return impl_->current;}
unsigned ThpMovie::ReadCount()const{impl_->Thread();return impl_->reads;}
}
