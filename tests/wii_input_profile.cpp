#include "runtime/wii_input_profile.h"
#include "NL/plat/WiiPadSteps.h"
#include "NL/nlMath.h"
#include "NL/nlMath.inl"
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>
using namespace mscharged;
namespace
{
std::uint64_t checks=0;
void Check(bool value,const char* what){++checks;if(!value)throw std::runtime_error(what);}
template<class F>void Reject(F&& f){++checks;try{f();}catch(const std::exception&){return;}throw std::runtime_error("Unsupported Wii profile input accepted");}
constexpr std::array<std::uint16_t,13> masks{0x8000,0x1000,0x800,0x400,0x200,0x100,0x10,8,4,2,1,0x2000,0x4000};
// Independent integer deadzone and correctly-rounded libm reference, without
// original reciprocal-square-root/Newton code. Preserve each source float
// rounding before its final truncation; an ideal-real circle is not that code.
WiiRawStick Oracle(int x,int y)
{
    const auto zone=[](int v){return v>15?v-15:v<-15?v+15:0;};x=zone(x);y=zone(y);
    const int squared=x*x+y*y;
    if(squared>3136)
    {
        const float length=static_cast<float>(std::sqrt(static_cast<double>(squared)));
        const float factor=56.f/length;
        x=static_cast<int>(static_cast<float>(x)*factor);y=static_cast<int>(static_cast<float>(y)*factor);
    }
    return {static_cast<std::int8_t>(x),static_cast<std::int8_t>(y)};
}
std::uint16_t Sector(unsigned angle,WiiDeviceProfile profile)
{
    // 360/65536 is exactly45/8192: all unsigned16 products by45 fit in
    // binary32 exactly. The original two truncations therefore equal >>13.
    constexpr std::array<std::uint16_t,8> free{2,10,8,9,1,5,4,6};
    constexpr std::array<std::uint16_t,8> classic{0x8000,0x8001,1,3,2,0x4002,0x4000,0xc000};
    return (profile==WiiDeviceProfile::Classic?classic:free)[(angle&65535)>>13];
}
std::uint16_t Expected(float x,float y,WiiDeviceProfile profile)
{
    const bool keep_x=std::abs(x)>=.6f,keep_y=std::abs(y)>=.6f;
    if(!keep_x&&!keep_y)return 0;
    // This oracle intentionally uses the selected original angle provider;
    // the independently expressed mapping below qualifies source sector and
    // profile policy, not a claim of new Wii atan arithmetic equivalence.
    return Sector(nlATan2Angle(keep_y?y:0.f,keep_x?x:0.f),profile);
}
void Buttons()
{
    for(auto profile:{WiiDeviceProfile::Remote,WiiDeviceProfile::Freestyle,WiiDeviceProfile::Classic})
    {
        const unsigned count=profile==WiiDeviceProfile::Remote?11:profile==WiiDeviceProfile::Freestyle?13:16;
        for(unsigned i=0;i<count;++i)
        {
            const auto wanted=profile==WiiDeviceProfile::Classic?std::uint16_t(1u<<i):masks[i];
            Check(WiiProfileButtonMask(profile,i)==wanted&&WiiProfileButtonIndex(profile,wanted)==i,"Original button permutation differs");
        }
        Reject([&]{WiiProfileButtonMask(profile,count);});Reject([&]{WiiProfileButtonMask(profile,~0u);});
        for(unsigned bits=0;bits<65536;++bits)
        {
            bool valid=false;for(unsigned i=0;i<count;++i)valid|=bits==(profile==WiiDeviceProfile::Classic?1u<<i:masks[i]);
            if(valid)Check(WiiProfileButtonIndex(profile,static_cast<std::uint16_t>(bits))<count,"Valid native button rejected");
            else Reject([&]{WiiProfileButtonIndex(profile,static_cast<std::uint16_t>(bits));});
        }
    }
    for(int value:{-123,-1,13,17,32,0xdead,0xffff})
    {
        int expected=value;for(unsigned i=0;i<masks.size();++i)if(value==masks[i])expected=static_cast<int>(i);
        Check(GetWiiButtonIndex(value)==expected,"Original raw index fallback changed");
        Check(GetWiiButtonMask(value)==value,"Original raw mask fallback changed");
    }
    Check(WiiClassicButtonIndex(0)==0&&WiiClassicButtonIndex(3)==0&&WiiClassicButtonMask(-1)==0
        &&WiiClassicButtonMask(16)==0,"Original Classic defaults changed");
}
void Axes()
{
    for(int x=-128;x<128;++x)for(int y=-128;y<128;++y)
    {
        const WiiRawStick raw{static_cast<std::int8_t>(x),static_cast<std::int8_t>(y)};const auto expected=Oracle(x,y);
        for(auto profile:{WiiDeviceProfile::Freestyle,WiiDeviceProfile::Classic})
        {
            const auto actual=ClampWiiProfileStick(profile,raw);
            Check(actual.clamped.x==expected.x&&actual.clamped.y==expected.y,"Original signed8 clamp differs from independent sqrt oracle");
            Check(std::bit_cast<std::uint32_t>(actual.x)==std::bit_cast<std::uint32_t>(static_cast<float>(static_cast<long double>(expected.x)/56))
                &&std::bit_cast<std::uint32_t>(actual.y)==std::bit_cast<std::uint32_t>(static_cast<float>(static_cast<long double>(expected.y)/56)),"Original normalized stick bits differ");
            Check(int(actual.clamped.x)*actual.clamped.x+int(actual.clamped.y)*actual.clamped.y<=3136,"Clamped stick exceeds original radius");
            Check(MapWiiProfileDPad(profile,actual.x,actual.y)==Expected(actual.x,actual.y,profile),"Clamped stick DPad mapping differs");
        }
    }
}
void Directions()
{
    for(unsigned angle=0;angle<65536;++angle)
    {
        const double radians=double(angle)*6.2831853071795864769/65536.;
        const float x=static_cast<float>(std::cos(radians)),y=static_cast<float>(std::sin(radians));
        for(auto profile:{WiiDeviceProfile::Freestyle,WiiDeviceProfile::Classic})
            Check(MapWiiProfileDPad(profile,x,y)==Expected(x,y,profile),"Full normalized-angle sweep changed source sector policy");
    }
    const float below=std::nextafter(.6f,0.f),above=std::nextafter(.6f,1.f);
    for(float x:{-1.f,-above,-.6f,-below,-0.f,0.f,below,.6f,above,1.f})
        for(float y:{-1.f,-above,-.6f,-below,-0.f,0.f,below,.6f,above,1.f})
            for(auto profile:{WiiDeviceProfile::Freestyle,WiiDeviceProfile::Classic})
                Check(MapWiiProfileDPad(profile,x,y)==Expected(x,y,profile),"Threshold/filtered-axis boundary differs");
    Check(MapWiiProfileDPad(WiiDeviceProfile::Freestyle,below,below)==0,"Subthreshold axes must both disappear");
    Check(MapWiiProfileDPad(WiiDeviceProfile::Freestyle,.6f,0)==2&&MapWiiProfileDPad(WiiDeviceProfile::Classic,.6f,0)==0x8000,"Inclusive .6 threshold/profile masks differ");
    // Source LUT atan differs from ideal atan near diagonal boundaries. These
    // masks preserve its positive-octant overshoot and complementary undershoot.
    Check(MapWiiProfileDPad(WiiDeviceProfile::Freestyle,1,1)==10
        &&MapWiiProfileDPad(WiiDeviceProfile::Freestyle,-1,1)==8
        &&MapWiiProfileDPad(WiiDeviceProfile::Freestyle,-1,-1)==5
        &&MapWiiProfileDPad(WiiDeviceProfile::Freestyle,1,-1)==4,"Original diagonal-sector asymmetry changed");
    for(float bad:{std::numeric_limits<float>::quiet_NaN(),std::numeric_limits<float>::infinity(),-std::numeric_limits<float>::infinity(),1.001f,-1.001f})
    {Reject([&]{MapWiiProfileDPad(WiiDeviceProfile::Freestyle,bad,0);});Reject([&]{MapWiiProfileDPad(WiiDeviceProfile::Classic,0,bad);});}
}
void Profiles()
{
    for(auto invalid:{static_cast<WiiDeviceProfile>(-1),static_cast<WiiDeviceProfile>(3)})
    {Reject([&]{MapWiiControls(invalid,{});});Reject([&]{WiiProfileButtonMask(invalid,0);});Reject([&]{ClampWiiProfileStick(invalid,{});});Reject([&]{MapWiiProfileDPad(invalid,0,0);});}
    Check(MapWiiControls(WiiDeviceProfile::Remote,{0x8000,{},{}}).buttons==0x8000,"Remote serialized bits changed");
    Reject([]{ClampWiiProfileStick(WiiDeviceProfile::Remote,{});});Reject([]{MapWiiProfileDPad(WiiDeviceProfile::Remote,0,0);});
    Reject([]{MapWiiControls(WiiDeviceProfile::Remote,{},true);});Reject([]{MapWiiControls(WiiDeviceProfile::Remote,{0,{1,0},{}});});
    Reject([]{MapWiiControls(WiiDeviceProfile::Remote,{0x2000,{},{}});});Reject([]{MapWiiControls(WiiDeviceProfile::Freestyle,{0x20,{},{}});});
    Reject([]{MapWiiControls(WiiDeviceProfile::Freestyle,{0,{},{1,0}});});
    const WiiControlSample raw{0x100, {127,0}, {}};
    Check(MapWiiControls(WiiDeviceProfile::Freestyle,raw).buttons==0x100,"Default mapping must remain disabled");
    Check(MapWiiControls(WiiDeviceProfile::Freestyle,raw,true).buttons==0x102,"Freestyle physical and emulated bits not combined");
    const auto classic=MapWiiControls(WiiDeviceProfile::Classic,{4,{127,0},{0,127}},true);
    Check(classic.buttons==0x8004&&classic.right.x==0&&classic.right.y>0,"Classic right stick incorrectly mapped to DPad");
    Check(MapWiiControls(WiiDeviceProfile::Freestyle,{0,{43,0},{}},true).buttons==0,"Native .5 policy leaked into Wii .6 profile");
}
}
int main()
{
    try{Buttons();Axes();Directions();Profiles();std::cout<<checks<<" original Wii profile checks passed\n";}
    catch(const std::exception& e){std::cerr<<"FAILED: "<<e.what()<<" (check "<<checks<<")\n";return 1;}
}
