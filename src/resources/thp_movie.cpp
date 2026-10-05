#include "resources/thp_movie.h"
#include <dolphin/thp.h>
#include <bit>
#include <cmath>
#include <stdexcept>
#include <limits>

namespace mscharged::resources
{
namespace
{
using Bytes=std::span<const std::uint8_t>;
void Check(bool ok,const char* text){if(!ok)throw std::invalid_argument(text);}
std::uint32_t U32(Bytes b,std::size_t p)
{Check(p<=b.size()&&4<=b.size()-p,"Truncated THP metadata");return std::uint32_t(b[p])<<24|std::uint32_t(b[p+1])<<16|std::uint32_t(b[p+2])<<8|b[p+3];}
void Validate(const ThpMovieInfo& i)
{
    Check(i.width&&i.width<=1920&&i.height&&i.height<=1088&&i.width%16==0&&i.height%16==0&&i.video_type==0,"Unsupported THP video dimensions/type");
    Check(std::isfinite(i.frame_rate)&&i.frame_rate>=1&&i.frame_rate<=120&&i.frame_count&&i.frame_count<=432000,"Invalid THP timing");
    Check(i.component_count>=1&&i.component_count<=2,"Invalid THP component count");
    unsigned video=0,audio=0;for(unsigned n=0;n<i.component_count;++n){Check(i.components[n]<=1,"Unsupported THP component");video+=i.components[n]==0;audio+=i.components[n]==1;}
    Check(video==1&&audio<=1,"Duplicate/missing THP components");
    Check(i.max_frame_bytes>=12&&i.max_frame_bytes<=8*1024*1024&&i.first_frame_bytes>=12&&i.first_frame_bytes<=i.max_frame_bytes,"THP frame exceeds bounded profile");
    Check(i.data_offset>=48&&i.data_bytes&&std::uint64_t(i.data_offset)+i.data_bytes<=1024ull*1024*1024&&i.first_frame_bytes<=i.data_bytes&&i.last_frame_offset>=i.data_offset&&i.last_frame_offset<std::uint64_t(i.data_offset)+i.data_bytes,"Invalid THP movie range");
    if(audio)Check((i.channels==1||i.channels==2)&&i.sample_rate>=8000&&i.sample_rate<=48000&&i.max_audio_samples&&i.max_audio_samples<=16384&&i.total_audio_samples>=i.frame_count&&std::uint64_t(i.total_audio_samples)<=std::uint64_t(i.frame_count)*i.max_audio_samples,"Unsupported THP audio profile");
    else Check(!i.channels&&!i.sample_rate&&!i.total_audio_samples&&!i.max_audio_samples,"Silent THP contains audio metadata");
}
}
ThpMovieInfo ReadThpMovieInfo(Bytes b,std::uint64_t size)
{
    Check(b.size()>=48&&size>=b.size()&&size<=1024ull*1024*1024,"Invalid THP file/prefix size");
    Check(U32(b,0)==0x54485000&&U32(b,4)==0x11000,"Unsupported THP magic/version");
    ThpMovieInfo i;i.max_frame_bytes=U32(b,8);i.max_audio_samples=U32(b,12);i.frame_rate=std::bit_cast<float>(U32(b,16));i.frame_count=U32(b,20);i.first_frame_bytes=U32(b,24);i.data_bytes=U32(b,28);const auto table=U32(b,32);
    Check(U32(b,36)==0,"Indexed THP files are outside the sequential profile");
    i.data_offset=U32(b,40);i.last_frame_offset=U32(b,44);Check(table>=48&&table<=4096-48&&table<=b.size()&&20<=b.size()-table,"Invalid THP component table");
    i.component_count=U32(b,table);Check(i.component_count>=1&&i.component_count<=2,"Unsupported THP component count");
    auto pos=table+20;
    for(unsigned n=0;n<i.component_count;++n)
    {
        const auto type=b[table+4+n];i.components[n]=type;
        if(type==0){i.width=U32(b,pos);i.height=U32(b,pos+4);i.video_type=U32(b,pos+8);pos+=12;}
        else if(type==1){i.channels=U32(b,pos);i.sample_rate=U32(b,pos+4);i.total_audio_samples=U32(b,pos+8);Check(U32(b,pos+12)==1,"Multiple THP audio tracks are unqualified");pos+=16;}
        else throw std::invalid_argument("Unsupported THP component type");
    }
    Check(pos<=i.data_offset&&std::uint64_t(i.data_offset)+i.data_bytes==size,"THP metadata/data range differs from file");Validate(i);return i;
}
ThpMovieFrameHandle DecodeThpMovieFrame(const ThpMovieInfo& i,Bytes b,std::uint32_t index,std::uint32_t offset,std::uint32_t previous,std::uint64_t firstSample)
{
    Validate(i);const std::uint64_t end=std::uint64_t(i.data_offset)+i.data_bytes;
    Check(index<i.frame_count&&offset>=i.data_offset&&offset<end&&b.size()>=8+i.component_count*4&&b.size()<=i.max_frame_bytes&&b.size()<=end-offset,"Invalid THP frame range");
    Check(index?offset>i.data_offset:offset==i.data_offset&&b.size()==i.first_frame_bytes,"THP frame sequence identity differs");
    Check(U32(b,4)==previous,"THP previous frame size differs");
    auto frame=std::make_shared<ThpMovieFrame>();frame->index=index;frame->file_offset=offset;frame->encoded_bytes=b.size();frame->next_frame_bytes=U32(b,0);frame->width=i.width;frame->height=i.height;frame->video_seconds=double(index)/double(i.frame_rate);frame->sample_rate=i.sample_rate;frame->first_audio_sample=firstSample;
    if(index+1==i.frame_count)Check(offset==i.last_frame_offset&&offset+b.size()==end&&frame->next_frame_bytes==i.first_frame_bytes,"THP final frame does not close original loop");
    else Check(offset<i.last_frame_offset&&frame->next_frame_bytes>=8+i.component_count*4&&frame->next_frame_bytes<=i.max_frame_bytes&&frame->next_frame_bytes<=end-offset-b.size(),"THP next frame range is invalid");
    std::size_t pos=8+i.component_count*4;Bytes video,audio;
    for(unsigned n=0;n<i.component_count;++n){const auto count=U32(b,8+4*n);Check(count&&count<=b.size()-pos,"THP component exceeds frame");auto part=b.subspan(pos,count);if(i.components[n]==0)video=part;else audio=part;pos+=count;}
    Check(b.size()-pos<32,"THP frame has excessive trailing padding");
    frame->y.resize(std::size_t(i.width)*i.height);frame->u.resize(frame->y.size()/4);frame->v.resize(frame->u.size());
    Check(AuroraTHPVideoDecodeBounded(video.data(),video.size(),frame->y.data(),frame->y.size(),frame->u.data(),frame->u.size(),frame->v.data(),frame->v.size(),i.width,i.height)==0,"THP video decoder rejected frame");
    if(!audio.empty())
    {
        Check(audio.size()>=80,"THP audio header is truncated");const auto samples=U32(audio,4),channelOffset=U32(audio,0);
        Check(samples&&samples<=i.max_audio_samples&&firstSample<=i.total_audio_samples&&samples<=i.total_audio_samples-firstSample,"THP audio sample range is invalid");
        Check(i.channels==1?channelOffset==0:channelOffset!=0,"THP audio channel metadata differs");
        frame->pcm_right_left.resize(std::size_t(samples)*2);
        Check(AuroraTHPAudioDecodeBounded(frame->pcm_right_left.data(),frame->pcm_right_left.size(),audio.data(),audio.size(),0)==samples,"THP audio decoder rejected frame");frame->audio_samples=samples;
    }
    else Check(firstSample==0,"Silent THP has an audio cursor");
    if(index+1==i.frame_count)Check(firstSample+frame->audio_samples==i.total_audio_samples,"THP decoded sample total differs");
    return frame;
}
}
