#pragma once
#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <vector>
namespace pose_fixture
{
using Blob=std::vector<std::uint8_t>;
inline void Word(Blob& b,std::uint32_t v){for(int s:{24,16,8,0})b.push_back(v>>s);}
inline void Set(Blob& b,std::size_t at,std::uint32_t v){for(int s:{24,16,8,0})b.at(at++)=v>>s;}
inline void Chunk(Blob& b,unsigned kind,const Blob& data)
{Word(b,kind);Word(b,data.size());b.insert(b.end(),data.begin(),data.end());while(b.size()%4)b.push_back(0);}
inline Blob Hierarchy(const std::vector<int>& parents,const std::vector<bool>& preserve={},int pelvis=-1,int spine=-1,
                      const std::vector<std::uint32_t>& ids={})
{
 const auto n=parents.size();std::array<Blob,11> c;
 for(unsigned i=0;i<13;++i)Word(c[0],0xdead0000+i*4);
 Set(c[0],4,0x80736511);Set(c[0],8,n);Set(c[0],36,pelvis);Set(c[0],40,spine);
 c[1]={'p','o','s','e',0,0,0,0};
 for(unsigned i=0;i<n;++i)
 {
  Word(c[2],ids.empty()?0x50100000+i:ids.at(i));Word(c[3],parents[i]);Word(c[4],std::count(parents.begin(),parents.end(),int(i)));
  Word(c[5],0xfefefefe);Word(c[6],0xcdcdcdcd);
  for(unsigned j=0;j<n;++j)if(parents[j]==int(i))Word(c[7],j);
  Word(c[8],i);
  for(float v:{float(i)*.125f+.25f,float(i%3)*.25f-.5f,float(i%5)*.0625f+.125f})Word(c[9],std::bit_cast<std::uint32_t>(v));
  c[10].push_back(!preserve.empty()&&preserve[i]?255:0);
 }
 const unsigned kinds[]{0x18001,0x18002,0x18003,0x18009,0x18004,0x18005,0x18006,0x18007,0x18008,0x18010,0x18011};
 Blob body;for(unsigned i=0;i<11;++i)Chunk(body,kinds[i],c[i]);Blob result;Chunk(result,0x80018000,body);return result;
}
}
