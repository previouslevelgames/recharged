#include "platform/ax_studio_depop.h"
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <vector>
namespace {
unsigned checks{};
void Check(bool value,const char* reason) {++checks;if(!value)throw std::runtime_error(reason);}
std::uint16_t Half(const unsigned char* p) {return std::uint16_t(p[0])|(std::uint16_t(p[1])<<8);}
std::uint32_t Word(const unsigned char* p) {return std::uint32_t(Half(p))|(std::uint32_t(Half(p+2))<<16);}
std::int32_t S32(std::uint32_t bits) {std::int32_t x;std::memcpy(&x,&bits,4);return x;}
std::int16_t S16(std::uint16_t bits) {std::int16_t x;std::memcpy(&x,&bits,2);return x;}
}
int main(int argc,char** argv) {
    using namespace mscharged::platform;
    try {
        Check(argc==2,"need independent selected owned Setup oracle");
        std::ifstream input(argv[1],std::ios::binary);
        Check(bool(input),"owned Setup oracle missing");
        const std::vector<unsigned char> bytes{std::istreambuf_iterator<char>(input),{}};
        Check(bytes.size()>=12&&std::memcmp(bytes.data(),"AXDP399\0",8)==0,"owned Setup header differs");
        const auto cases=Word(bytes.data()+8);
        Check(cases==24&&bytes.size()==12+cases*(20*6+(12*96+8*18)*4),"owned Setup geometry differs");
        std::size_t at=12;
        for(unsigned c=0;c<cases;++c)for(unsigned record=0;record<20;++record) {
            const auto value=S32(Word(bytes.data()+at));const auto delta=S16(Half(bytes.data()+at+4));at+=6;
            if(record<12) {
                const auto output=NativeAXStudioDepop96(value,delta);
                for(const auto sample:output) {Check(sample==S32(Word(bytes.data()+at)),"main96 depop differs from owned Setup instructions");at+=4;}
            } else {
                const auto output=NativeAXStudioDepop18(value,delta);
                for(const auto sample:output) {Check(sample==S32(Word(bytes.data()+at)),"remote18 arithmetic differs from owned Setup instructions");at+=4;}
            }
        }
        Check(at==bytes.size(),"owned Setup oracle has unconsumed bytes");
        for(const auto sample:NativeAXStudioDepop96(0,32767))Check(sample==0,"owned zero-fill quirk changed");
        for(const auto sample:NativeAXStudioDepop18(0,-32768))Check(sample==0,"remote zero-fill quirk changed");
        std::cout<<"native_ax_depop: "<<checks<<" checks; original20Studio ramps/24 owned instruction cases; remote output/boot/game cue remain separate\n";
        return 0;
    }catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
