#pragma once
#include <bit>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <vector>

namespace audio_bank_fixture
{
using Data=std::vector<std::uint8_t>;
inline void Put(Data& b,std::size_t at,std::uint32_t n){for(unsigned j=0;j<4;++j)b.at(at+j)=n>>(24-8*j);}
inline Data Words(std::initializer_list<std::uint32_t> words){Data b(words.size()*4);unsigned i=0;for(auto v:words){Put(b,i,v);i+=4;}return b;}
inline std::uint32_t Float(float f){return std::bit_cast<std::uint32_t>(f);}
inline std::size_t Append(Data& b,unsigned id,const Data& v)
{auto at=b.size();b.resize(at+8);Put(b,at,id);Put(b,at+4,v.size());b.insert(b.end(),v.begin(),v.end());b.resize((b.size()+3)&~3u);return at+8;}
inline Data Wrap(unsigned id,const Data& v){Data b;Append(b,id,v);return b;}
struct Fixture
{
 Data bytes,wave;std::size_t header,cues,voices,sequences,sounds,refs,seqrefs,eventrefs,choices,sp,sources,map;
};
inline Fixture Make(unsigned voice_count=1,unsigned mode=3,bool delay=false,bool many=false)
{
 constexpr unsigned vb=0xf0001000,qb=0xe0002000,eb=0xd0003000;
 Fixture f;Data graph,map,sources,sp(4+5*74);
 auto h=Words({0,0,2,0,2,vb,2,qb,2,eb,0,0,0,0,0,0});f.header=Append(graph,0x23301,h);
 f.cues=Append(graph,0x23302,Words({0x99887766,1,0,0,3,255,0xffff,0,255,0,0x10203040,voice_count,0,0,mode,255,0xffff,0,255,0}));
 f.voices=Append(graph,0x23303,Words({0x10203040,0,0,1,1,0,0,0,0,0,0,0x99887766,0,0,1,1,0,0,0,0,0,0}));
 f.sequences=Append(graph,0x23304,Words({0,many?2u:1u,0,0,1,0}));
 f.sounds=Append(graph,0x23305,Words({0,1,4,0,0,0,0,0,0,0,Float(delay?1:0),Float(delay?2:0),0,1,1,0,0,0x0101aabb,Float(-1),Float(1),Float(-2),Float(2),0,0}));
 Append(graph,0x23306,{});Append(graph,0x23307,{});
 Append(graph,0x23308,Words({vb+44,0,Float(255),0,0x01020304}));
 Data refs=Words({vb,0,Float(2),0,0x01020304});if(voice_count==2){auto r=Words({vb+44,0,Float(3),0,0});refs.insert(refs.end(),r.begin(),r.end());}f.refs=Append(graph,0x23308,refs);
 f.seqrefs=Append(graph,0x23309,Words({qb}));Append(graph,0x2330c,{});Append(graph,0x23309,Words({qb+12}));Append(graph,0x2330c,{});
 auto events=Words({1,eb});if(many){auto e=Words({1,eb+48});events.insert(events.end(),e.begin(),e.end());}f.eventrefs=Append(graph,0x2330a,events);Append(graph,0x2330a,Words({1,eb+48}));
 f.choices=Append(graph,0x2330b,Words({3,1,4,2,0,3,1,4}));Append(graph,0x2330b,Words({2,255}));
 Append(map,0x23001,Words({2,0,0}));f.map=Append(map,0x23003,Words({0x99887766,0,0,0,0,0x10203040,0,0,0,1}));
 Put(sp,0,5);f.wave.resize(80);for(unsigned i=0;i<f.wave.size();++i)f.wave[i]=i;
 for(unsigned i=0;i<5;++i)
 {
  auto off=4+i*28;Put(sp,off,0);Put(sp,off+4,i==2?44100:32000);Put(sp,off+8,0xffffffff);Put(sp,off+12,0xffffffff);Put(sp,off+16,i*32+31);Put(sp,off+20,i*32+2);Put(sp,off+24,0xfdfdfdfd);
  auto a=4+5*28+i*46;sp[a]=sp[a+1]=0xff;sp[a+34]=0;sp[a+35]=0x24;sp[a+36]=0xff;sp[a+37]=0xfe;sp[a+39]=3;
 }
 Append(sources,0x23201,Words({5,0,0x00ab1234}));Data records;for(unsigned i=0;i<5;++i){auto b=Words({i,0,0,0xab000000+i,2,0,0xffffffff});records.insert(records.end(),b.begin(),b.end());}f.sources=Append(sources,0x23202,records);
 Data root;auto mb=Append(root,0x80023000,map)+8;auto gb=Append(root,0x80023300,graph)+8;
 f.sp=Append(root,0x23703,sp)+8;auto sb=Append(root,0x80023200,sources)+8;
 for(auto* p:{&f.header,&f.cues,&f.voices,&f.sequences,&f.sounds,&f.refs,&f.seqrefs,&f.eventrefs,&f.choices})*p+=gb;
 f.map+=mb;f.sources+=sb;f.bytes=Wrap(0x80000001,root);return f;
}
}
