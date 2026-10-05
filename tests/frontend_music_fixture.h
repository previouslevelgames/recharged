#pragma once
#include "audio_bank_fixture.h"
#include <algorithm>
#include <array>
namespace frontend_music_fixture
{
using audio_bank_fixture::Data;using audio_bank_fixture::Put;using audio_bank_fixture::Words;
using audio_bank_fixture::Float;using audio_bank_fixture::Append;using audio_bank_fixture::Wrap;
inline void Half(Data& b,unsigned at,unsigned v){b.at(at)=v>>8;b.at(at+1)=v;}
struct Fixture{Data metadata,wave;unsigned block=1024;};
inline Fixture Make(bool finite=false)
{
    Fixture f;Data map,graph,sources;constexpr unsigned vb=0x80000100,qb=0x80000200,eb=0x80000300;
    Append(map,0x23001,Words({2,0,0}));Append(map,0x23003,Words({0xe326f931,0,0,0,0,0x445abf3a,0,0,0,1}));
    Append(graph,0x23301,Words({0,0,2,0,2,vb,2,qb,2,eb,0,0,0,0,0,0}));
    Append(graph,0x23302,Words({0xe326f931,1,0,0,3,255,0xffff,0,255,0,0x445abf3a,1,0,0,3,255,0xffff,0,255,0}));
    Append(graph,0x23303,Words({1,Float(-6),0,2,1,0,0,0,0,0,0,2,Float(-7.5f),0,2,1,0,0,0,0,0,0}));
    Append(graph,0x23304,Words({0,1,0,0,1,0}));
    Data events;for(unsigned i=0;i<2;++i){auto e=Words({0,finite?1u:0xffffffffu,1,0,0,0,0,0,0,0,0,0});events.insert(events.end(),e.begin(),e.end());}
    Append(graph,0x23305,events);Append(graph,0x23306,{});Append(graph,0x23307,{});
    for(unsigned i=0;i<2;++i)Append(graph,0x23308,Words({vb+i*44,0,Float(255),0,0}));
    for(unsigned i=0;i<2;++i){Append(graph,0x23309,Words({qb+i*12}));Append(graph,0x2330c,{});}
    for(unsigned i=0;i<2;++i)Append(graph,0x2330a,Words({1,eb+i*48}));
    for(unsigned i=0;i<2;++i)Append(graph,0x2330b,Words({i,255}));
    Data records;
    for(unsigned track=0;track<2;++track)
    {
        constexpr unsigned nibbles=4135,frames=3617; // two full blocks and a partial third.
        constexpr unsigned size=6368;
        auto record=Words({track,unsigned(f.wave.size()),size,track?0x445abf3a:0xe326f931,2,0,0});records.insert(records.end(),record.begin(),record.end());
        Data wave(size);Put(wave,0,0x49445350);Put(wave,4,f.block);Put(wave,8,2080);
        for(unsigned ch=0;ch<2;++ch)
        {
            const auto at=12+96*ch;Put(wave,at,frames);Put(wave,at+4,nibbles);Put(wave,at+8,44100);Put(wave,at+16,2);Put(wave,at+20,nibbles-1);Put(wave,at+24,2);
            // Predictor0 histories matter; source wraps retain them.
            Half(wave,at+28,1024);Half(wave,at+30,unsigned(-512));Half(wave,at+62,2);Half(wave,at+64,100+ch);Half(wave,at+66,unsigned(-200-int(ch)));
            for(unsigned block=0;block<3;++block)for(unsigned j=0;j<f.block;++j)
                wave[204+(block*2+ch)*f.block+j]=j%8==0?2:std::uint8_t((track*17+ch*49+block*5+j*19)%256);
        }
        f.wave.insert(f.wave.end(),wave.begin(),wave.end());
    }
    Append(sources,0x23201,Words({2,f.block,0x01000000}));Append(sources,0x23202,records);
    Data root;Append(root,0x80023000,map);Append(root,0x80023300,graph);Append(root,0x80023200,sources);f.metadata=Wrap(0x80000001,root);return f;
}
inline Data Calculation()
{
    Data section,records(72);Append(section,0x23401,Words({3,0xf1000100,0}));
    for(unsigned i=0;i<3;++i){Put(records,i*24,i);Put(records,i*24+4,0x100+i);}
    Append(section,0x23402,records);return Wrap(0x80000001,Wrap(0x80023400,section));
}
}
