#pragma once

#include "platform/filesystem_device.h"

namespace mscharged::platform {

// Use the actual InitializeNativeFilesystemForDisc result after the same disc
// has been mounted by aurora_dvd_open. This registers native OS boot metadata;
// it does not change any source reset selector, title state, or ready flag.
void ConfigureNativeOSDiscBootIdentity(const NativeFilesystemSettings& boot);

// Retain this copied boot identity through all original source calls. Removing
// or resetting the physical medium does not change the running application's
// original app-type byte. Retire explicitly after the source instance stops.
void RetireNativeOSDiscBootIdentity();

} // namespace mscharged::platform
