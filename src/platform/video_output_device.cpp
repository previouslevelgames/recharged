#include "platform/video_output_device.h"
#include "platform/system.h"
#include "platform/interrupt_controller.h"
#include "platform/interrupts.h"

#include <aurora/video.h>
#include <stdexcept>

namespace mscharged::platform {
void ConfigureNativeVideoOutputHardware(const NativeSystemSettings& settings, NativeDesktopPicture picture) {
    NativeInterruptGuard exclusion;
    const auto controller = GetNativeInterruptControllerStatus();
    if (!controller.initialized || !controller.owner_thread)
        throw std::logic_error("native VI output requires the initialized owner interrupt controller");
    if (settings.aspect_ratio > 1)
        throw std::invalid_argument("native VI output aspect record is outside the original SC enum");
    const AuroraVIOutputConfig output{settings.aspect_ratio == 1,
        static_cast<std::uint8_t>(picture == NativeDesktopPicture::Sharp ? AURORA_DESKTOP_SHARP
                                  : picture == NativeDesktopPicture::Clean ? AURORA_DESKTOP_SMOOTH
                                                                           : AURORA_DESKTOP_SIGNAL)};
    aurora_configure_video_output_hardware(&output);
}
} // namespace mscharged::platform
