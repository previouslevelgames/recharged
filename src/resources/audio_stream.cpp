#include "resources/audio_stream.h"
#include "revolution/thp/THPAdpcmStep.h"
#include "resources/chunk_reader.h"
#include <algorithm>
#include <bit>
#include <cmath>
#include <set>

namespace mscharged::resources
{
namespace
{
constexpr std::size_t Limit=4096;
std::vector<Chunk> Children(Bytes file,Bytes bytes)
{
    std::vector<Chunk> out;const auto end=std::size_t(bytes.data()-file.data())+bytes.size();
    for(auto at=std::size_t(bytes.data()-file.data());at<end;)
    {Require(out.size()<8192,"Excessive audio bank chunks");auto c=ReadChunk(file,at,end);Require(c.next<=end,"Audio chunk padding exceeds container");out.push_back(c);at=c.next;}
    return out;
}
Bytes Take(const std::vector<Chunk>& parts,std::size_t& at,std::uint32_t id,std::size_t count,std::size_t stride)
{Require(at<parts.size()&&parts[at].id==id,"Unexpected audio bank chunk order");auto b=parts[at++].payload;Require(count<=Limit&&b.size()==count*stride,"Invalid audio bank record extent");return b;}
void Address(std::uint32_t base,std::size_t count,std::size_t stride)
{Require(base%4==0&&std::uint64_t(base)+count*stride<=0x100000000ULL,"Audio serialized address range wraps");}
std::uint32_t Index(std::uint32_t value,std::uint32_t base,std::size_t count,std::size_t stride)
{Require(value>=base&&(value-base)%stride==0&&(value-base)/stride<count,"Audio reference is unaligned or outside its section");return (value-base)/stride;}
float Scalar(Bytes b,std::size_t at)
{auto v=F32(b,at);Require(std::isfinite(v)&&std::abs(v)<=1e6f,"Invalid audio scalar");return v;}
void Supported(bool value,const char* why){if(!value)throw UnsupportedResource(why);}
std::uint32_t SamplesBefore(std::uint32_t nibble)
{return nibble/16*14+std::max(int(nibble%16)-2,0);}
}
AudioStreamBank::Handle ReadAudioStreamBank(Bytes file,std::uint64_t wave_bytes)
{
    Require(file.size()<=MaximumAssetBytes&&wave_bytes&&wave_bytes<=256*1024*1024,"Audio bank size exceeds selected profile");
    const auto top=ReadChunk(file,0,file.size());Require(top.id==0x80000001&&top.next==file.size(),"Invalid audio bank root");
    std::map<std::uint32_t,Bytes> sections;
    for(const auto& c:Children(file,top.payload))
    {Supported(c.id==0x80023000||c.id==0x80023300||c.id==0x80023200,"Unselected audio bank section");Require(sections.emplace(c.id,c.payload).second,"Duplicate audio bank section");}
    Require(sections.size()==3,"Incomplete stream audio bank");
    auto out=std::shared_ptr<AudioStreamBank>(new AudioStreamBank);out->map_=ReadAudioCueCatalog(file);
    auto parts=Children(file,sections.at(0x80023300));std::size_t at=0;
    Require(!parts.empty()&&parts[0].id==0x23301&&(parts[0].payload.size()==56||parts[0].payload.size()==64),"Invalid audio resource header");
    const auto header=parts[at++].payload;
    const auto nc=U32(header,8),nv=U32(header,16),nq=U32(header,24),ne=U32(header,32);
    Require(nc&&nv&&nq&&ne&&nc<=Limit&&nv<=Limit&&nq<=Limit&&ne<=Limit,"Empty or excessive audio graph");
    Supported(U32(header,40)==0&&U32(header,48)==0,"Hit-marker/parameter events are not selected");
    if(header.size()==64)Supported(U32(header,56)==0&&U32(header,60)==0,"Unknown audio header extension");
    const auto vb=U32(header,20),qb=U32(header,28),eb=U32(header,36);Address(vb,nv,44);Address(qb,nq,12);Address(eb,ne,48);
    const auto cues=Take(parts,at,0x23302,nc,40),voices=Take(parts,at,0x23303,nv,44),seqs=Take(parts,at,0x23304,nq,12),sounds=Take(parts,at,0x23305,ne,48);
    Take(parts,at,0x23306,0,16);Take(parts,at,0x23307,0,24);
    for(std::uint32_t i=0;i<nc;++i)
    {
        auto b=cues.subspan(i*40,40);const auto count=U32(b,4);auto entries=Take(parts,at,0x23308,count,20);
        const auto mode=std::bit_cast<std::int32_t>(U32(b,16));
        Supported(b[12]==0&&mode>=0&&mode<=4,"Slider/disabled/unknown audio cue selection is unavailable");
        Require(count&&count<=64,"Invalid cue voice count");
        Supported(U32(b,24)==0xffff&&U32(b,28)==0,"Persisted active cue state is unsupported");
        AudioBankCue cue{U32(b,0),U32(b,32),mode,{}};float total=0;
        for(std::uint32_t j=0;j<count;++j)
        {
            const auto minimum=Scalar(entries,j*20+4),weight=Scalar(entries,j*20+8);
            Require(weight>0&&std::trunc(weight)==weight,"Cue weights must be positive exact integers");total+=weight;
            Require(total<=0x00ffffff,"Cue weight total exceeds exact source float/integer domain");
            Supported(U32(entries,j*20+12)==0,"Persisted cue selection count is unsupported");
            cue.voices.push_back({Index(U32(entries,j*20),vb,nv,44),minimum,weight});
        }
        out->cues_.push_back(std::move(cue));
    }
    for(std::uint32_t i=0;i<nv;++i)
    {
        auto b=voices.subspan(i*44,44);const auto count=U32(b,16),rpc=U32(b,24);
        auto refs=Take(parts,at,0x23309,count,4);Take(parts,at,0x2330c,rpc,4);
        Supported(!rpc&&!U32(b,32)&&!U32(b,36),"Audio RPC/modifier services are unavailable");Require(count&&count<=64,"Invalid voice sequence count");
        AudioBankVoice voice{U32(b,0),U32(b,12),Scalar(b,4),Scalar(b,8),{}};
        for(std::uint32_t j=0;j<count;++j)voice.sequences.push_back(Index(U32(refs,j*4),qb,nq,12));out->voices_.push_back(std::move(voice));
    }
    for(std::uint32_t i=0;i<nq;++i)
    {
        auto b=seqs.subspan(i*12,12);auto count=U32(b,4);auto refs=Take(parts,at,0x2330a,count,8);Require(count&&count<=64,"Invalid sequence event count");
        AudioBankSequence seq{Scalar(b,0),{}};
        for(std::uint32_t j=0;j<count;++j){Supported(U32(refs,j*8)==1,"Non-sound sequence event is unavailable");seq.events.push_back(Index(U32(refs,j*8+4),eb,ne,48));}out->sequences_.push_back(std::move(seq));
    }
    for(std::uint32_t i=0;i<ne;++i)
    {
        auto b=sounds.subspan(i*48,48);auto count=U32(b,8);auto refs=Take(parts,at,0x2330b,count,8);Require(count&&count<=256,"Invalid sound choice count");
        // Read the source byte fields individually, never infer flags from
        // the whole word: owned event1 has nonzero bytes in pad_16.
        AudioBankSound sound{U32(b,4),b[20]!=0,b[21]!=0,Scalar(b,24),Scalar(b,28),Scalar(b,32),Scalar(b,36),Scalar(b,40),Scalar(b,44),{}};
        Require(sound.pitch_min<=sound.pitch_max&&sound.volume_min<=sound.volume_max&&sound.delay_min>=0&&sound.delay_range>=0&&sound.delay_min+sound.delay_range<=1e6f,"Invalid sound parameter range");
        std::uint64_t total=0;
        for(std::uint32_t j=0;j<count;++j){const auto w=U32(refs,j*8+4);total+=w;Require(w&&total<=UINT32_MAX,"Invalid or overflowing sound weights");sound.choices.push_back({U32(refs,j*8),w});}
        out->sounds_.push_back(std::move(sound));
    }
    Require(at==parts.size(),"Trailing audio resource records");
    parts=Children(file,sections.at(0x80023200));at=0;
    auto sh=Take(parts,at,0x23201,1,12);Supported(sh[8]==1,"Resident source table is not a stream bank");
    out->block_=U32(sh,4);Require(out->block_>=32&&out->block_<=65536&&out->block_%32==0,"Invalid stream block size");
    const auto count=U32(sh,0);auto sources=Take(parts,at,0x23202,count,28);Require(count&&at==parts.size(),"Invalid stream source section");
    std::vector<std::pair<std::uint64_t,std::uint64_t>> ranges;
    for(std::uint32_t i=0;i<count;++i)
    {
        const auto b=sources.subspan(i*28,28);
        AudioStreamTrack track{U32(b,0),U32(b,4),U32(b,8),U32(b,12),U32(b,16)};
        Require(track.source==i&&track.offset%32==0&&track.byte_size>=224&&track.byte_size%32==0&&
            std::uint64_t(track.offset)+track.byte_size<=wave_bytes,"Invalid stream source extent");
        Supported(track.channels==2&&U32(b,20)==0,"Only ordinary stereo stream sources are selected");
        ranges.emplace_back(track.offset,std::uint64_t(track.offset)+track.byte_size);out->tracks_.push_back(track);
    }
    std::sort(ranges.begin(),ranges.end());
    for(std::size_t i=1;i<ranges.size();++i)Require(ranges[i-1].second<=ranges[i].first,"Overlapping stream source extents");
    for(const auto& sound:out->sounds_)for(auto choice:sound.choices)Require(choice.index<count,"Sound choice references absent stream");
    for(const auto& [key,index]:out->map_->cues)Require(index<nc,"SoundMap references absent cue");
    return out;
}
AudioStreamFormat::Handle ReadAudioStreamFormat(AudioStreamBank::Handle bank,std::uint32_t track,Bytes bytes)
{
    Require(bool(bank)&&bytes.size()==204,"IDSP requires exactly its prefix and two DSP headers");
    const auto& entry=bank->Tracks().at(track);
    Require(U32(bytes,0)==0x49445350&&U32(bytes,4)==bank->BlockBytes(),"Invalid IDSP magic or interleave size");
    auto out=std::shared_ptr<AudioStreamFormat>(new AudioStreamFormat);out->bank_=std::move(bank);out->track_=track;
    for(unsigned channel=0;channel<2;++channel)
    {
        auto b=bytes.subspan(12+96*channel,96);auto& h=out->channels_[channel];
        h.samples=U32(b,0);h.nibbles=U32(b,4);h.rate=U32(b,8);
        Require(h.samples&&h.nibbles>=3&&h.rate>=8000&&h.rate<=192000,"Invalid IDSP samples/rate");
        Require((h.nibbles%16==0||h.nibbles%16>=3)&&h.samples==SamplesBefore(h.nibbles),"IDSP sample/nibble totals disagree");
        Supported(U16(b,12)==0&&U16(b,14)==0&&U32(b,16)==2&&U32(b,24)==2&&U16(b,60)==0,
            "DSP embedded loop, format, alternate start or gain is unavailable");
        Require(U32(b,20)==h.nibbles-1,"IDSP inclusive endpoint differs");
        Require(U32(bytes,8)==Align((std::uint64_t(h.nibbles)+1)/2,32),"IDSP padded channel size differs");
        for(unsigned i=0;i<16;++i)h.coefficients[i]=std::bit_cast<std::int16_t>(U16(b,28+i*2));
        h.predictor_scale=U16(b,62);h.history1=std::bit_cast<std::int16_t>(U16(b,64));h.history2=std::bit_cast<std::int16_t>(U16(b,66));
        Require(h.predictor_scale<=127,"IDSP initial predictor is invalid");
    }
    const auto& a=out->channels_[0];const auto& b=out->channels_[1];
    Require(a.samples==b.samples&&a.nibbles==b.nibbles&&a.rate==b.rate,"IDSP stereo channel lengths/rates disagree");
    const auto channel_bytes=(std::uint64_t(a.nibbles)+1)/2;
    Require(channel_bytes<=entry.byte_size/2,"IDSP channel length exceeds source extent");
    out->cycle_frames_=SamplesBefore(static_cast<std::uint32_t>(Align(channel_bytes,32)*2));
    out->blocks_=static_cast<std::uint32_t>((channel_bytes+out->BlockBytes()-1)/out->BlockBytes());
    Require(Align(204+std::uint64_t(out->blocks_)*out->BlockBytes()*2,32)==entry.byte_size,"IDSP block extent differs from source table");
    return out;
}
std::uint32_t AudioStreamFormat::BlockOffset(std::uint32_t block,unsigned channel)const
{
    Require(block<blocks_&&channel<2,"IDSP block/channel is outside retained source");
    const auto at=std::uint64_t(Track().offset)+204+(std::uint64_t(block)*2+channel)*BlockBytes();
    Require(at+BlockBytes()<=std::uint64_t(Track().offset)+Track().byte_size,"IDSP block exceeds retained source");
    return static_cast<std::uint32_t>(at);
}
AudioStreamDecoder BeginAudioStream(AudioStreamFormat::Handle format)
{
    Require(bool(format),"Stream decoder requires a retained format");AudioStreamDecoder d;
    for(unsigned ch=0;ch<2;++ch){d.history1[ch]=format->Channels()[ch].history1;d.history2[ch]=format->Channels()[ch].history2;}return d;
}
AudioStreamPcmBlock DecodeAudioStreamBlock(AudioStreamFormat::Handle format,AudioStreamDecoder& state,Bytes left,Bytes right)
{
    Require(bool(format)&&state.next_block<format->Blocks(),"Stream decoder block is out of range");
    Require(left.size()==format->BlockBytes()&&right.size()==format->BlockBytes(),"IDSP refill must provide both complete blocks");
    auto next=state;AudioStreamPcmBlock pcm;
    const auto first=std::uint64_t(state.next_block)*format->BlockBytes()*2;
    const auto last=std::min<std::uint64_t>(first+format->BlockBytes()*2,Align((std::uint64_t(format->Channels()[0].nibbles)+1)/2,32)*2);
    pcm.frames=SamplesBefore(static_cast<std::uint32_t>(last))-SamplesBefore(static_cast<std::uint32_t>(first));
    pcm.stereo.resize(std::size_t(pcm.frames)*2);
    const std::array<Bytes,2> input{left,right};
    for(unsigned ch=0;ch<2;++ch)
    {
        const auto& h=format->Channels()[ch];
        if(state.next_block==0)Require(input[ch][0]==h.predictor_scale,"IDSP first frame/header predictor differs");
        std::uint16_t predictor=h.predictor_scale;std::size_t sample=0;
        for(std::uint64_t nibble=first;nibble<last;++nibble)
        {
            const auto local=nibble-first;
            if(nibble%16==0){predictor=input[ch][local/2];Require(predictor<=127,"IDSP frame predictor is outside coefficients");}
            if(nibble%16<2)continue;
            const auto raw=input[ch][local/2];const int v=(nibble&1)?raw&15:raw>>4;
            const int signed_nibble=v>=8?v-16:v;
            const auto acc=THPAdpcmAccumulator(signed_nibble,predictor&15,h.coefficients[(predictor>>4)*2],h.coefficients[(predictor>>4)*2+1],next.history1[ch],next.history2[ch]);
            const auto value=static_cast<std::int16_t>(acc>>16);
            next.history2[ch]=next.history1[ch];next.history1[ch]=value;pcm.stereo[sample++*2+ch]=value;
        }
        Require(sample==pcm.frames,"IDSP decoded sample count differs");
    }
    next.next_block=(state.next_block+1)%format->Blocks();state=next;return pcm;
}
}
