#include "runtime/thp_movie.h"
#include "runtime/startup.h"
#include "runtime/startup_files.h"
#include "NL/nlFileGC.h"
#include "NL/MemAlloc.h"
#include <dolphin/thp.h>
#include <aurora/aurora.h>
#include <aurora/dvd.h>
#include <SDL3/SDL.h>
#include <algorithm>
#include <bit>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <source_location>
#include <thread>
using namespace mscharged;
using namespace mscharged::resources;
namespace
{
unsigned checks=0;
using Data=std::vector<std::uint8_t>;
void Check(bool ok,const char* text){++checks;if(!ok)throw std::runtime_error(text);}
template<class F>void Reject(F f,std::source_location at=std::source_location::current()){++checks;try{f();}catch(const std::exception&){return;}throw std::runtime_error("Invalid THP accepted at "+std::to_string(at.line()));}
unsigned U32(std::span<const std::uint8_t>b,unsigned p){return unsigned(b[p])<<24|unsigned(b[p+1])<<16|unsigned(b[p+2])<<8|b[p+3];}
void Put(Data& b,unsigned p,unsigned v){for(unsigned j=0;j<4;++j)b[p+j]=v>>(24-j*8);}
Data Disc(const char* path,unsigned offset=0,unsigned size=0){std::unique_ptr<nlFile> f(nlOpen(path));Check(bool(f),"THP fixture missing");if(!size)size=nlFileSize(f.get(),nullptr);Data b(size);nlSeek(f.get(),offset,0);nlRead(f.get(),b.data(),size,size);return b;}
Data Host(const std::filesystem::path& path){std::ifstream f(path,std::ios::binary);return Data(std::istreambuf_iterator<char>(f),{});}
std::uint64_t Hash(std::span<const std::uint8_t> b){std::uint64_t h=14695981039346656037ull;for(auto v:b){h^=v;h*=1099511628211ull;}return h;}
void Pump(ThpMovie& movie){const auto end=std::chrono::steady_clock::now()+std::chrono::seconds(20);while(movie.State()==ThpMovieState::Reading){movie.Service();if(std::chrono::steady_clock::now()>end)throw std::runtime_error("THP read timeout");if(movie.State()==ThpMovieState::Reading)SDL_Delay(1);}movie.RethrowFailure();}
std::vector<std::int16_t> AudioOracle(std::span<const std::uint8_t> a)
{
    const auto count=U32(a,4),offset=U32(a,0);std::vector<std::int16_t> result(count*2);
    auto s16=[&](unsigned p){return std::bit_cast<std::int16_t>(std::uint16_t(unsigned(a[p])<<8|a[p+1]));};
    for(unsigned channel=0;channel<2;++channel)
    {
        const bool right=channel==0&&offset!=0;const auto start=80+(right?offset:0);const unsigned coef=right?40:8;std::int64_t y1=s16(right?76:72),y2=s16(right?78:74);
        for(unsigned n=0;n<count;++n)
        {
            const auto block=start+(n/14)*8,within=n%14;const unsigned descriptor=a[block];int nibble=(a[block+1+within/2]>>(within%2?0:4))&15;if(nibble>=8)nibble-=16;
            const auto predictor=(descriptor>>4)&7;const std::int64_t sum=std::int64_t(nibble)*(1ll<<(descriptor&15))*2048+s16(coef+predictor*4)*y1+s16(coef+predictor*4+2)*y2;
            auto scaled=std::clamp(sum*32+32768,std::int64_t(INT32_MIN),std::int64_t(INT32_MAX));const auto sample=scaled>=0?scaled/65536:-((-scaled+65535)/65536);
            result[2*n+channel]=std::int16_t(sample);y2=y1;y1=sample;
        }
    }return result;
}
void Verify(const ThpMovieInfo& info,const ThpMovieFrameHandle& frame,const char* path,bool generated)
{
    auto bytes=Disc(path,frame->file_offset,frame->encoded_bytes);unsigned pos=8+4*info.component_count;std::span<const std::uint8_t> video,audio;
    for(unsigned n=0;n<info.component_count;++n){const auto size=U32(bytes,8+n*4);auto b=std::span<const std::uint8_t>(bytes).subspan(pos,size);if(info.components[n]==0)video=b;else audio=b;pos+=size;}
    Check(frame->video_seconds==double(frame->index)/info.frame_rate,"THP video timestamp differs");
    if(!audio.empty())Check(frame->pcm_right_left==AudioOracle(audio),"PCM differs from independent DSP oracle");
    Data y(frame->y.size()),u(frame->u.size()),v(frame->v.size());Check(THPVideoDecode(video.data(),y.data(),u.data(),v.data(),nullptr)==0,"Legacy valid video rejected");Check(y==frame->y&&u==frame->u&&v==frame->v,"Bounded video changed valid legacy output");
    if(generated){Check(std::all_of(y.begin(),y.end(),[&](auto value){return value==(frame->index?128:129);}),"Independent DC pixel oracle differs");Check(std::all_of(u.begin(),u.end(),[](auto value){return value==128;})&&u==v,"Independent chroma oracle differs");}
}
void DecoderBounds(const std::filesystem::path& folder)
{
    auto video=Host(folder/"dc.video");Data y(258,0xa5),u(66,0xa5),v(66,0xa5);
    auto decode=[&](const Data& data,unsigned bytesY=256,unsigned width=16){return AuroraTHPVideoDecodeBounded(data.data(),data.size(),y.data()+1,bytesY,u.data()+1,64,v.data()+1,64,width,16);};
    Check(decode(video)==0,"Generated THP video failed");Check(y.front()==0xa5&&y.back()==0xa5&&u.front()==0xa5&&u.back()==0xa5&&v.front()==0xa5&&v.back()==0xa5,"THP video wrote outside planes");
    for(unsigned n=0;n<video.size();++n){Data shortData(video.begin(),video.begin()+n);Check(decode(shortData)!=0,"Truncated entropy/header accepted");}
    Check(decode(video,255)!=0&&decode(video,256,32)!=0,"Short output or wrong dimensions accepted");
    auto huge=Host(folder/"overflow.video");Data a(1024*8192),b(a.size()/4),c(b.size());Check(AuroraTHPVideoDecodeBounded(huge.data(),huge.size(),a.data(),a.size(),b.data(),b.size(),c.data(),c.size(),1024,8192)!=0,"Malformed DC predictor overflow was accepted");
    Data audio(96);Put(audio,0,8);Put(audio,4,14);for(unsigned n=81;n<88;++n)audio[n]=0x11;for(unsigned n=89;n<96;++n)audio[n]=0xee;
    std::vector<std::int16_t> pcm(30,12345);Check(AuroraTHPAudioDecodeBounded(pcm.data()+1,28,audio.data(),audio.size(),0)==14,"Generated bounded audio failed");Check(pcm.front()==12345&&pcm.back()==12345,"Audio output guard changed");Check(std::equal(pcm.begin()+1,pcm.end()-1,AudioOracle(audio).begin()),"Generated audio scalar oracle differs");
    for(unsigned n=0;n<audio.size();++n)Check(AuroraTHPAudioDecodeBounded(pcm.data()+1,28,audio.data(),n,0)==0,"Truncated audio accepted");
    Check(AuroraTHPAudioDecodeBounded(pcm.data()+1,27,audio.data(),audio.size(),0)==0,"Short PCM output accepted");Check(AuroraTHPAudioDecodeBounded(pcm.data()+1,28,audio.data(),audio.size(),2)==0,"Unknown audio layout accepted");
    for(auto p:{0u,4u}){auto bad=audio;Put(bad,p,UINT32_MAX);Check(AuroraTHPAudioDecodeBounded(pcm.data()+1,28,bad.data(),bad.size(),0)==0,"Oversized audio metadata accepted");}
}
void Malformed()
{
    const auto good=Disc("/Art/movies/test.thp");const auto info=ReadThpMovieInfo(good,good.size());
    for(unsigned n=0;n<96;++n)Reject([&]{ReadThpMovieInfo(std::span<const std::uint8_t>(good).first(n),good.size());});
    for(auto p:{0u,4u,8u,12u,16u,20u,24u,28u,32u,36u,40u,44u,48u,68u,72u,76u,80u,84u,88u,92u}){auto bad=good;Put(bad,p,UINT32_MAX);Reject([&]{ReadThpMovieInfo(bad,bad.size());});}
    auto bad=good;bad[53]=0;Reject([&]{ReadThpMovieInfo(bad,bad.size());});
    const auto frame=std::span<const std::uint8_t>(good).subspan(info.data_offset,info.first_frame_bytes);const auto previous=info.data_offset+info.data_bytes-info.last_frame_offset;
    for(unsigned n=0;n<16;++n)Reject([&]{DecodeThpMovieFrame(info,frame.first(n),0,info.data_offset,previous,0);});
    Reject([&]{DecodeThpMovieFrame(info,frame,1,info.data_offset,previous,0);});Reject([&]{DecodeThpMovieFrame(info,frame,0,info.data_offset,previous+1,0);});Reject([&]{DecodeThpMovieFrame(info,frame,0,info.data_offset,previous,info.total_audio_samples);});
    for(unsigned p:{0u,4u,8u,12u}){Data corrupt(frame.begin(),frame.end());Put(corrupt,p,UINT32_MAX);Reject([&]{DecodeThpMovieFrame(info,corrupt,0,info.data_offset,previous,0);});}
}
void Movie(const char* path,unsigned count,bool generated)
{
    const auto a=StandardAllocator.TotalFreeMemory(),b=VirtualAllocator.TotalFreeMemory();ThpMovie::Frame retained;
    {
        ThpMovie movie(path);Check(movie.State()==ThpMovieState::Ready&&!movie.Current()&&movie.ReadCount()==1,"THP initial state differs");const auto info=movie.Info();std::uint64_t sample=0;
        bool thread_rejected=false;std::thread thread([&]{try{movie.BeginNext();}catch(...){thread_rejected=true;}});thread.join();Check(thread_rejected,"Cross-thread movie read accepted");
        count=std::min(count,info.frame_count);
        for(unsigned index=0;index<count;++index){auto old=movie.Current();movie.BeginNext();Check(movie.Current()==old,"Pending frame destroyed prior publication");Reject([&]{movie.BeginNext();});Pump(movie);auto frame=movie.Current();Check(frame&&frame!=old&&frame->index==index&&frame->first_audio_sample==sample,"Movie sequence/sample cursor differs");Verify(info,frame,path,generated);sample+=frame->audio_samples;if(index==0)retained=frame;}
        if(count==info.frame_count){Check(movie.State()==ThpMovieState::EndOfStream&&sample==info.total_audio_samples,"THP EOF/sample total differs");Reject([&]{movie.BeginNext();});}
        else{Check(movie.State()==ThpMovieState::Ready,"Partial decode fabricated EOF");movie.BeginNext();movie.Cancel();Check(movie.State()==ThpMovieState::Cancelled&&movie.Current(),"Cancel lost last decoded frame");}
        movie.Cancel();Reject([&]{movie.BeginNext();});
    }
    Check(retained&&retained->y.size()>0&&retained->index==0,"Frame did not outlive movie");Check(StandardAllocator.TotalFreeMemory()==a&&VirtualAllocator.TotalFreeMemory()==b&&!nlAsyncReadsPending(nullptr),"THP cleanup lost arena bytes/reads");
    std::cout<<path<<" decoded="<<count<<" first_Y="<<Hash(retained->y)<<" first_PCM="<<Hash({reinterpret_cast<const std::uint8_t*>(retained->pcm_right_left.data()),retained->pcm_right_left.size()*2})<<'\n';
}
void OwnedIndex(const char* path)
{
    // Separate raw container walk: no production index/cursor helper is used.
    auto bytes=Disc(path);const auto info=ReadThpMovieInfo(std::span<const std::uint8_t>(bytes).first(std::min<std::size_t>(bytes.size(),4096)),bytes.size());
    unsigned offset=U32(bytes,40),size=U32(bytes,24),previous=unsigned(bytes.size())-U32(bytes,44);std::uint64_t samples=0;
    for(unsigned n=0;n<U32(bytes,20);++n)
    {
        Check(offset<=bytes.size()&&size<=bytes.size()-offset&&size>=16,"Independent frame chain exceeds source");
        Check(U32(bytes,offset+4)==previous,"Independent previous-frame chain differs");const auto video=U32(bytes,offset+8),audio=U32(bytes,offset+12);Check(16ull+video+audio<=size&&size-16ull-video-audio<32,"Independent component ranges differ");
        if(n==info.frame_count/2||n+1==info.frame_count)
        {
            auto frame=DecodeThpMovieFrame(info,std::span<const std::uint8_t>(bytes).subspan(offset,size),n,offset,previous,samples);Verify(info,frame,path,false);
            std::cout<<path<<" sampled="<<n<<" Y="<<Hash(frame->y)<<" audio_samples="<<frame->audio_samples<<'\n';
        }
        samples+=U32(bytes,offset+16+video+4);previous=size;size=U32(bytes,offset);offset+=previous;
    }
    Check(offset==bytes.size()&&size==U32(bytes,24)&&samples==U32(bytes,88),"Independent full container/sample total differs");
}
struct Runtime{bool live=false,disc=false;~Runtime(){if(live)ResetStartupFiles();if(disc)aurora_dvd_close();if(live){ResetStartupMemory();aurora_shutdown();}}};
}
int main(int argc,char** argv)
{
    try
    {
        Check(argc==4,"THP test needs disc/output/generated-or-owned");const bool generated=std::string_view(argv[3])=="generated";const auto folder=std::filesystem::absolute(argv[2]);std::filesystem::create_directories(folder/"thp-runtime");const auto user=(folder/"thp-runtime").string();
        if(generated)DecoderBounds(folder);
        Runtime runtime;AuroraConfig config{};config.appName="Charged THP CPU";config.userPath=config.cachePath=user.c_str();config.resourcesPath=SDL_GetBasePath();config.desiredBackend=BACKEND_NULL;config.windowWidth=320;config.windowHeight=240;config.windowPosX=config.windowPosY=-1;config.mem1Size=MEM1_DEFAULT_SIZE;config.mem2Size=64*1024*1024;config.logLevel=LOG_WARNING;
        Check(aurora_initialize(argc,argv,&config).window,"Aurora core failed");runtime.live=true;InitializeStartupOS();nlInitMemory();Check(aurora_dvd_open(argv[1]),"THP disc failed");runtime.disc=true;nlInitFileSystem();
        if(generated)
        {
            Malformed();for(unsigned n=0;n<2;++n){Movie("/Art/movies/test.thp",3,true);Movie("/Art/movies/mono.thp",3,true);Movie("/Art/movies/silent.thp",3,true);}
            const auto before=VirtualAllocator.TotalFreeMemory();{ThpMovie movie("/Art/movies/bad.thp");movie.BeginNext();Pump(movie);auto old=movie.Current();movie.BeginNext();Reject([&]{Pump(movie);});Check(movie.State()==ThpMovieState::Failed&&movie.Current()==old,"Failed later frame replaced prior output");Reject([&]{movie.BeginNext();});movie.Cancel();}Check(before==VirtualAllocator.TotalFreeMemory(),"Failed movie leaked arena");Reject([&]{ThpMovie missing("/Art/movies/missing.thp");});
            {ThpMovie movie("/Art/movies/test.thp");movie.BeginNext();movie.Cancel();Check(movie.State()==ThpMovieState::Cancelled&&!movie.Current()&&!nlAsyncReadsPending(nullptr),"Pending movie cancellation retained a worker or published output");}
        }
        else{Movie("/Art/movies/nlgintrowide.thp",450,false);Movie("/Art/movies/credits.thp",12,false);OwnedIndex("/Art/movies/nlgintrowide.thp");OwnedIndex("/Art/movies/credits.thp");}
        std::cout<<checks<<" bounded THP decoder/stream checks passed\n";
    }
    catch(const std::exception& e){std::cerr<<"FAILED: "<<e.what()<<" (check "<<checks<<")\n";return 1;}
}
