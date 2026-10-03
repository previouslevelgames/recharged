#include "resources/compressed_asset.h"
#include <iostream>
#include <stdexcept>
using namespace mscharged::resources;
int main()
{
    unsigned checks=0;
    try
    {
        // Python zlib encoding of "hello"; no encoder under test constructs this fixture.
        std::vector<std::uint8_t> data={0,0,0,5,0x78,0x9c,0xcb,0x48,0xcd,0xc9,0xc9,0x07,0x00,0x06,0x2c,0x02,0x15};
        if(InflateAsset(data)!=std::vector<std::uint8_t>{'h','e','l','l','o'})throw std::runtime_error("Exact decompressed bytes");
        ++checks;
        auto reject=[&](Bytes bytes){try{InflateAsset(bytes);}catch(const std::runtime_error&){++checks;return;}throw std::runtime_error("Malformed compressed asset accepted");};
        for(std::size_t n=0;n<data.size();++n)reject(Bytes(data).first(n));
        for(unsigned size:{0u,4u,6u,0x1000001u,0xffffffffu})
        {
            auto bad=data;for(unsigned i=0;i<4;++i)bad[i]=size>>(24-8*i);reject(bad);
        }
        auto bad=data;bad.back()^=1;reject(bad); // Adler checksum.
        bad=data;bad.push_back(0);reject(bad); // Trailing byte.
        bad=data;bad.insert(bad.end(),data.begin()+4,data.end());reject(bad); // Concatenated streams.
        bad=data;bad[4]=0;reject(bad); // Invalid zlib header.
        std::cout<<checks<<" bounded compressed asset checks passed\n";
    }
    catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
