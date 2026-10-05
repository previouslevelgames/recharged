#pragma once
#include "resources/thp_movie.h"
#include <string>
namespace mscharged
{
enum class ThpMovieState { Ready, Reading, EndOfStream, Failed, Cancelled };
// CPU reader only. EndOfStream attests complete sequential decoding, never
// presentation/audio consumption or original MovieIsDone. NL owner thread only;
// destroy before file/memory shutdown. Returned immutable frames outlive owner.
class ThpMovie
{
    struct Implementation;
    std::unique_ptr<Implementation> impl_;
public:
    using Frame=resources::ThpMovieFrameHandle;
    explicit ThpMovie(std::string path);
    ~ThpMovie();
    ThpMovie(const ThpMovie&)=delete;
    ThpMovie& operator=(const ThpMovie&)=delete;
    const resources::ThpMovieInfo& Info()const;
    void BeginNext();
    void Poll();
    void Service();
    void Cancel(); // Drains actual worker before closing/freeing; keeps Current.
    void RethrowFailure()const;
    ThpMovieState State()const;
    Frame Current()const; // Last successful frame retained even on later failure.
    unsigned ReadCount()const;
};
}
