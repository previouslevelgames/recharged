#include "resources/native_preferences.h"
#include "Game/DB/SaveStateSteps.h"
#include <algorithm>
#include <bit>
#include <cmath>
#include <stdexcept>

namespace mscharged::resources
{
namespace
{
constexpr std::array<std::uint8_t,8> magic{'M','S','C','P','R','E','F',0};
void Check(bool value,const char* message) { if(!value) throw std::runtime_error(message); }
std::uint32_t Word(std::span<const std::uint8_t> b,std::size_t p)
{
    return (std::uint32_t(b[p])<<24)|(std::uint32_t(b[p+1])<<16)|(std::uint32_t(b[p+2])<<8)|b[p+3];
}
void Put(NativePreferencesBytes& b,std::size_t p,std::uint32_t v)
{
    b[p]=v>>24;b[p+1]=v>>16;b[p+2]=v>>8;b[p+3]=v;
}
// Native format CRC-32/ISO-HDLC. The original checksum has the same polynomial,
// but this byte implementation never reads native unsigned long or packed data.
std::uint32_t Crc(std::span<const std::uint8_t> b)
{
    std::uint32_t c=0xffffffffu;
    for(auto x:b){c^=x;for(unsigned i=0;i<8;++i)c=(c>>1)^((c&1)?0xedb88320u:0);}
    return ~c;
}
}
NativePreferencesValues DefaultNativePreferences()
{
    struct Audio { int MusicVolume,SFXVolume,VoiceVolume,DefaultMusicVolume,DefaultSFXVolume,DefaultVoiceVolume; } a{};
    struct Visual { bool mIsAutoZoomCamera; float mCameraZoomLevel; } v{};
    SaveAudioDefaults(a);SaveVisualDefaults(v);
    return {{a.MusicVolume,a.SFXVolume,a.VoiceVolume},{a.DefaultMusicVolume,a.DefaultSFXVolume,a.DefaultVoiceVolume},v.mIsAutoZoomCamera,v.mCameraZoomLevel};
}
void ValidateNativePreferences(const NativePreferencesValues& v)
{
    for(int n:v.audio)Check(n>=0&&n<=10,"Native audio setting is outside 0..10");
    for(int n:v.audio_defaults)Check(n>=0&&n<=10,"Native audio default is outside 0..10");
    Check(std::isfinite(v.camera_zoom)&&v.camera_zoom>=0&&v.camera_zoom<=1,"Native camera zoom is outside 0..1");
}
NativePreferencesBytes EncodeNativePreferences(const NativePreferencesValues& v)
{
    ValidateNativePreferences(v);NativePreferencesBytes b{};
    std::copy(magic.begin(),magic.end(),b.begin());Put(b,8,1);Put(b,12,b.size());Put(b,20,v.auto_zoom?1:0);
    for(unsigned i=0;i<3;++i){Put(b,24+i*4,v.audio[i]);Put(b,36+i*4,v.audio_defaults[i]);}
    Put(b,48,std::bit_cast<std::uint32_t>(v.camera_zoom));Put(b,16,Crc(std::span(b).subspan(20)));return b;
}
NativePreferencesValues DecodeNativePreferences(std::span<const std::uint8_t> b)
{
    Check(b.size()==64,"Native preferences must contain exactly 64 bytes");
    Check(std::equal(magic.begin(),magic.end(),b.begin()),"Not a native preferences file");
    Check(Word(b,8)==1,"Unsupported native preferences version");Check(Word(b,12)==64,"Invalid native preferences length");
    Check(Word(b,16)==Crc(b.subspan(20)),"Native preferences checksum differs");
    Check(Word(b,20)<=1,"Unknown native preferences flags");
    for(unsigned i=52;i<64;++i)Check(b[i]==0,"Unknown native preferences reserved data");
    NativePreferencesValues v;
    for(unsigned i=0;i<3;++i)
    {
        const auto current=Word(b,24+i*4),initial=Word(b,36+i*4);
        Check(current<=10&&initial<=10,"Native audio setting is outside 0..10");
        v.audio[i]=static_cast<int>(current);v.audio_defaults[i]=static_cast<int>(initial);
    }
    v.auto_zoom=Word(b,20)!=0;v.camera_zoom=std::bit_cast<float>(Word(b,48));ValidateNativePreferences(v);return v;
}
}
