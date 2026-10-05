#include "runtime/frontend_movie_playback.h"
#include "runtime/startup.h"
#include "runtime/startup_files.h"
#include "NL/nlFileGC.h"
#include "NL/MemAlloc.h"
#include "RVL_SDK/thp/THPMovieSteps.h"
#include <aurora/aurora.h>
#include <aurora/dvd.h>
#include <SDL3/SDL.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <source_location>
#include <thread>
using namespace mscharged;
namespace
{
unsigned checks=0;
void Check(bool value,const char* text){++checks;if(!value)throw std::runtime_error(text);}
template<class F>void Reject(F f,std::source_location at=std::source_location::current()){++checks;try{f();}catch(const std::exception&){return;}throw std::runtime_error("Invalid movie operation accepted line "+std::to_string(at.line()));}
void Wait(FrontendMoviePlayback& movie,unsigned count,std::uint64_t retrace)
{
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(20);
    while(movie.Status().published_frames<count)
    {
        movie.Advance(retrace);Check(std::chrono::steady_clock::now()<deadline,"Movie publish timed out");SDL_Delay(1);
    }
}
void Equations()
{
    constexpr unsigned short table[]={
#include "RVL_SDK/thp/THPVolumeTable.inc"
    };
    Check(table[0]==0&&table[127]==32768,"Source volume table endpoints differ");
    // Independent signed integer oracle, deliberately using division/floor
    // rather than source shifts. Covers every int16 input at extreme gains.
    auto scaled=[](int value,unsigned gain){const long n=long(value)*gain;return n>=0?n/32768:-((-n+32767)/32768);};
    for(unsigned gain:{0u,2u,16384u,32768u})for(int value=-32768;value<=32767;++value)
    {
        const short game[2]={30000,-30000},pcm[2]={static_cast<short>(value),static_cast<short>(-value-1)};short output[2];
        THPMovieMixPair(output,game,pcm,gain,false);
        Check(output[0]==std::clamp(30000+scaled(pcm[0],gain),-32768l,32767l)&&output[1]==std::clamp(-30000+scaled(pcm[1],gain),-32768l,32767l),"Movie stereo mix differs from scalar oracle");
        THPMovieMixPair(output,game,pcm,gain,true);long sum=scaled(pcm[0],gain)+scaled(pcm[1],gain);const auto expected=sum>=0?sum/2:-((-sum+1)/2);
        Check(output[0]==expected&&output[1]==expected,"Movie mono mix differs from scalar oracle");
    }
    struct Volume{long rampCount=0;float curVolume=0,targetVolume=0,deltaVolume=0;} volume;
    THPMovieSetVolume(volume,64,1,32);Check(volume.rampCount==32&&volume.deltaVolume==2,"Original volume ramp admission differs");
    for(unsigned n=1;n<=32;++n)Check(THPMovieNextVolume(volume,table)==table[n*2],"Original exact ramp sample differs");
    Check(THPMovieNextVolume(volume,table)==table[64]&&volume.rampCount==0,"Original ramp target hold differs");
}
void Session(const char* path,unsigned count,bool generated,bool mono=false)
{
    const auto a=StandardAllocator.TotalFreeMemory(),b=VirtualAllocator.TotalFreeMemory();resources::ThpMovieFrameHandle retained;
    {
        FrontendMoviePlayback movie({1,path,true,false,false,true},{mono,100,0},100);
        Check(movie.Options().mono==mono&&movie.Options().volume_percent==100&&movie.Options().fade_in_ms==0&&movie.Options().device_id==0,"Movie output profile changed");
        Check(!movie.Current()&&!movie.Completion()&&movie.Status().next_retrace==100,"Movie initial publication differs");
        bool wrong=false;std::thread thread([&]{try{movie.Advance(100);}catch(...){wrong=true;}});thread.join();Check(wrong,"Cross-thread playback accepted");
        Reject([&]{movie.Advance(99);});Reject([&]{movie.Acknowledge({});});
        count=std::min(count,movie.Info().frame_count);
        std::uint64_t samples=0;
        for(unsigned n=0;n<count;++n)
        {
            Wait(movie,n+1,100+n*2);const auto frame=movie.Current();Check(frame&&frame->index==n,"Movie source frame order differs");
            samples+=frame->audio_samples;Check(movie.Status().submitted_audio_frames==samples,"Movie audio frame accounting differs");
            const auto previous=movie.Current();movie.Advance(100+n*2);movie.Advance(101+n*2);
            Check(movie.Current()==previous&&movie.Status().next_retrace==102+n*2,"Movie decoded early or used header FPS");
            if(!n)retained=frame;
        }
        if(count==movie.Info().frame_count)
        {
            Check(movie.Status().decoded_eof&&!movie.Completion(),"Decoded EOF fabricated movie completion");
            const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
            do{movie.Advance(100+count*2);SDL_Delay(1);Check(std::chrono::steady_clock::now()<deadline,"Movie stream did not drain");}
            while(movie.Status().queued_input_bytes||movie.Status().available_output_bytes);
            Check(movie.Status().state==FrontendMovieState::Draining&&!movie.Completion()&&!movie.Status().final_presented,"Audio drain fabricated a presentation receipt");
        }
        movie.Cancel();movie.Cancel();Reject([&]{movie.Advance(100+count*2);});Check(movie.Current()&&!movie.Completion(),"Cancel lost retained output or kept completion");
    }
    Check(retained&&!retained->y.empty(),"Movie frame did not outlive playback");Check(a==StandardAllocator.TotalFreeMemory()&&b==VirtualAllocator.TotalFreeMemory()&&!nlAsyncReadsPending(nullptr),"Movie teardown lost game arena/read ownership");
    std::cout<<path<<" playback frames="<<count<<" mono="<<mono<<'\n';
}
struct Runtime{bool live=false,disc=false;~Runtime(){if(live)ResetStartupFiles();if(disc)aurora_dvd_close();if(live){ResetStartupMemory();aurora_shutdown();}}};
}
int main(int argc,char** argv)
{
    try
    {
        Check(argc==4,"Playback test needs disc/output/generated-or-owned");const bool generated=std::string_view(argv[3])=="generated";const auto folder=std::filesystem::absolute(argv[2]);std::filesystem::create_directories(folder/"movie-runtime");const auto user=(folder/"movie-runtime").string();
        const bool disk=std::getenv("SDL_AUDIODRIVER")&&std::string_view(std::getenv("SDL_AUDIODRIVER"))=="disk";
        if(disk){SDL_SetHint(SDL_HINT_AUDIO_DISK_OUTPUT_FILE,(folder/"movie-audio.raw").string().c_str());SDL_SetHint(SDL_HINT_AUDIO_FORMAT,"S16LE");SDL_SetHint(SDL_HINT_AUDIO_FREQUENCY,"32000");SDL_SetHint(SDL_HINT_AUDIO_CHANNELS,"2");}
        Equations();
        Runtime runtime;AuroraConfig config{};config.appName="Charged original movie playback";config.userPath=config.cachePath=user.c_str();config.resourcesPath=SDL_GetBasePath();config.desiredBackend=BACKEND_NULL;config.windowWidth=320;config.windowHeight=240;config.windowPosX=config.windowPosY=-1;config.mem1Size=MEM1_DEFAULT_SIZE;config.mem2Size=64*1024*1024;config.logLevel=LOG_WARNING;
        Check(aurora_initialize(argc,argv,&config).window,"Aurora core failed");runtime.live=true;InitializeStartupOS();nlInitMemory();Check(aurora_dvd_open(argv[1]),"Movie disc failed");runtime.disc=true;nlInitFileSystem();
        if(generated)
        {
            if(disk)Session("/Art/movies/test.thp",3,true);
            else
            {
                for(unsigned n=0;n<2;++n){Session("/Art/movies/test.thp",3,true);Session("/Art/movies/test.thp",3,true,true);Session("/Art/movies/silent.thp",3,true);}
                {
                    FrontendMoviePlayback movie({4,"/Art/movies/test.thp"},{},0);
                    struct Probe{FrontendMoviePlayback* movie;bool called=false;}probe{&movie};
                    std::unique_ptr<nlFile> file(nlOpen("/Art/movies/test.thp"));alignas(32)std::array<unsigned char,32> data{};
                    const auto callback=+[](nlFile*,void*,unsigned,nlFileAsyncParam raw)
                    {
                        auto& probe=*reinterpret_cast<Probe*>(raw);probe.called=true;
                        Reject([&]{probe.movie->Advance(0);});Reject([&]{probe.movie->Cancel();});
                        Check(probe.movie->Status().state!=FrontendMovieState::Failed,"Rejected callback reentry poisoned live playback");
                    };
                    nlReadAsync(file.get(),data.data(),data.size(),callback,reinterpret_cast<nlFileAsyncParam>(&probe),data.size());
                    Wait(movie,1,0);const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
                    while(!probe.called){movie.Advance(0);Check(std::chrono::steady_clock::now()<deadline,"Movie callback guard fixture timed out");SDL_Delay(1);}
                    movie.Check();movie.Cancel();
                }
                {FrontendMoviePlayback movie({2,"/Art/movies/test.thp"},{},0);movie.Cancel();Check(!movie.Current()&&!nlAsyncReadsPending(nullptr),"Pending movie cancellation retained read");}
                {FrontendMoviePlayback movie({3,"/Art/movies/bad.thp"},{},0);Wait(movie,1,0);const auto old=movie.Current();Reject([&]{Wait(movie,2,2);});Check(movie.Status().state==FrontendMovieState::Failed&&movie.Current()==old&&!movie.Completion(),"Failed decode changed visible output");Reject([&]{movie.Check();});movie.Cancel();}
                Reject([&]{FrontendMoviePlayback movie({0,"/Art/movies/test.thp"},{},0);});
                Reject([&]{FrontendMoviePlayback movie({1,"/Art/movies/test.thp",true,false,true},{},0);});
                Reject([&]{FrontendMoviePlayback movie({1,"/Art/movies/test.thp",true,false,false,false},{},0);});
                Reject([&]{FrontendMoviePlayback movie({1,"/Art/movies/test.thp"},{false,101,0},0);});
                Reject([&]{FrontendMoviePlayback movie({1,"/Art/movies/missing.thp"},{},0);});
            }
            if(disk)
            {
                std::ifstream file(folder/"movie-audio.raw",std::ios::binary);std::vector<unsigned char> bytes(std::istreambuf_iterator<char>(file),{});
                std::vector<unsigned char> expected;for(unsigned n=0;n<14;++n)expected.insert(expected.end(),{1,0,254,255});
                Check(std::search(bytes.begin(),bytes.end(),expected.begin(),expected.end())!=bytes.end(),"Actual SDL output changed original gain or R,L to L,R mapping");
            }
        }
        else{Session("/Art/movies/nlgintrowide.thp",12,false);Session("/Art/movies/credits.thp",12,false,true);}
        std::cout<<checks<<" original movie scheduling/output checks passed\n";
    }
    catch(const std::exception& e){std::cerr<<"FAILED: "<<e.what()<<" (check "<<checks<<")\n";return 1;}
}
