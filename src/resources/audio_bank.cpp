#include "resources/audio_bank.h"
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
AudioResidentBank::Handle ReadAudioResidentBank(Bytes file,Bytes wave)
{
    Require(file.size()<=MaximumAssetBytes&&!wave.empty()&&wave.size()<=64*1024*1024,"Audio bank size exceeds selected profile");
    const auto top=ReadChunk(file,0,file.size());Require(top.id==0x80000001&&top.next==file.size(),"Invalid audio bank root");
    std::map<std::uint32_t,Bytes> sections;
    for(const auto& c:Children(file,top.payload))
    {Supported(c.id==0x80023000||c.id==0x80023300||c.id==0x23703||c.id==0x80023200,"Unselected audio bank section");Require(sections.emplace(c.id,c.payload).second,"Duplicate audio bank section");}
    Require(sections.size()==4,"Incomplete resident audio bank");
    auto out=std::shared_ptr<AudioResidentBank>(new AudioResidentBank);out->map_=ReadAudioCueCatalog(file);
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
    const auto sp=sections.at(0x23703);const auto ns=U32(sp,0);Require(ns&&ns<=Limit&&sp.size()==4+std::size_t(ns)*74,"Invalid resident SP table extent");
    for(std::uint32_t i=0;i<ns;++i)
    {
        const auto b=sp.subspan(4+i*28,28),a=sp.subspan(4+ns*28+i*46,46);
        Supported(U32(b,0)==0,"Only nonloop resident DSP ADPCM samples are selected");
        const auto rate=U32(b,4),first=U32(b,20),last=U32(b,16);
        Require(rate&&rate<=192000&&first<=last&&first%16>=2&&last%16>=2&&std::uint64_t(last)<wave.size()*2,"SP inclusive nibble range/rate is invalid");
        AudioDspSample s{};s.rate=rate;s.current_nibble=first;s.end_nibble=last;
        s.first_byte=(first/16)*8;s.byte_count=last/2+1-s.first_byte;s.sample_count=SamplesBefore(last+1)-SamplesBefore(first);
        Require(s.sample_count,"SP sample has no ADPCM payload");
        for(unsigned j=0;j<16;++j)s.coefficients[j]=std::bit_cast<std::int16_t>(U16(a,j*2));
        s.gain=U16(a,32);s.predictor_scale=U16(a,34);s.history1=std::bit_cast<std::int16_t>(U16(a,36));s.history2=std::bit_cast<std::int16_t>(U16(a,38));
        s.loop_predictor_scale=U16(a,40);s.loop_history1=std::bit_cast<std::int16_t>(U16(a,42));s.loop_history2=std::bit_cast<std::int16_t>(U16(a,44));
        Require(s.predictor_scale<=0x7f,"SP initial predictor is outside the coefficient table");
        out->samples_.push_back(s);
    }
    parts=Children(file,sections.at(0x80023200));at=0;auto sh=Take(parts,at,0x23201,1,12);Supported(sh[8]==0,"Stream audio source tables are unavailable");const auto count=U32(sh,0);auto sources=Take(parts,at,0x23202,count,28);Require(count&&at==parts.size(),"Invalid audio source section");
    for(std::uint32_t i=0;i<count;++i)
    {AudioBankSource source;for(unsigned j=0;j<6;++j)source.fields[j]=U32(sources,i*28+j*4);Require(source.fields[0]<ns,"Audio source references absent SP sample");out->sources_.push_back(source);}
    for(const auto& sound:out->sounds_)for(auto choice:sound.choices)Require(choice.index<count,"Sound choice references absent source");
    for(const auto& [key,index]:out->map_->cues)Require(index<nc,"SoundMap references absent cue");
    // No host address is published until all metadata and sample bounds pass.
    out->wave_.assign(wave.begin(),wave.end());return out;
}
Bytes AudioResidentBank::SampleBytes(std::uint32_t sample)const
{const auto& s=samples_.at(sample);return Bytes(wave_).subspan(s.first_byte,s.byte_count);}
}
