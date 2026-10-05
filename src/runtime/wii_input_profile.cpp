#include "runtime/wii_input_profile.h"
#include "NL/plat/WiiPadSteps.h"
#include <cmath>
#include <stdexcept>
namespace mscharged
{
namespace
{
unsigned Count(WiiDeviceProfile profile)
{
    switch(profile)
    {
    case WiiDeviceProfile::Remote:return 11;
    case WiiDeviceProfile::Freestyle:return 13;
    case WiiDeviceProfile::Classic:return 16;
    }
    throw std::invalid_argument("Unknown Wii input profile");
}
void HasStick(WiiDeviceProfile profile)
{
    Count(profile);
    if(profile==WiiDeviceProfile::Remote)throw std::invalid_argument("Wii Remote profile has no analog stick");
}
bool Nonzero(WiiRawStick stick){return stick.x||stick.y;}
}
std::uint16_t WiiProfileButtonMask(WiiDeviceProfile profile,unsigned index)
{
    if(index>=Count(profile))throw std::out_of_range("Button index is outside the selected Wii profile");
    return static_cast<std::uint16_t>(profile==WiiDeviceProfile::Classic?
        WiiClassicButtonMask(static_cast<int>(index)):GetWiiButtonMask(static_cast<int>(index)));
}
unsigned WiiProfileButtonIndex(WiiDeviceProfile profile,std::uint16_t button)
{
    const auto count=Count(profile);
    if(!button||(button&(button-1)))throw std::invalid_argument("Wii button lookup requires a single button mask");
    const int index=profile==WiiDeviceProfile::Classic?WiiClassicButtonIndex(button):GetWiiButtonIndex(button);
    if(index<0||static_cast<unsigned>(index)>=count||WiiProfileButtonMask(profile,static_cast<unsigned>(index))!=button)
        throw std::invalid_argument("Button mask is outside the selected Wii profile");
    return static_cast<unsigned>(index);
}
WiiStickValue ClampWiiProfileStick(WiiDeviceProfile profile,WiiRawStick raw)
{
    HasStick(profile);signed char x=raw.x,y=raw.y;ClampWiiStick(&x,&y);
    return {{x,y},WiiNormalizeClampedStick(x),WiiNormalizeClampedStick(y)};
}
std::uint16_t MapWiiProfileDPad(WiiDeviceProfile profile,float x,float y)
{
    HasStick(profile);
    if(!std::isfinite(x)||!std::isfinite(y)||std::abs(x)>1||std::abs(y)>1)
        throw std::invalid_argument("Wii normalized stick requires finite components in [-1,1]");
    return profile==WiiDeviceProfile::Classic?WiiClassicStickDPad(x,y):WiiFreestyleStickDPad(x,y);
}
WiiControlValue MapWiiControls(WiiDeviceProfile profile,const WiiControlSample& sample,bool map)
{
    const auto count=Count(profile);std::uint16_t allowed=0;
    for(unsigned i=0;i<count;++i)allowed|=WiiProfileButtonMask(profile,i);
    if(sample.buttons&~allowed)throw std::invalid_argument("Wii sample contains bits absent from its device profile");
    if(profile==WiiDeviceProfile::Remote&&(Nonzero(sample.left)||Nonzero(sample.right)||map))
        throw std::invalid_argument("Wii Remote cannot supply stick input or analog mapping");
    if(profile==WiiDeviceProfile::Freestyle&&Nonzero(sample.right))
        throw std::invalid_argument("Freestyle has only one analog stick");
    WiiControlValue result;result.buttons=sample.buttons;
    if(profile!=WiiDeviceProfile::Remote)result.left=ClampWiiProfileStick(profile,sample.left);
    if(profile==WiiDeviceProfile::Classic)result.right=ClampWiiProfileStick(profile,sample.right);
    if(map)result.buttons|=MapWiiProfileDPad(profile,result.left.x,result.left.y);
    return result;
}
}
