#pragma once

namespace mscharged { struct NativeSystemSettings; }

#include <cstdint>

namespace mscharged::platform {
// Desktop picture (display.picture), host presentation only: Signal stretches
// the VI signal into the window like a TV; Clean and Sharp draw the XFB into
// the window in one pass without the display copy's flicker filter.
enum class NativeDesktopPicture : std::uint8_t { Signal, Clean, Sharp };
// Stage native scanout before original module/static construction and VIInit.
// Pass the same explicit system records supplied to the SC endpoint. This
// function transports aspect only; original SC requests select render modes.
void ConfigureNativeVideoOutputHardware(const NativeSystemSettings& settings,
    NativeDesktopPicture picture = NativeDesktopPicture::Signal);
}
