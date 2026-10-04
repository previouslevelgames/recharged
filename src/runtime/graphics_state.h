#pragma once

namespace mscharged
{
// Select the original state/matrix stages after graphics memory is available.
// Views, targets and material programs have separate session owners; full
// original glStartup remains pending.
void InitializeOriginalGraphicsState();
// Exact glStartup stage order; unlike the standalone helper above, it does not
// select a current identity matrix or apply per-draw default-state overrides.
void InitializeOriginalGraphicsStartupState();
}
