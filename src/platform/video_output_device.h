#pragma once

namespace mscharged { struct NativeSystemSettings; }

namespace mscharged::platform {
// Stage native scanout before original module/static construction and VIInit.
// Pass the same explicit system records supplied to the SC endpoint. This
// function transports aspect only; original SC requests select render modes.
void ConfigureNativeVideoOutputHardware(const NativeSystemSettings& settings);
}
