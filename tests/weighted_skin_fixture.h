#pragma once
#include "animation_pose_fixture.h"
namespace weighted_skin_fixture
{
using namespace animation_pose_fixture;
inline void Floats(Blob& b,const auto& values){for(float x:values)Word(b,std::bit_cast<std::uint32_t>(x));}
inline Blob Model()
{
 Blob params,indices,vertices,streams,packets,matrices,models,skin,binds;
 for(unsigned i=0;i<3;++i){Word(params,0x12345000+i);Half(params,0x8765);params.push_back(i&3);params.push_back(0);}
 Word(params,0xdeadbeef);Word(params,0xfedcba98);Floats(params,std::array{.75f,.875f,.5f,16.f,1.f,.5f,.25f,1.f});Word(params,0xffffffff);Word(params,1);
 const std::array<unsigned,7> ids{1,2,4,4,4,7,5},strides{12,12,4,4,4,4,16};
 const std::array<std::array<unsigned char,4>,4> bones{{{0,1,2,255},{2,1,255,255},{255,255,3,255},{0,1,2,3}}};
 const std::array<std::array<float,4>,4> weights{{{.2f,.3f,.5f,0},{.5f,.5f,0,0},{0,0,1,0},{.25f,.25f,.25f,.25f}}};
 for(unsigned s=0;s<7;++s)
 {
  Word(streams,vertices.size());streams.push_back(0xd0+s);streams.push_back(strides[s]);streams.push_back(ids[s]);streams.push_back(0);
  for(unsigned v=0;v<4;++v)
  {
   if(s==0)Floats(vertices,std::array{float(v+1),float(v)-2,3.f});
   else if(s==1)Floats(vertices,std::array{0.f,1.f,0.f});
   else if(s<5){Half(vertices,v*1024+s);Half(vertices,-512+int(v)*1024-int(s));}
   else if(s==5)for(auto x:bones[v])vertices.push_back(x);
   else Floats(vertices,weights[v]);
  }
 }
 for(unsigned v:{0u,1u,2u})Half(indices,v);
 packets.resize(48);Set(packets,4,3);packets[9]=4;packets[10]=0;packets[11]=7;Set(packets,16,0x22cadb20);Set(packets,28,0x11002233);
 std::array<float,16> identity{};identity[0]=identity[5]=identity[10]=identity[15]=1;
 Floats(matrices,identity);Word(models,0x01020304);Word(models,1);Word(models,0xcdcdcdcd);
 Chunk(skin,0x1b00b,Words({0x123000,0x123001,0x123002,0x123003}));
 for(unsigned n=0;n<4;++n){Word(binds,0x123000+n);auto m=identity;m[0]=m[5]=m[10]=2;m[12]=n*2;m[13]=4;m[14]=-6;Floats(binds,m);}
 Chunk(skin,0x1b00a,binds);Chunk(skin,0x1b00c,Words({0,16,1}));
 Blob body;
 for(const auto& pair:std::array{std::pair{0x1b016u,params},std::pair{0x1b007u,indices},std::pair{0x1b006u,vertices},std::pair{0x1b005u,streams},std::pair{0x1b004u,packets},std::pair{0x1b002u,matrices},std::pair{0x1b003u,models},std::pair{0x8001b008u,skin}})Chunk(body,pair.first,pair.second);
 Blob out;Chunk(out,0x8001b000,body);return out;
}
inline std::size_t Find(const Blob& b,unsigned wanted,std::size_t begin=0,std::size_t end=SIZE_MAX)
{
 if(end==SIZE_MAX)end=b.size();auto u=[&](std::size_t p){return unsigned(b.at(p))<<24|unsigned(b.at(p+1))<<16|unsigned(b.at(p+2))<<8|b.at(p+3);};
 for(auto p=begin;p<end;){const auto kind=u(p),size=u(p+4);if(kind==wanted)return p+8;if(kind&0x80000000){auto n=Find(b,wanted,p+8,p+8+size);if(n!=SIZE_MAX)return n;}p=(p+8+size+3)&~std::size_t(3);}return SIZE_MAX;
}
}
