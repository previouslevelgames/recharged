#pragma once
#include "platform/rtc_device.h"

namespace mscharged::platform {
// Explicit first-run policy for an owned native compatibility device. This is
// not a Wii SRAM dump, installed firmware, locale or a wall-clock setting.
// Existing backing remains authoritative; InitializeNativeRTC ignores this
// initial image when the backing already exists.
constexpr NativeRTCImage CreateVirginRTCImage() {
    NativeRTCImage image{};
    // Original UnlockSram sums four BE16 words at offsets 12..19. For this
    // virgin zero record the sum is 0 and the complement sum is 4*FFFF=FFFC.
    image.sram[2] = 0xff;
    image.sram[3] = 0xfc;
    // This native device has produced no RTC wake/event condition. The actual
    // opaque flags register starts at 0; future reads/clears use real EXI work.
    return image;
}
} // namespace mscharged::platform
