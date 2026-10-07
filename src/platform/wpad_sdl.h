#pragma once

#include "platform/desktop_dpd.h"

#include <cstddef>
#include <cstdint>

namespace mscharged::platform {

// Supply host system preferences before original KPADInit requests WPADInit.
// These are the Wii sensor-bar (0=bottom, 1=top) and sensitivity (1..5) values.
struct WpadSDLSettings {
    std::uint8_t sensor_bar_position;
    std::uint8_t dpd_sensitivity;
    // Selected host profiles may defer physical remotes while retaining their
    // existing default discovery policy for all other callers.
    bool physical_wii_remotes = true;
};
void ConfigureWpadSDL(WpadSDLSettings settings);

// Call from the SDL owner while servicing host events. Bare Wii Remote and
// explicitly registered desktop virtual-device reports use this raw provider.
// Original connect/sampling callbacks run on this owner through the shared IRQ
// boundary. No callbacks are executed by an SDL watcher or device worker.
void ServiceWpadSDL();
std::size_t WpadSDLConnectedChannels();

} // namespace mscharged::platform
