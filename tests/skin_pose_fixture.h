#pragma once
#include "animation_pose_fixture.h"
namespace skin_fixture
{
using namespace animation_pose_fixture;
inline std::array<float,16> Matrix(float scale,float x,float y,float z)
{return {scale,0,0,0,0,scale,0,0,0,0,scale,0,x,y,z,1};}
inline void Floats(Blob& out,const auto& values){for(float value:values)Word(out,std::bit_cast<std::uint32_t>(value));}
inline Blob Rlg(unsigned lane=0)
{
 Blob params,indices,vertices,streams,packets,matrices,models,skin,binds;
 for(unsigned i=0;i<2;++i){Word(params,0x12345678+i);Half(params,0xffff);params.push_back(0);params.push_back(0);}
 Word(params,0xdeadbeef);Word(params,0xfedcba98);Floats(params,std::array{1.f,1.f});Word(params,0xffffffff);Word(params,1);
 const std::array<unsigned,6> ids{1,2,4,4,7,5},strides{12,12,4,4,4,16};
 for(unsigned s=0;s<6;++s)
 {
  Word(streams,vertices.size());streams.push_back(0xa0+s);streams.push_back(strides[s]);streams.push_back(ids[s]);streams.push_back(0);
  for(unsigned v=0;v<3;++v)
  {
   if(s==0)Floats(vertices,std::array{float(v+1),float(v)-2,3.f});
   else if(s==1)Floats(vertices,std::array{0.f,1.f,0.f});
   else if(s<4){Half(vertices,v*1024);Half(vertices,-512+int(v)*1024);}
   else if(s==4)for(unsigned w=0;w<4;++w)vertices.push_back(w==lane?v%2:255);
   else for(unsigned w=0;w<4;++w)Word(vertices,std::bit_cast<std::uint32_t>(w==lane?1.f:0.f));
  }
 }
 for(unsigned v:{0u,1u,2u})Half(indices,v);
 packets.resize(48);Set(packets,4,3);packets[9]=3;packets[10]=0;packets[11]=6;Set(packets,16,0x041c3281);Set(packets,28,0x11002233);
 Floats(matrices,Matrix(1,0,0,0));Word(models,0x99f34aae);Word(models,1);Word(models,0xcdcdcdcd);
 Chunk(skin,0x1b00b,Words({0x123000,0x123001}));
 Word(binds,0x123000);Floats(binds,Matrix(2,4,2,-6));Word(binds,0x123001);Floats(binds,Matrix(1,0,3,0));
 Chunk(skin,0x1b00a,binds);Chunk(skin,0x1b00c,Words({0,16,1}));
 Blob body;
 for(const auto& pair:std::array{std::pair{0x1b016u,params},std::pair{0x1b007u,indices},std::pair{0x1b006u,vertices},std::pair{0x1b005u,streams},std::pair{0x1b004u,packets},std::pair{0x1b002u,matrices},std::pair{0x1b003u,models},std::pair{0x8001b008u,skin}})Chunk(body,pair.first,pair.second);
 Blob out;Chunk(out,0x8001b000,body);return out;
}
inline std::size_t Find(Blob& bytes,unsigned kind,std::size_t start=0,std::size_t end=SIZE_MAX)
{
 if(end==SIZE_MAX)end=bytes.size();auto u=[&](std::size_t p){return unsigned(bytes.at(p))<<24|unsigned(bytes.at(p+1))<<16|unsigned(bytes.at(p+2))<<8|bytes.at(p+3);};
 for(auto p=start;p<end;){const auto id=u(p),size=u(p+4);if(id==kind)return p+8;if(id&0x80000000){auto found=Find(bytes,kind,p+8,p+8+size);if(found!=SIZE_MAX)return found;}p=(p+8+size+3)&~std::size_t(3);}return SIZE_MAX;
}
}
