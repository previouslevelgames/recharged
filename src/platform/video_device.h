#pragma once

#include <cstdint>

namespace mscharged::platform {

// After real host SDK/controller setup, before original module constructors.
// This stages explicit virtual-hardware preferences and the genuine PI_VI
// endpoint. Original source VIInit installs/unmasks its line and starts video.
void ConfigureNativeVideoHardware(std::uint32_t boot_tv_mode, bool dtv_cable);

} // namespace mscharged::platform
