#pragma once
#include "resources/chunk_reader.h"
#include <bit>
#include <map>
#include <vector>
namespace nis_pip_fixture
{
namespace resources = mscharged::resources;
using Blob = std::vector<std::uint8_t>;
inline void Word(Blob& data, std::uint32_t word) { for (int n : {24,16,8,0}) data.push_back(word>>n); }
inline void Set(Blob& data, std::size_t at, std::uint32_t word) { for (int n : {24,16,8,0}) data.at(at++)=word>>n; }
inline void CameraChunk(Blob& data, unsigned count, float base, float target=0)
{
    const auto begin=data.size(); Word(data,0x8002500b);Word(data,0);
    std::map<unsigned,Blob> channels;
    channels[0x25000]={'n','i','s',0};Word(channels[0x2500c],count);
    for(unsigned i=0;i<count;++i)
    {
        for(float v:{base+i*.01f,2.f,3.f})Word(channels[0x25003],std::bit_cast<unsigned>(v));
        for(float v:{target,0.f,0.f})Word(channels[0x25006],std::bit_cast<unsigned>(v));
        for(float v:{0.f,0.f,0.f,1.f})Word(channels[0x25004],std::bit_cast<unsigned>(v));
        Word(channels[0x25009],std::bit_cast<unsigned>(45.f+i));Word(channels[0x2500a],std::bit_cast<unsigned>(4.f+i));
    }
    for(const auto& [id,payload]:channels)
    {
        Word(data,id|0x05000000);const auto size=data.size();Word(data,0);data.resize(resources::Align(data.size(),32));
        data.insert(data.end(),payload.begin(),payload.end());Set(data,size,data.size()-size-4);
    }
    Set(data,begin+4,data.size()-begin-8);
}
inline Blob Fixture(float target=0)
{
    Blob data;CameraChunk(data,30,10,target);CameraChunk(data,60,20,target);return data;
}
}
