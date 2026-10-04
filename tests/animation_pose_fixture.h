#pragma once
#include "pose_accumulator_fixture.h"
#include <string_view>
namespace animation_pose_fixture
{
using namespace pose_fixture;
inline Blob Words(std::initializer_list<std::uint32_t> values){Blob b;for(auto v:values)Word(b,v);return b;}
inline void Half(Blob& b,std::uint16_t v){b.push_back(v>>8);b.push_back(v);}
inline std::uint32_t Hash(std::string_view s){std::uint32_t h=~0u;for(unsigned char c:s)h=h*33+c;return h;}
inline Blob Rig(unsigned n,bool preserved=false,bool mirror=false,std::string_view name="rig")
{
 std::array<Blob,11> c;
 for(unsigned i=0;i<13;++i)Word(c[0],0xfedcba98);
 Set(c[0],4,Hash(name));Set(c[0],8,n);Set(c[0],36,-1);Set(c[0],40,-1);
 c[1].assign(name.begin(),name.end());c[1].push_back(0);while(c[1].size()%4)c[1].push_back(0);
 for(unsigned i=0;i<n;++i)
 {
  Word(c[2],0x123000+i);Word(c[3],i?0:-1);Word(c[4],i?0:n-1);Word(c[5],0xdeadbeef);Word(c[6],0xfedcba98);
  if(i)Word(c[7],i);Word(c[8],mirror?n-1-i:i);
  for(float v:{float(i+1),float(i+2),float(i+3)})Word(c[9],std::bit_cast<std::uint32_t>(v));
  c[10].push_back(preserved&&i==n-1?1:0);
 }
 const unsigned ids[]{0x18001,0x18002,0x18003,0x18009,0x18004,0x18005,0x18006,0x18007,0x18008,0x18010,0x18011};
 Blob body;for(unsigned i=0;i<11;++i)Chunk(body,ids[i],c[i]);Blob out;Chunk(out,0x80018000,body);return out;
}
struct Node
{
 unsigned properties=0;
 std::vector<std::array<float,4>> rotation;
 std::vector<unsigned short> angles;
 std::vector<std::array<float,3>> scale,translation;
};
inline Blob Animation(unsigned frames,const std::vector<Node>& nodes,unsigned signature=0xabcd,unsigned id=1,bool morph=false)
{
 Blob body,header;for(unsigned i=0;i<22;++i)Word(header,0xfedcba98);
 Set(header,4,id);Set(header,8,frames);Set(header,12,nodes.size());Set(header,16,morph);Set(header,52,0);Set(header,84,signature);
 Chunk(body,0x17001,header);Chunk(body,0x17002,{'k','e','y',0});
 Blob zero(nodes.size()*4),ptrs(nodes.size()*4,0xce);Chunk(body,0x17110,zero);Chunk(body,0x17113,zero);
 for(unsigned kind:{0x17004,0x17005,0x17006,0x17111,0x17114})Chunk(body,kind,ptrs);
 Chunk(body,0x17007,{});Chunk(body,0x17008,{});
 for(const auto& node:nodes)
 {
  Blob payload,rot,scale,trans;
  if(node.properties&1)for(auto v:node.angles)Half(rot,v);
  else for(auto q:node.rotation)
  {
   if(node.properties&0x10)for(float v:q)Half(rot,std::uint16_t(std::int16_t(v*32768)));
   else if(node.properties&0x20)
   {
    for(unsigned pair=0;pair<2;++pair)
    {
     const auto a=std::uint16_t(std::int16_t(q[2*pair]*2048))&4095,b=std::uint16_t(std::int16_t(q[2*pair+1]*2048))&4095;
     rot.push_back(a>>4);rot.push_back((a&15)*16+(b&15));rot.push_back(b>>4);
    }
   }
   else for(float v:q)rot.push_back(std::uint8_t(std::int8_t(v*128)));
  }
  for(auto key:node.scale)for(float v:key)Half(scale,std::uint16_t(v*2048));
  for(auto key:node.translation)for(float v:key)Word(trans,std::bit_cast<std::uint32_t>(v));
  if(!rot.empty())Chunk(payload,0x17101,rot);if(!trans.empty())Chunk(payload,0x17102,trans);if(!scale.empty())Chunk(payload,0x17103,scale);
  Chunk(body,0x80017100,payload);
 }
 Chunk(body,0x17009,morph?Words({1}):Blob{});Chunk(body,0x1700a,morph?Words({0xabc}):Blob{});Chunk(body,0x1700b,morph?Blob{127}:Blob{});
 Blob props;for(const auto& n:nodes)Word(props,n.properties);Chunk(body,0x17003,props);
 Blob out;Chunk(out,0x80017000,body);return out;
}
inline Blob World(const Blob& data){Blob out;Chunk(out,0x80000001,data);return out;}
inline Blob Retarget(const std::vector<std::pair<unsigned,std::vector<int>>>& maps)
{
 Blob body,group,records;
 Chunk(body,0x17105,Words({0,0x1357,unsigned(maps.size()),0}));
 for(auto& [signature,indices]:maps)for(auto v:{signature,0u,unsigned(indices.size()),0u})Word(records,v);
 Chunk(group,0x17107,records);
 for(auto& [signature,indices]:maps){Blob data;for(int i:indices)Half(data,std::uint16_t(i));while(data.size()%4)data.push_back(0xfe);Chunk(group,0x17108,data);}
 Chunk(body,0x80017106,group);Blob out;Chunk(out,0x80017104,body);return out;
}
inline void Append(Blob& target,const Blob& bytes){target.insert(target.end(),bytes.begin(),bytes.end());}
}
