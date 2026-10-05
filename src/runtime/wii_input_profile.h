#pragma once
#include <cstdint>
namespace mscharged
{
// Pure original device policy. Selecting one is NOT a connection, original
// PadBackend class ID, WPAD/KPAD registration or a motion/IR/HOME service.
// DesktopPad remains a separate class -1 with its own .5 menu mapping.
enum class WiiDeviceProfile { Remote, Freestyle, Classic };
struct WiiRawStick { std::int8_t x=0,y=0; };
struct WiiStickValue
{
    WiiRawStick clamped;
    float x=0,y=0;
};
struct WiiControlSample
{
    std::uint16_t buttons=0; // Actual device's serialized button layout.
    WiiRawStick left,right; // Calibrated original signed8 inputs, not SDL axes.
};
struct WiiControlValue
{
    std::uint16_t buttons=0;
    WiiStickValue left,right;
};
// No default device selection. Missing axes/unknown bits fail before output.
// Original PadBackend starts with analog-to-DPad disabled; caller opts in.
WiiControlValue MapWiiControls(WiiDeviceProfile,const WiiControlSample&,
    bool map_left_stick_to_dpad=false);
WiiStickValue ClampWiiProfileStick(WiiDeviceProfile,WiiRawStick);
// For already normalized original samples; each component must be finite[-1,1].
// Original .6 filtering, fixed-angle conversion and sector arithmetic are used.
std::uint16_t MapWiiProfileDPad(WiiDeviceProfile,float x,float y);
// Single actual button only; these are not frontend action/remap indices.
// Remote has11 buttons, Freestyle13, Classic16. Original raw helper fallback
// semantics remain untouched; this checked boundary rejects unknown masks.
unsigned WiiProfileButtonIndex(WiiDeviceProfile,std::uint16_t single_button);
std::uint16_t WiiProfileButtonMask(WiiDeviceProfile,unsigned index);
}
